#include "ai/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/toml.hpp"

namespace culprit::ai {
namespace {

namespace fs = std::filesystem;

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string toml_string(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out += c;
    }
    return out + "\"";
}

std::optional<std::string> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

const std::vector<Preset>& presets() {
    static const std::vector<Preset> list = {
        {"ollama", "Ollama", ProviderKind::OpenAI, "http://localhost:11434/v1", "",
         "on this computer: free, private, nothing leaves it; needs Ollama and ~8 GB of free memory", "qwen3:8b"},
        {"groq", "Groq", ProviderKind::OpenAI, "https://api.groq.com/openai/v1", "GROQ_API_KEY",
         "free tier, very fast; key at console.groq.com", "llama-3.3-70b-versatile"},
        {"openrouter", "OpenRouter", ProviderKind::OpenAI, "https://openrouter.ai/api/v1", "OPENROUTER_API_KEY",
         "free models (names ending in :free), low daily limit; key at openrouter.ai/keys", "qwen/qwen3-coder:free"},
        {"cerebras", "Cerebras", ProviderKind::OpenAI, "https://api.cerebras.ai/v1", "CEREBRAS_API_KEY",
         "free tier; key at cloud.cerebras.ai", "llama-3.3-70b"},
        {"gemini", "Google Gemini", ProviderKind::OpenAI, "https://generativelanguage.googleapis.com/v1beta/openai",
         "GEMINI_API_KEY", "free tier; prompts may be used for training outside the EU/UK; key at aistudio.google.com",
         "gemini-2.5-flash"},
        {"anthropic", "Anthropic (Claude)", ProviderKind::Anthropic, "https://api.anthropic.com/v1", "ANTHROPIC_API_KEY",
         "paid per use; key at platform.claude.com", ""},
        {"openai", "OpenAI", ProviderKind::OpenAI, "https://api.openai.com/v1", "OPENAI_API_KEY",
         "paid per use; key at platform.openai.com", ""},
        {"mistral", "Mistral", ProviderKind::OpenAI, "https://api.mistral.ai/v1", "MISTRAL_API_KEY",
         "key at console.mistral.ai", "codestral-latest"},
        {"deepseek", "DeepSeek", ProviderKind::OpenAI, "https://api.deepseek.com/v1", "DEEPSEEK_API_KEY",
         "paid per use, low prices; key at platform.deepseek.com", "deepseek-chat"},
    };
    return list;
}

const Preset* find_preset(const std::string& id) {
    for (const auto& p : presets()) {
        if (id == p.id) return &p;
    }
    return nullptr;
}

ProviderKind Settings::kind() const {
    if (provider == "anthropic") return ProviderKind::Anthropic;
    if (const Preset* p = find_preset(provider)) return p->kind;
    return ProviderKind::OpenAI;
}

std::string Settings::host() const {
    std::string h = url;
    size_t scheme = h.find("://");
    if (scheme != std::string::npos) h.erase(0, scheme + 3);
    size_t slash = h.find('/');
    if (slash != std::string::npos) h.resize(slash);
    if (!h.empty() && h.front() == '[') {  // [::1]:11434
        size_t close = h.find(']');
        return h.substr(1, close == std::string::npos ? std::string::npos : close - 1);
    }
    size_t colon = h.rfind(':');
    if (colon != std::string::npos) h.resize(colon);
    return lower(h);
}

bool Settings::local() const {
    std::string h = host();
    return h == "localhost" || h == "127.0.0.1" || h == "::1" || h.rfind("127.", 0) == 0;
}

std::string Settings::api_key() const {
    if (api_key_env.empty()) return {};
    const char* v = std::getenv(api_key_env.c_str());
    return v ? std::string(v) : std::string();
}

std::string config_dir() {
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA")) return (fs::path(appdata) / "culprit").string();
    return "culprit";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) return (fs::path(xdg) / "culprit").string();
    if (const char* home = std::getenv("HOME")) return (fs::path(home) / ".config" / "culprit").string();
    return ".culprit";
#endif
}

std::string settings_path() { return (fs::path(config_dir()) / "ai.toml").string(); }

