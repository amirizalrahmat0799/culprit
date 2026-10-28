#include "ai/workspace.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace culprit::ai {
namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxFileBytes = 2 * 1024 * 1024;  // bigger files are generated or data, not code
constexpr size_t kMaxWalkFiles = 30000;
constexpr size_t kMaxToolOutput = 14000;           // characters per tool answer
constexpr int kMaxReadLines = 250;

char fold(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = fold(c);
    return out;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool skipped_dir(std::string_view name) {
    static const std::array<std::string_view, 22> dirs = {
        ".git", "node_modules", "target", "build", "dist", "out", ".gradle", ".idea", ".vscode", ".venv", "venv",
        "__pycache__", ".mypy_cache", ".pytest_cache", "vendor", "bin", "obj", ".next", "coverage", ".terraform",
        ".ssh", ".aws"};
    if (std::find(dirs.begin(), dirs.end(), name) != dirs.end()) return true;
    return name.rfind("cmake-build-", 0) == 0 || name == ".gnupg";
}

bool config_like(std::string_view file) {
    std::string f = lower(file);
    for (std::string_view ext : {".properties", ".yml", ".yaml", ".toml", ".ini", ".conf", ".cfg", ".env", ".json", ".xml"}) {
        if (ends_with(f, ext)) return true;
    }
    return f.find(".env") != std::string::npos;
}

std::string clip(std::string s, size_t max = kMaxToolOutput) {
    if (s.size() <= max) return s;
    s.resize(max);
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
    if (!s.empty() && (static_cast<unsigned char>(s.back()) & 0x80)) s.pop_back();
    return s + "\n[... cut: the answer was too long; ask for a smaller range]";
}

std::optional<std::string> slurp(const fs::path& p) {
    std::error_code ec;
    auto size = fs::file_size(p, ec);
    if (ec || size > kMaxFileBytes) return std::nullopt;
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    if (s.substr(0, 8192).find('\0') != std::string::npos) return std::nullopt;  // binary
    return s;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        std::string line = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(std::move(line));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    if (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

// things that look like credentials wherever they appear
void redact_tokens(std::string& s) {
    static const std::array<std::string_view, 9> prefixes = {"sk-", "sk_live_", "ghp_", "gho_", "github_pat_", "xoxb-",
                                                              "xoxp-", "AKIA", "AIza"};
    for (std::string_view p : prefixes) {
        size_t at = 0;
        while ((at = s.find(p, at)) != std::string::npos) {
            bool word_start = at == 0 || !(std::isalnum(static_cast<unsigned char>(s[at - 1])) || s[at - 1] == '_' || s[at - 1] == '-');
            size_t end = at + p.size();
            while (end < s.size() && (std::isalnum(static_cast<unsigned char>(s[end])) || s[end] == '_' || s[end] == '-')) ++end;
            if (word_start && end - at >= p.size() + 12) {
                s.replace(at, end - at, "[redacted]");
                at += 10;
            } else {
                at = end;
            }
        }
    }
}

#ifdef _WIN32
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
// quoting for CreateProcess, following the rules the C runtime uses to split the command line
std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += c;
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            out += c;
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, L'\\');
    return out + L"\"";
}
#endif

}  // namespace

