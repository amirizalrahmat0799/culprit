#include "ai/provider.hpp"

#include <algorithm>

namespace culprit::ai {
namespace {

// a short, readable error from an API's error body
std::string api_error(const HttpResponse& r, const Settings& s) {
    if (r.status == 0) {
        std::string msg = "can't reach " + s.name + " at " + s.url + ": " + r.error;
        if (s.provider == "ollama") msg += ". Is Ollama running? Start it, or run: ollama serve";
        return msg;
    }
    std::string detail;
    if (auto j = Json::parse(r.body)) {
        const Json& e = (*j)["error"];
        detail = e.is_string() ? e.str() : e["message"].str();
        if (detail.empty()) detail = (*j)["message"].str();
    }
    if (detail.empty()) detail = r.body.substr(0, 300);
    std::string msg = s.name + " answered " + std::to_string(r.status);
    if (!detail.empty()) msg += ": " + detail;
    if (r.status == 401 || r.status == 403) msg += " (check the API key in " + (s.api_key_env.empty() ? std::string("its variable") : s.api_key_env) + ")";
    if (r.status == 404 && s.provider == "ollama") msg += " (is the model pulled? ollama pull " + s.model + ")";
    if (r.status == 429) msg += " (rate limit: the free tier's limit may be used up; try again later)";
    return msg;
}

Json schema_or_empty(const Json& j) {
    if (j.is_object()) return j;
    return Json::object({{"type", "object"}, {"properties", Json::object()}});
}

// ---- OpenAI-compatible --------------------------------------------------------------------------------

class OpenAIModel : public Model {
public:
    OpenAIModel(Settings s, Transport& t) : s_(std::move(s)), t_(t) {}

    Reply chat(const std::string& system, const std::vector<Message>& messages, const std::vector<ToolSpec>& tools,
               const std::atomic<bool>* cancel) override {
        Json msgs = Json::array();
        msgs.push(Json::object({{"role", "system"}, {"content", system}}));
        for (const auto& m : messages) {
            switch (m.role) {
                case Message::Role::User: msgs.push(Json::object({{"role", "user"}, {"content", m.text}})); break;
                case Message::Role::Assistant: {
                    Json a = Json::object({{"role", "assistant"}, {"content", m.text.empty() ? Json() : Json(m.text)}});
                    if (!m.calls.empty()) {
                        Json calls = Json::array();
                        for (const auto& c : m.calls) {
                            calls.push(Json::object({{"id", c.id},
                                                     {"type", "function"},
                                                     {"function", Json::object({{"name", c.name}, {"arguments", c.arguments.dump()}})}}));
                        }
                        a.set("tool_calls", calls);
                    }
                    msgs.push(a);
                    break;
                }
                case Message::Role::Tool:
                    msgs.push(Json::object(
                        {{"role", "tool"}, {"tool_call_id", m.call_id}, {"name", m.tool_name}, {"content", m.text}}));
                    break;
            }
        }
        Json body = Json::object({{"model", s_.model}, {"messages", msgs}, {"temperature", 0.2}, {"stream", false}});
        if (!tools.empty()) {
            Json ts = Json::array();
            for (const auto& t : tools) {
                ts.push(Json::object({{"type", "function"},
                                      {"function", Json::object({{"name", t.name},
                                                                 {"description", t.description},
                                                                 {"parameters", schema_or_empty(t.parameters)}})}}));
            }
            body.set("tools", ts);
        }

        HttpRequest req;
        req.url = s_.url + "/chat/completions";
        req.headers = {{"Content-Type", "application/json"}};
        if (std::string key = s_.api_key(); !key.empty()) req.headers.emplace_back("Authorization", "Bearer " + key);
        if (s_.provider == "openrouter") {
            req.headers.emplace_back("HTTP-Referer", "https://github.com/amirizalrahmat0799/culprit");
            req.headers.emplace_back("X-Title", "culprit");
        }
        req.body = body.dump();
        req.timeout_seconds = s_.timeout;

        Reply out;
        HttpResponse r = t_.send(req, cancel);
        if (r.status != 200) {
            out.error = r.error == "cancelled" ? "cancelled" : api_error(r, s_);
            return out;
        }
        std::string perr;
        auto j = Json::parse(r.body, &perr);
        if (!j) {
            out.error = s_.name + " sent an answer that isn't JSON (" + perr + ")";
            return out;
        }
        const Json& msg = (*j)["choices"][0]["message"];
        if (!msg.is_object()) {
            out.error = s_.name + " sent no answer" + ((*j)["error"].is_null() ? "" : ": " + (*j)["error"]["message"].str());
            return out;
        }
        out.text = msg["content"].str();
        int n = 0;
        for (const auto& c : msg["tool_calls"].items()) {
            ToolCall call;
            call.id = c["id"].str();
            if (call.id.empty()) call.id = "call_" + std::to_string(++n);
            call.name = c["function"]["name"].str();
            const Json& args = c["function"]["arguments"];
            if (args.is_object()) call.arguments = args;  // Ollama sometimes sends an object
            else if (auto parsed = Json::parse(args.str())) call.arguments = *parsed;
            if (!call.arguments.is_object()) call.arguments = Json::object();
            out.calls.push_back(std::move(call));
        }
        if (out.calls.empty()) {
            ToolCall call;
            if (tool_call_from_text(out.text, tools, call)) {
                out.calls.push_back(std::move(call));
                out.text.clear();
            }
        }
        out.input_tokens = static_cast<long>((*j)["usage"]["prompt_tokens"].integer());
        out.output_tokens = static_cast<long>((*j)["usage"]["completion_tokens"].integer());
        out.ok = true;
        return out;
    }

