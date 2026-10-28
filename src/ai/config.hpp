#pragma once
// AI settings. Nothing uses AI until `culprit ai setup` writes ~/.config/culprit/ai.toml:
//
//   enabled     = true
//   provider    = "ollama"                 # ollama, openai (any OpenAI-compatible API), anthropic
//   name        = "Ollama"                 # shown on screen
//   url         = "http://localhost:11434/v1"
//   model       = "qwen3:8b"
//   api_key_env = ""                       # the environment variable holding the key; never the key itself
//   max_steps   = 12
//   timeout     = 180                      # seconds per answer
//   confirm     = true                     # ask before sending anything to a provider that isn't local
//
// AI stays off, whatever ai.toml says, when:
//   --no-ai is given, CULPRIT_AI=off is set, or the project's .culprit/config.toml has `ai = false`.

#include <optional>
#include <string>
#include <vector>

namespace culprit::ai {

enum class ProviderKind { OpenAI, Anthropic };

struct Preset {
    const char* id;          // "groq"
    const char* name;        // "Groq"
    ProviderKind kind;
    const char* url;
    const char* key_env;     // "" for none
    const char* note;        // shown in setup
    const char* model_hint;  // a model to suggest when the provider can't list them
};

/// Ollama, the free cloud tiers, and the paid APIs, in the order setup shows them.
const std::vector<Preset>& presets();
const Preset* find_preset(const std::string& id);

struct Settings {
    bool enabled = false;
    std::string provider = "ollama";  // a preset id, or "openai" / "anthropic" for your own URL
    std::string name = "Ollama";
    std::string url = "http://localhost:11434/v1";
    std::string model;
    std::string api_key_env;
    int max_steps = 12;
    int timeout = 180;
    bool confirm = true;

    ProviderKind kind() const;
    bool local() const;            // the URL is this computer: nothing leaves it
    std::string host() const;      // "api.groq.com"
    std::string api_key() const;   // from api_key_env; empty when unset
};

/// Where ai.toml and the user's rules live: ~/.config/culprit (%APPDATA%\culprit on Windows).
std::string config_dir();
std::string settings_path();

/// Reads ai.toml; nullopt when there is none. Problems go to `error`.
std::optional<Settings> load_settings(std::string* error = nullptr);
/// Writes ai.toml (creating the folder). Returns an error message, or empty on success.
std::string save_settings(const Settings& s);

/// Whether AI may be used here, and why not when it can't.
struct Availability {
    bool allowed = false;
    std::string reason;  // when not allowed: what turned it off, and how to turn it on
    Settings settings;
};
/// `no_ai_flag`: --no-ai was given. `project_start`: where to look for .culprit/config.toml.
Availability availability(bool no_ai_flag, const std::string& project_start);

/// The project's root: the nearest folder above `start` with .git or .culprit, else `start` itself.
std::string find_project_root(const std::string& start);

}  // namespace culprit::ai