int run_program(const std::vector<std::string>& args, std::string& output, size_t max) {
    output.clear();
    if (args.empty()) return -1;
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return -1;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = null_in;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    std::wstring cmd;
    for (const auto& a : args) cmd += (cmd.empty() ? L"" : L" ") + quote_arg(wide(a));
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(write_end);
    if (null_in != INVALID_HANDLE_VALUE) CloseHandle(null_in);
    if (!ok) {
        CloseHandle(read_end);
        return -1;
    }
    char buf[8192];
    DWORD n = 0;
    while (ReadFile(read_end, buf, sizeof buf, &n, nullptr) && n > 0) {
        if (output.size() < max) output.append(buf, std::min<size_t>(n, max - output.size()));
    }
    CloseHandle(read_end);
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
#else
    int fds[2];
    if (pipe(fds) != 0) return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", 0, 0);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        return -1;
    }
    char buf[8192];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) {
        if (output.size() < max) output.append(buf, std::min<size_t>(static_cast<size_t>(n), max - output.size()));
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

Workspace::Workspace(std::string root) {
    std::error_code ec;
    root_ = fs::weakly_canonical(fs::absolute(root, ec), ec).string();
    std::string out;
    git_ = fs::exists(fs::path(root_) / ".git", ec) && run_program({"git", "--version"}, out, 256) == 0;
}

bool Workspace::is_secret(std::string_view file_name) {
    std::string f = lower(file_name);
    size_t slash = f.find_last_of("/\\");
    std::string name = slash == std::string::npos ? f : f.substr(slash + 1);
    if (name == ".env" || (name.rfind(".env.", 0) == 0 && !ends_with(name, ".example") && !ends_with(name, ".sample") &&
                           !ends_with(name, ".template"))) {
        return true;
    }
    for (std::string_view ext : {".pem", ".key", ".p12", ".pfx", ".jks", ".keystore", ".kdbx", ".asc", ".gpg", ".ppk"}) {
        if (ends_with(name, ext)) return true;
    }
    for (std::string_view exact : {".npmrc", ".pypirc", ".netrc", ".git-credentials", ".dockercfg", "credentials",
                                   "credentials.json", "service-account.json", "terraform.tfstate", "terraform.tfvars"}) {
        if (name == exact) return true;
    }
    if (name.rfind("id_rsa", 0) == 0 || name.rfind("id_ed25519", 0) == 0 || name.rfind("id_ecdsa", 0) == 0) return true;
    // secrets.yaml, app-secret.json, k8s secret manifests; not SecretService.java
    if (name.find("secret") != std::string::npos || name.find("credential") != std::string::npos) {
        return !(ends_with(name, ".java") || ends_with(name, ".kt") || ends_with(name, ".ts") || ends_with(name, ".js") ||
                 ends_with(name, ".py") || ends_with(name, ".go") || ends_with(name, ".cs") || ends_with(name, ".rb") ||
                 ends_with(name, ".php") || ends_with(name, ".cpp") || ends_with(name, ".hpp") || ends_with(name, ".rs"));
    }
    for (std::string_view dir : {"/.ssh/", "/.aws/", "/.gnupg/", "/.docker/"}) {
        if (("/" + f).find(dir) != std::string::npos) return true;
    }
    return false;
}

std::string Workspace::redact(std::string_view line, bool config) {
    std::string s(line);
    std::string l = lower(s);
    static const std::array<std::string_view, 13> keys = {"password", "passwd", "pwd", "secret", "token", "apikey", "api_key",
                                                          "api-key", "private_key", "private-key", "credential",
                                                          "access_key", "auth"};
    if (l.find("-----begin") != std::string::npos && l.find("private key") != std::string::npos) return "[redacted private key]";
    for (std::string_view k : keys) {
        size_t at = l.find(k);
        if (at == std::string::npos) continue;
        // a separator soon after the key: password=..., "token": "...", api-key: ...
        size_t sep = l.find_first_of(":=", at + k.size());
        if (sep == std::string::npos || sep > at + k.size() + 24) continue;
        size_t v = sep + 1;
        while (v < s.size() && (s[v] == ' ' || s[v] == '\t')) ++v;
        if (v >= s.size()) continue;
        if (s[v] == '"' || s[v] == '\'') {
            char q = s[v];
            size_t close = s.find(q, v + 1);
            if (close == std::string::npos) close = s.size();
            if (close > v + 1) s.replace(v + 1, close - v - 1, "[redacted]");
        } else if (config) {
            // the rest of the value, up to a comment or the end
            size_t end = s.find(" #", v);
            if (end == std::string::npos) end = s.size();
            while (end > v && (s[end - 1] == ',' || s[end - 1] == ' ')) --end;
            if (end > v && s.compare(v, 2, "${") != 0) s.replace(v, end - v, "[redacted]");  // ${ENV} references stay
        }
        break;
    }
    redact_tokens(s);
    return s;
}

std::optional<std::string> Workspace::inside(const std::string& path) {
    std::error_code ec;
    fs::path p = fs::path(path);
    fs::path abs = p.is_absolute() ? p : fs::path(root_) / p;
    fs::path canon = fs::weakly_canonical(abs, ec);
    if (ec) return std::nullopt;
    fs::path rel = canon.lexically_relative(root_);
    std::string r = rel.generic_string();
    if (r.empty() || r == "." ) return std::string(".");
    if (r.rfind("..", 0) == 0 || rel.is_absolute()) return std::nullopt;
    return r;
}

const Workspace::Walk& Workspace::walk() {
    if (walk_) return *walk_;
    Walk w;
    std::error_code ec;
    fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end && !ec; it.increment(ec)) {
        const auto& entry = *it;
        std::string name = entry.path().filename().string();
        if (entry.is_directory(ec)) {
            if (skipped_dir(name) || entry.is_symlink(ec)) it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(ec)) continue;
        if (w.files.size() >= kMaxWalkFiles) {
            w.truncated = true;
            break;
        }
        w.files.push_back(entry.path().lexically_relative(root_).generic_string());
    }
    std::sort(w.files.begin(), w.files.end());
    walk_ = std::move(w);
    return *walk_;
}

