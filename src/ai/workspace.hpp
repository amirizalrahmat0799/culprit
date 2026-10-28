#pragma once
// What the AI may look at: the project's files, read-only, through a few tools. Every request goes
// through the checks here, so the rules hold whatever the model asks for:
//
//   - paths must stay inside the project (after following symbolic links)
//   - secrets are never read: .env files, keys and certificates' private parts, credential files, and
//     anything git ignores (local config usually is)
//   - in what is read, values next to "password", "secret", "token", "api_key" ... are replaced with
//     [redacted], and so are things that look like keys (sk-..., ghp_..., AKIA..., private key blocks)
//   - outputs are size-limited
//
// Nothing here writes, deletes or runs anything except `git log` / `git diff` / `git ls-files`.

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace culprit::ai {

class Workspace {
public:
    explicit Workspace(std::string root);
    const std::string& root() const { return root_; }

    // The tools. Each returns text for the model; problems are explained in the text ("error: ...").
    std::string read_file(const std::string& path, int start_line, int end_line);
    std::string search(const std::string& text, const std::string& under);
    std::string list_dir(const std::string& path);
    std::string find_files(const std::string& name);
    std::string git_history(const std::string& path);

    /// A path the model or a stack trace gave, as a project-relative path, if it's allowed and exists.
    /// Absolute paths from another machine or a container (/app/src/server.ts) are matched by their
    /// end; bare file names (OrderService.java) are found if there's exactly one, or one under src/.
    std::optional<std::string> resolve(const std::string& path, std::string* why = nullptr);

    /// The file's lines as the model sees them (redacted), for checking quotes.
    std::optional<std::vector<std::string>> lines(const std::string& relative_path);

    /// Files read so far, project-relative.
    const std::vector<std::string>& files_read() const { return files_read_; }
    /// Top-level entries and the build files found, for the first message.
    std::string overview();

    static bool is_secret(std::string_view file_name);
    /// The line with secret-looking values replaced. `config` files (.properties, .yml, .env ...) have any
    /// value after a secret-looking key replaced; in code only quoted values are.
    static std::string redact(std::string_view line, bool config);

private:
    struct Walk {
        std::vector<std::string> files;  // project-relative, '/' separated
        bool truncated = false;
    };
    const Walk& walk();
    bool ignored(const std::string& rel);
    bool allowed(const std::string& rel, std::string* why);
    std::optional<std::string> inside(const std::string& path);  // absolute or relative -> relative, if inside

    std::string root_;
    std::optional<Walk> walk_;
    std::optional<std::set<std::string>> ignored_;  // from git: files and folders (ending in /)
    std::vector<std::string> files_read_;
    bool git_ = false;
};

/// Runs a program with arguments (no shell), collecting at most `max` bytes of its output.
/// Returns the exit status, or -1 if it couldn't start.
int run_program(const std::vector<std::string>& args, std::string& output, size_t max = 1 << 20);

}  // namespace culprit::ai