std::optional<Settings> load_settings(std::string* error) {
    auto text = read_file(settings_path());
    if (!text) return std::nullopt;
    toml::Error err;
    auto doc = toml::parse(*text, err);
    if (!doc) {
        if (error) *error = settings_path() + ":" + std::to_string(err.line) + ": " + err.message;
        return std::nullopt;
    }
    Settings s;
    const toml::Table& t = doc->root;
    auto str = [&](const char* key, std::string& out) {
        if (const auto* v = t.get(key); v && v->type == toml::Value::Type::String) out = v->str;
    };
    auto num = [&](const char* key, int& out) {
        if (const auto* v = t.get(key); v && v->type == toml::Value::Type::Integer) out = static_cast<int>(v->integer);
    };
    auto flag = [&](const char* key, bool& out) {
        if (const auto* v = t.get(key); v && v->type == toml::Value::Type::Bool) out = v->boolean;
    };
    flag("enabled", s.enabled);
    str("provider", s.provider);
    str("name", s.name);
    str("url", s.url);
    str("model", s.model);
    str("api_key_env", s.api_key_env);
    num("max_steps", s.max_steps);
    num("timeout", s.timeout);
    flag("confirm", s.confirm);
    if (const Preset* p = find_preset(s.provider); p && t.get("url") == nullptr) s.url = p->url;
    while (!s.url.empty() && s.url.back() == '/') s.url.pop_back();
    if (s.max_steps < 1) s.max_steps = 1;
    if (s.max_steps > 40) s.max_steps = 40;
    if (s.timeout < 10) s.timeout = 10;
    return s;
}

std::string save_settings(const Settings& s) {
    std::error_code ec;
    fs::create_directories(config_dir(), ec);
    std::ofstream out(settings_path(), std::ios::binary | std::ios::trunc);
    if (!out) return "can't write " + settings_path();
    out << "# culprit's AI settings, written by `culprit ai setup`. See docs/ai.md.\n"
        << "enabled     = " << (s.enabled ? "true" : "false") << "\n"
        << "provider    = " << toml_string(s.provider) << "\n"
        << "name        = " << toml_string(s.name) << "\n"
        << "url         = " << toml_string(s.url) << "\n"
        << "model       = " << toml_string(s.model) << "\n"
        << "api_key_env = " << toml_string(s.api_key_env) << "   # the key itself is never stored here\n"
        << "max_steps   = " << s.max_steps << "\n"
        << "timeout     = " << s.timeout << "\n"
        << "confirm     = " << (s.confirm ? "true" : "false")
        << "   # ask before sending anything to a provider that isn't on this computer\n";
    return out ? "" : "can't write " + settings_path();
}

std::string find_project_root(const std::string& start) {
    std::error_code ec;
    fs::path dir = fs::weakly_canonical(fs::absolute(start.empty() ? "." : start, ec), ec);
    fs::path p = dir;
    while (!p.empty()) {
        if (fs::exists(p / ".git", ec) || fs::is_directory(p / ".culprit", ec)) return p.string();
        fs::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return dir.string();
}

Availability availability(bool no_ai_flag, const std::string& project_start) {
    Availability a;
    if (no_ai_flag) {
        a.reason = "AI is off for this run (--no-ai)";
        return a;
    }
    if (const char* env = std::getenv("CULPRIT_AI")) {
        std::string v = lower(env);
        if (v == "off" || v == "0" || v == "false" || v == "no") {
            a.reason = "AI is turned off by CULPRIT_AI=" + std::string(env);
            return a;
        }
    }
    // the project can forbid it, whatever the user's settings say
    fs::path project = find_project_root(project_start);
    if (auto text = read_file(project / ".culprit" / "config.toml")) {
        toml::Error err;
        if (auto doc = toml::parse(*text, err)) {
            if (const auto* v = doc->root.get("ai"); v && v->type == toml::Value::Type::Bool && !v->boolean) {
                a.reason = "AI is turned off for this project (ai = false in " +
                           (project / ".culprit" / "config.toml").string() + ")";
                return a;
            }
        }
    }
    std::string error;
    auto s = load_settings(&error);
    if (!s) {
        a.reason = error.empty() ? "AI isn't set up yet. Run: culprit ai setup" : error;
        return a;
    }
    a.settings = *s;
    if (!s->enabled) {
        a.reason = "AI is turned off. Run: culprit ai on";
        return a;
    }
    if (s->model.empty()) {
        a.reason = "No model is chosen. Run: culprit ai setup";
        return a;
    }
    if (!s->api_key_env.empty() && s->api_key().empty()) {
        a.reason = s->name + " needs an API key in the environment variable " + s->api_key_env + ", and it isn't set";
        return a;
    }
    a.allowed = true;
    return a;
}

}  // namespace culprit::ai
