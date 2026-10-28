#pragma once
// Talking to a model: a conversation in, a reply (text and/or tool calls) out. Two wire formats cover
// every provider: OpenAI's chat completions (Ollama, Groq, OpenRouter, Cerebras, Gemini, OpenAI, Mistral,
// DeepSeek, company servers) and Anthropic's messages API (Claude).

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "ai/config.hpp"
#include "ai/http.hpp"
#include "ai/json.hpp"

namespace culprit::ai {

struct ToolCall {
    std::string id;
    std::string name;
    Json arguments;  // an object
};

struct Message {
    enum class Role { User, Assistant, Tool };
    Role role = Role::User;
    std::string text;
    std::vector<ToolCall> calls;  // Assistant: the tools it asked for
    std::string call_id;          // Tool: which call this answers
    std::string tool_name;        // Tool
};

struct ToolSpec {
    std::string name;
    std::string description;
    Json parameters;  // JSON Schema of the arguments
};

struct Reply {
    bool ok = false;
    std::string error;  // when !ok: what went wrong, in words
    std::string text;
    std::vector<ToolCall> calls;
    long input_tokens = 0;
    long output_tokens = 0;
};

class Model {
public:
    virtual ~Model() = default;
    virtual Reply chat(const std::string& system, const std::vector<Message>& messages, const std::vector<ToolSpec>& tools,
                       const std::atomic<bool>* cancel) = 0;
    /// The models this provider offers (for setup). Empty, with `error`, when it can't say.
    virtual std::vector<std::string> list_models(std::string* error) = 0;
};

/// A client for `settings`. The transport must outlive it.
std::unique_ptr<Model> make_model(const Settings& settings, Transport& transport);

/// Finds a tool call written as text ({"name": ..., "arguments": {...}}), which small local models
/// sometimes produce instead of a real tool call. Exposed for tests.
bool tool_call_from_text(const std::string& text, const std::vector<ToolSpec>& tools, ToolCall& call);

}  // namespace culprit::ai