bool Workspace::ignored(const std::string& rel) {
    if (!git_) return false;
    if (!ignored_) {
        ignored_.emplace();
        std::string out;
        if (run_program({"git", "-C", root_, "ls-files", "--others", "--ignored", "--exclude-standard", "--directory"}, out, 8 << 20) == 0) {
            std::istringstream ss(out);
            std::string line;
            while (std::getline(ss, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) ignored_->insert(line);
            }
        }
    }
    if (ignored_->count(rel)) return true;
    // inside an ignored folder
    for (size_t slash = rel.find('/'); slash != std::string::npos; slash = rel.find('/', slash + 1)) {
        if (ignored_->count(rel.substr(0, slash + 1))) return true;
    }
    return false;
}

bool Workspace::allowed(const std::string& rel, std::string* why) {
    if (is_secret(rel)) {
        if (why) *why = rel + " may hold secrets, so culprit doesn't read it";
        return false;
    }
    if (rel == ".git" || rel.rfind(".git/", 0) == 0) {
        if (why) *why = "the .git folder isn't readable; use git_history";
        return false;
    }
    if (ignored(rel)) {
        if (why) *why = rel + " is ignored by git (usually local settings or generated files), so culprit doesn't read it";
        return false;
    }
    return true;
}

std::optional<std::string> Workspace::resolve(const std::string& path, std::string* why) {
    std::string p = path;
    while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    if (p.rfind("file://", 0) == 0) p.erase(0, 7);
    if (p.empty() || p == ".") return std::string(".");
    std::error_code ec;

    if (auto rel = inside(p)) {
        if (fs::exists(fs::path(root_) / *rel, ec)) {
            if (!allowed(*rel, why)) return std::nullopt;
            return rel;
        }
    } else if (!fs::path(p).has_root_path()) {
        // a relative path that climbs out of the project. (A path from another machine starts at a root:
        // /app/src/a.ts from a Linux container isn't "absolute" on Windows, which wants a drive letter,
        // but it's matched by its tail below just the same.)
        if (why) *why = path + " is outside the project";
        return std::nullopt;
    }

    // a path from somewhere else: match the longest tail that exists here (/app/src/a.ts -> src/a.ts)
    std::string generic = fs::path(p).generic_string();
    for (size_t slash = generic.find('/'); slash != std::string::npos; slash = generic.find('/', slash + 1)) {
        std::string tail = generic.substr(slash + 1);
        if (tail.empty()) continue;
        if (auto rel = inside(tail); rel && fs::is_regular_file(fs::path(root_) / *rel, ec)) {
            if (!allowed(*rel, why)) return std::nullopt;
            return rel;
        }
    }

    // a bare name or partial path: OrderService.java, kedai/order/OrderService.java
    std::vector<std::string> matches;
    std::string want = "/" + generic;
    for (const auto& f : walk().files) {
        if (f == generic || ends_with("/" + f, want)) matches.push_back(f);
    }
    if (matches.size() > 1) {
        // prefer main sources over tests and generated copies
        std::vector<std::string> main;
        for (const auto& m : matches) {
            if (m.find("test") == std::string::npos) main.push_back(m);
        }
        if (main.size() == 1) matches = main;
    }
    if (matches.size() == 1) {
        if (!allowed(matches[0], why)) return std::nullopt;
        return matches[0];
    }
    if (why) {
        if (matches.empty()) *why = "no file " + path + " in the project";
        else {
            *why = path + " matches several files:";
            for (size_t i = 0; i < matches.size() && i < 10; ++i) *why += "\n  " + matches[i];
        }
    }
    return std::nullopt;
}