    std::vector<std::string> list_models(std::string* error) override {
        HttpRequest req;
        req.method = "GET";
        req.url = s_.url + "/models";
        req.timeout_seconds = 20;
        if (std::string key = s_.api_key(); !key.empty()) req.headers.emplace_back("Authorization", "Bearer " + key);
        HttpResponse r = t_.send(req, nullptr);
        std::vector<std::string> out;
        if (r.status != 200) {
            if (error) *error = api_error(r, s_);
            return out;
        }
        if (auto j = Json::parse(r.body)) {
            for (const auto& m : (*j)["data"].items()) {
                if (!m["id"].str().empty()) out.push_back(m["id"].str());
            }
        }
        std::sort(out.begin(), out.end());
        if (out.empty() && error) *error = s_.name + " listed no models";
        return out;
    }

private:
    Settings s_;
    Transport& t_;
};

// ---- Anthropic -------------------------------------------------------------------------------------------

class AnthropicModel : public Model {
public:
    AnthropicModel(Settings s, Transport& t) : s_(std::move(s)), t_(t) {}

    Reply chat(const std::string& system, const std::vector<Message>& messages, const std::vector<ToolSpec>& tools,
               const std::atomic<bool>* cancel) override {
        // tool results go back in a user message; consecutive ones share it
        Json msgs = Json::array();
        Json pending_results = Json::array();
        auto flush_results = [&] {
            if (pending_results.size() == 0) return;
            msgs.push(Json::object({{"role", "user"}, {"content", pending_results}}));
            pending_results = Json::array();
        };
        for (const auto& m : messages) {
            if (m.role == Message::Role::Tool) {
                pending_results.push(Json::object({{"type", "tool_result"}, {"tool_use_id", m.call_id}, {"content", m.text}}));
                continue;
            }
            flush_results();
            if (m.role == Message::Role::User) {
                msgs.push(Json::object({{"role", "user"}, {"content", Json::array({Json::object({{"type", "text"}, {"text", m.text}})})}}));
            } else {
                Json content = Json::array();
                if (!m.text.empty()) content.push(Json::object({{"type", "text"}, {"text", m.text}}));
                for (const auto& c : m.calls) {
                    content.push(Json::object({{"type", "tool_use"}, {"id", c.id}, {"name", c.name}, {"input", c.arguments}}));
                }
                msgs.push(Json::object({{"role", "assistant"}, {"content", content}}));
            }
        }
        flush_results();
        // prompt caching: the system prompt and tools, and everything up to the newest message, are
        // reused on the next step, so they're read from the cache at a fraction of the price
        if (msgs.size() > 0) mark_cache_point(msgs);

        Json ts = Json::array();
        for (size_t i = 0; i < tools.size(); ++i) {
            Json t = Json::object({{"name", tools[i].name},
                                   {"description", tools[i].description},
                                   {"input_schema", schema_or_empty(tools[i].parameters)}});
            if (i + 1 == tools.size()) t.set("cache_control", Json::object({{"type", "ephemeral"}}));
            ts.push(t);
        }
        Json body = Json::object(
            {{"model", s_.model},
             {"max_tokens", 4096},
             {"system", Json::array({Json::object({{"type", "text"}, {"text", system}, {"cache_control", Json::object({{"type", "ephemeral"}})}})})},
             {"messages", msgs}});
        if (!tools.empty()) body.set("tools", ts);

        HttpRequest req;
        req.url = s_.url + "/messages";
        req.headers = {{"Content-Type", "application/json"}, {"anthropic-version", "2023-06-01"}, {"x-api-key", s_.api_key()}};
        req.body = body.dump();
        req.timeout_seconds = s_.timeout;

        Reply out;
        HttpResponse r = t_.send(req, cancel);
        if (r.status != 200) {
            out.error = r.error == "cancelled" ? "cancelled" : api_error(r, s_);
            return out;
        }
        auto j = Json::parse(r.body);
        if (!j) {
            out.error = s_.name + " sent an answer that isn't JSON";
            return out;
        }
        for (const auto& block : (*j)["content"].items()) {
            const std::string& type = block["type"].str();
            if (type == "text") {
                if (!out.text.empty()) out.text += "\n";
                out.text += block["text"].str();
            } else if (type == "tool_use") {
                ToolCall call{block["id"].str(), block["name"].str(), block["input"]};
                if (!call.arguments.is_object()) call.arguments = Json::object();
                out.calls.push_back(std::move(call));
            }
        }
        const Json& usage = (*j)["usage"];
        out.input_tokens = static_cast<long>(usage["input_tokens"].integer() + usage["cache_read_input_tokens"].integer() +
                                             usage["cache_creation_input_tokens"].integer());
        out.output_tokens = static_cast<long>(usage["output_tokens"].integer());
        out.ok = true;
        return out;
    }