std::optional<std::vector<std::string>> Workspace::lines(const std::string& rel) {
    std::string why;
    auto r = resolve(rel, &why);
    if (!r) return std::nullopt;
    auto text = slurp(fs::path(root_) / *r);
    if (!text) return std::nullopt;
    auto ls = split_lines(*text);
    const bool config = config_like(*r);
    for (auto& l : ls) l = redact(l, config);
    return ls;
}

std::string Workspace::read_file(const std::string& path, int start, int end) {
    // refused by name, whether or not it exists: the model learns nothing about which secrets there are
    if (is_secret(path)) return "error: " + path + " may hold secrets, so culprit doesn't read it";
    std::string why;
    auto rel = resolve(path, &why);
    if (!rel) return "error: " + why;
    std::error_code ec;
    if (fs::is_directory(fs::path(root_) / *rel, ec)) return "error: " + *rel + " is a folder; use list_dir";
    auto text = slurp(fs::path(root_) / *rel);
    if (!text) return "error: " + *rel + " is binary or too big to read";
    auto ls = split_lines(*text);
    const bool config = config_like(*rel);
    int total = static_cast<int>(ls.size());
    if (start < 1) start = 1;
    if (end < start) end = start + kMaxReadLines - 1;
    if (end - start + 1 > kMaxReadLines) end = start + kMaxReadLines - 1;
    if (end > total) end = total;
    if (std::find(files_read_.begin(), files_read_.end(), *rel) == files_read_.end()) files_read_.push_back(*rel);
    std::string out = *rel + " (lines " + std::to_string(start) + "-" + std::to_string(end) + " of " + std::to_string(total) + ")\n";
    for (int i = start; i <= end; ++i) {
        out += std::to_string(i) + ": " + redact(ls[static_cast<size_t>(i - 1)], config) + "\n";
    }
    if (end < total) out += "[" + std::to_string(total - end) + " more lines; ask for start_line " + std::to_string(end + 1) + "]\n";
    return clip(out);
}

std::string Workspace::search(const std::string& text, const std::string& under) {
    if (text.size() < 2) return "error: search for at least 2 characters";
    std::string prefix;
    if (!under.empty() && under != ".") {
        std::string why;
        auto rel = resolve(under, &why);
        if (!rel) return "error: " + why;
        prefix = *rel == "." ? "" : *rel + "/";
    }
    std::string needle = lower(text);
    std::string out;
    size_t hits = 0;
    const Walk& w = walk();
    for (const auto& f : w.files) {
        if (!prefix.empty() && f.rfind(prefix, 0) != 0) continue;
        if (!allowed(f, nullptr)) continue;
        auto content = slurp(fs::path(root_) / f);
        if (!content) continue;
        if (lower(*content).find(needle) == std::string::npos) continue;
        auto ls = split_lines(*content);
        const bool config = config_like(f);
        for (size_t i = 0; i < ls.size(); ++i) {
            if (lower(ls[i]).find(needle) == std::string::npos) continue;
            if (++hits > 40) break;
            std::string line = redact(ls[i], config);
            size_t first = line.find_first_not_of(" \t");
            line = first == std::string::npos ? "" : line.substr(first);
            if (line.size() > 200) line = line.substr(0, 200) + "...";
            out += f + ":" + std::to_string(i + 1) + ": " + line + "\n";
        }
        if (hits > 40) break;
    }
    if (hits == 0) return "no matches for \"" + text + "\"" + (prefix.empty() ? "" : " in " + prefix);
    if (hits > 40) out += "[more matches; search for something more specific]\n";
    return clip(out);
}

std::string Workspace::list_dir(const std::string& path) {
    std::string why;
    auto rel = resolve(path.empty() ? "." : path, &why);
    if (!rel) return "error: " + why;
    fs::path dir = fs::path(root_) / *rel;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return "error: " + *rel + " isn't a folder";
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string name = e.path().filename().string();
        std::string child = *rel == "." ? name : *rel + "/" + name;
        bool is_dir = e.is_directory(ec);
        if (name == ".git") continue;
        if (is_dir && skipped_dir(name)) {
            names.push_back(name + "/  (skipped: dependencies or build output)");
        } else if (!is_dir && is_secret(child)) {
            names.push_back(name + "  (not readable: may hold secrets)");
        } else if (ignored(is_dir ? child + "/" : child) || ignored(child)) {
            names.push_back(name + (is_dir ? "/" : "") + "  (ignored by git)");
        } else {
            names.push_back(name + (is_dir ? "/" : ""));
        }
        if (names.size() > 300) break;
    }
    std::sort(names.begin(), names.end());
    std::string out = (*rel == "." ? std::string("(project root)") : *rel) + "\n";
    for (const auto& n : names) out += "  " + n + "\n";
    return clip(out);
}

std::string Workspace::find_files(const std::string& name) {
    if (name.empty()) return "error: give a file name or part of a path";
    std::string want = lower(name);
    std::string out;
    size_t n = 0;
    for (const auto& f : walk().files) {
        std::string lf = lower(f);
        size_t slash = lf.rfind('/');
        std::string base = slash == std::string::npos ? lf : lf.substr(slash + 1);
        if (base == want || ends_with(lf, "/" + want) || lf == want || (want.size() >= 3 && base.find(want) != std::string::npos)) {
            if (!allowed(f, nullptr)) continue;
            out += f + "\n";
            if (++n >= 40) {
                out += "[more; be more specific]\n";
                break;
            }
        }
    }
    return n ? out : "no files matching \"" + name + "\"";
}

std::string Workspace::git_history(const std::string& path) {
    if (!git_) return "error: the project isn't a git repository (or git isn't installed)";
    std::string rel;
    if (!path.empty() && path != ".") {
        std::string why;
        auto r = resolve(path, &why);
        if (!r) return "error: " + why;
        rel = *r;
    }
    std::string out, part;
    // uncommitted changes first: they're often the cause
    std::vector<std::string> diff = {"git", "-C", root_, "diff", "--no-color", "--no-ext-diff", "HEAD", "--"};
    if (!rel.empty()) diff.push_back(rel);
    if (run_program(diff, part, 40000) == 0 && !part.empty()) out += "Uncommitted changes:\n" + part + "\n";
    std::vector<std::string> log = {"git", "-C", root_, "log", "-n", rel.empty() ? "10" : "5", "--no-color", "--date=short",
                                    "--format=commit %h %ad %an%n    %s"};
    if (!rel.empty()) {
        log.push_back("-p");
        log.push_back("--");
        log.push_back(rel);
    } else {
        log.push_back("--stat");
    }
    part.clear();
    if (run_program(log, part, 60000) != 0) return "error: git log failed: " + part.substr(0, 300);
    out += part.empty() ? "no commits" + (rel.empty() ? std::string() : " touch " + rel) : "Recent commits:\n" + part;
    // redact, line by line (diffs of config files can contain secrets too)
    std::istringstream ss(out);
    std::string line, redacted;
    while (std::getline(ss, line)) redacted += redact(line, rel.empty() || config_like(rel)) + "\n";
    return clip(redacted);
}

std::string Workspace::overview() {
    const Walk& w = walk();
    std::string out = "Project folder: " + fs::path(root_).filename().string() + " (" + std::to_string(w.files.size()) +
                      (w.truncated ? "+" : "") + " files)\nTop level:";
    std::set<std::string> top;
    for (const auto& f : w.files) {
        size_t slash = f.find('/');
        top.insert(slash == std::string::npos ? f : f.substr(0, slash + 1));
        if (top.size() > 60) break;
    }
    for (const auto& t : top) out += " " + t;
    out += "\nBuild files:";
    size_t builds = 0;
    for (const auto& f : w.files) {
        size_t slash = f.rfind('/');
        std::string base = slash == std::string::npos ? f : f.substr(slash + 1);
        static const std::array<std::string_view, 14> names = {"pom.xml", "build.gradle", "build.gradle.kts", "package.json",
                                                               "go.mod", "Cargo.toml", "pyproject.toml", "requirements.txt",
                                                               "CMakeLists.txt", "docker-compose.yml", "compose.yaml",
                                                               "Dockerfile", "composer.json", "Gemfile"};
        if (std::find(names.begin(), names.end(), base) != names.end() && std::count(f.begin(), f.end(), '/') <= 2) {
            out += " " + f;
            if (++builds >= 15) break;
        }
    }
    if (!builds) out += " (none found)";
    out += git_ ? "\nGit: yes" : "\nGit: no";
    return out;
}

}  // namespace culprit::ai