    std::vector<std::string> list_models(std::string* error) override {
        HttpRequest req;
        req.method = "GET";
        req.url = s_.url + "/models?limit=100";
        req.timeout_seconds = 20;
        req.headers = {{"anthropic-version", "2023-06-01"}, {"x-api-key", s_.api_key()}};
        HttpResponse r = t_.send(req, nullptr);
        std::vector<std::string> out;
        if (r.status != 200) {
            if (error) *error = api_error(r, s_);
            return out;
        }
        if (auto j = Json::parse(r.body)) {
            for (const auto& m : (*j)["data"].items()) out.push_back(m["id"].str());  // newest first, as listed
        }
        if (out.empty() && error) *error = s_.name + " listed no models";
        return out;
    }

private:
    // cache_control on the last content block of the last message
    static void mark_cache_point(Json& msgs) {
        Json::Array items = msgs.items();
        Json last = items.back();
        Json::Array content = last["content"].items();
        if (content.empty()) return;
        content.back().set("cache_control", Json::object({{"type", "ephemeral"}}));
        last.set("content", Json::array(std::move(content)));
        items.back() = std::move(last);
        msgs = Json::array(std::move(items));
    }

    Settings s_;
    Transport& t_;
};

}  // namespace

bool tool_call_from_text(const std::string& text, const std::vector<ToolSpec>& tools, ToolCall& call) {
    // look for a JSON object with "name" (a known tool) and "arguments"/"parameters"
    for (size_t start = text.find('{'); start != std::string::npos; start = text.find('{', start + 1)) {
        int depth = 0;
        bool in_string = false;
        for (size_t i = start; i < text.size(); ++i) {
            char c = text[i];
            if (in_string) {
                if (c == '\\') ++i;
                else if (c == '"') in_string = false;
                continue;
            }
            if (c == '"') in_string = true;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) {
                auto j = Json::parse(std::string_view(text).substr(start, i - start + 1));
                if (j && (*j)["name"].is_string()) {
                    const std::string& name = (*j)["name"].str();
                    bool known = std::any_of(tools.begin(), tools.end(), [&](const ToolSpec& t) { return t.name == name; });
                    const Json& args = (*j).has("arguments") ? (*j)["arguments"] : (*j)["parameters"];
                    if (known && args.is_object()) {
                        call.id = "call_text_1";
                        call.name = name;
                        call.arguments = args;
                        return true;
                    }
                }
                break;
            }
        }
    }
    return false;
}

std::unique_ptr<Model> make_model(const Settings& settings, Transport& transport) {
    if (settings.kind() == ProviderKind::Anthropic) return std::make_unique<AnthropicModel>(settings, transport);
    return std::make_unique<OpenAIModel>(settings, transport);
}

}  // namespace culprit::ai
