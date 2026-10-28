#include <filesystem>
#include <fstream>

#include "ai/investigator.hpp"
#include "ai/provider.hpp"
#include "check.hpp"

using namespace culprit::ai;
namespace fs = std::filesystem;

namespace {

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

fs::path project() {
    fs::path root = fs::temp_directory_path() / ("culprit-inv-test-" + std::to_string(std::rand()));
    fs::remove_all(root);
    write(root / "src/OrderService.java",
          "class OrderService {\n"
          "    void place(Order o) {\n"
          "        Customer c = customers.find(o.customerId());\n"
          "        String email = c.getEmail();\n"
          "    }\n"
          "}\n");
    return root;
}

// a model that answers from a script, and remembers what it was sent
class ScriptedModel : public Model {
public:
    std::vector<Reply> script;
    std::vector<std::vector<Message>> seen;
    std::vector<size_t> tool_counts;
    Reply chat(const std::string&, const std::vector<Message>& messages, const std::vector<ToolSpec>& tools,
               const std::atomic<bool>*) override {
        seen.push_back(messages);
        tool_counts.push_back(tools.size());
        if (script.empty()) return Reply{true, "", "I give up.", {}, 0, 0};
        Reply r = script.front();
        script.erase(script.begin());
        return r;
    }
    std::vector<std::string> list_models(std::string*) override { return {"fake"}; }
};

Reply call(const std::string& name, Json args, long in = 100, long out = 10) {
    Reply r;
    r.ok = true;
    r.calls.push_back({"c" + name, name, std::move(args)});
    r.input_tokens = in;
    r.output_tokens = out;
    return r;
}

Subject subject() {
    Subject s;
    s.title = "NullPointerException";
    s.entry = "java.lang.NullPointerException: Cannot invoke \"Customer.getEmail()\" because \"c\" is null\n"
              "\tat com.example.OrderService.place(OrderService.java:4)";
    s.frames = {"OrderService.java:4"};
    s.search_log = [](const std::string& t) { return "line 7  ERROR  " + t; };
    return s;
}

Settings settings() {
    Settings s;
    s.enabled = true;
    s.name = "Fake";
    s.model = "fake-1";
    s.max_steps = 5;
    return s;
}

}  // namespace

TEST("ai investigator: reads files, finishes, and checks the evidence") {
    fs::path root = project();
    Workspace ws(root.string());
    ScriptedModel m;
    m.script.push_back(call("read_file", Json::object({{"path", "OrderService.java"}, {"start_line", 1}, {"end_line", 6}})));
    m.script.push_back(call(
        "finish",
        Json::object({{"summary", "customers.find returns null for an unknown customer id"},
                      {"explanation", "place() uses the result without checking it."},
                      {"fix", Json::array({"Check the result of customers.find", "Return 404 for unknown customers"})},
                      {"evidence", Json::array({
                                       // right text, wrong line: corrected
                                       Json::object({{"file", "OrderService.java"}, {"line", 9}, {"quote", "Customer c = customers.find(o.customerId());"}}),
                                       // copied with the line-number prefix: still matches
                                       Json::object({{"file", "src/OrderService.java"}, {"line", 4}, {"quote", "4:         String email = c.getEmail();"}}),
                                       // made up
                                       Json::object({{"file", "src/OrderService.java"}, {"line", 3}, {"quote", "if (c == null) return;"}}),
                                       // not a file
                                       Json::object({{"file", "src/Nope.java"}, {"line", 1}, {"quote", "x"}}),
                                   })},
                      {"confidence", "high"}})));
    std::vector<Step> steps;
    Investigator inv(m, ws, settings());
    Finding f = inv.run(subject(), [&](const Step& s) { steps.push_back(s); }, nullptr);

    CHECK(f.ok);
    CHECK_EQ(f.steps, 2);
    CHECK_EQ(f.input_tokens, 200L);
    CHECK_EQ(f.summary, std::string("customers.find returns null for an unknown customer id"));
    CHECK_EQ(f.fix.size(), 2u);
    CHECK_EQ(f.evidence.size(), 4u);
    if (f.evidence.size() == 4) {
        CHECK(f.evidence[0].verified);
        CHECK_EQ(f.evidence[0].line, 3);
        CHECK_EQ(f.evidence[0].file, std::string("src/OrderService.java"));
        CHECK(f.evidence[0].note.find("found at line 3") != std::string::npos);
        CHECK(f.evidence[1].verified);
        CHECK(!f.evidence[2].verified);
        CHECK(!f.evidence[3].verified);
    }
    CHECK_EQ(f.confidence, std::string("medium"));  // lowered: two quotes didn't check out
    CHECK(!f.confidence_note.empty());
    CHECK_EQ(f.files_read.size(), 1u);
    // the model got the file, with line numbers, as the tool's answer
    CHECK(m.seen.size() == 2 && m.seen[1].back().role == Message::Role::Tool);
    if (m.seen.size() == 2) CHECK(m.seen[1].back().text.find("4:         String email = c.getEmail();") != std::string::npos);
    // the first message carries the error and the resolved stack frame
    CHECK(m.seen[0][0].text.find("OrderService.java:4  ->  src/OrderService.java") != std::string::npos);
    bool told = false;
    for (const auto& s : steps) told |= s.kind == Step::Kind::Tool && s.text.find("reading OrderService.java") != std::string::npos;
    CHECK(told);
    fs::remove_all(root);
}

TEST("ai investigator: a plain-text answer gets one nudge, then is taken as it is") {
    fs::path root = project();
    Workspace ws(root.string());
    ScriptedModel m;
    m.script.push_back(Reply{true, "", "The customer is null.", {}, 0, 0});
    m.script.push_back(Reply{true, "", "Still: the customer is null.", {}, 0, 0});
    Investigator inv(m, ws, settings());
    Finding f = inv.run(subject(), [](const Step&) {}, nullptr);
    CHECK(f.ok);
    CHECK(!f.structured);
    CHECK_EQ(f.confidence, std::string("low"));
    CHECK_EQ(f.summary, std::string("Still: the customer is null."));
    CHECK(m.seen.size() == 2 && m.seen[1].back().text.find("finish tool") != std::string::npos);
    fs::remove_all(root);
}

TEST("ai investigator: the step limit forces an answer, and the log can be searched") {
    fs::path root = project();
    Workspace ws(root.string());
    ScriptedModel m;
    for (int i = 0; i < 4; ++i) m.script.push_back(call("search_log", Json::object({{"text", "customer"}})));
    m.script.push_back(call("finish", Json::object({{"summary", "unknown"}, {"explanation", "ran out"}, {"confidence", "low"}})));
    Investigator inv(m, ws, settings());
    Finding f = inv.run(subject(), [](const Step&) {}, nullptr);
    CHECK(f.ok);
    CHECK_EQ(f.steps, 5);
    CHECK(m.tool_counts.size() == 5 && m.tool_counts.back() == 1u);  // only finish on the last step
    CHECK(m.seen[1].back().text.find("line 7  ERROR  customer") != std::string::npos);
    fs::remove_all(root);
}

TEST("ai investigator: errors and cancelling stop it") {
    fs::path root = project();
    Workspace ws(root.string());
    ScriptedModel m;
    m.script.push_back(Reply{false, "Groq answered 429: rate limit", "", {}, 0, 0});
    Investigator inv(m, ws, settings());
    Finding f = inv.run(subject(), [](const Step&) {}, nullptr);
    CHECK(!f.ok);
    CHECK(f.error.find("429") != std::string::npos);
    std::atomic<bool> cancel{true};
    Finding g = inv.run(subject(), [](const Step&) {}, &cancel);
    CHECK_EQ(g.error, std::string("cancelled"));
    fs::remove_all(root);
}

TEST("ai provider: tool calls written as text are understood") {
    std::vector<ToolSpec> tools = Investigator::tools();
    ToolCall c;
    CHECK(tool_call_from_text("Let me look.\n{\"name\": \"read_file\", \"arguments\": {\"path\": \"a.java\"}}", tools, c));
    CHECK_EQ(c.name, std::string("read_file"));
    CHECK_EQ(c.arguments["path"].str(), std::string("a.java"));
    CHECK(!tool_call_from_text("{\"name\": \"rm_rf\", \"arguments\": {}}", tools, c));
    CHECK(!tool_call_from_text("no json here", tools, c));
}

namespace {
// records the request and answers with a canned body
class FakeTransport : public Transport {
public:
    HttpRequest last;
    HttpResponse reply;
    HttpResponse send(const HttpRequest& r, const std::atomic<bool>*) override {
        last = r;
        return reply;
    }
};
std::string header(const HttpRequest& r, const std::string& name) {
    for (const auto& [k, v] : r.headers) {
        if (k == name) return v;
    }
    return {};
}
std::vector<Message> conversation() {
    std::vector<Message> m;
    m.push_back({Message::Role::User, "why?", {}, {}, {}});
    m.push_back({Message::Role::Assistant, "", {{"t1", "read_file", Json::object({{"path", "a.java"}})}, {"t2", "search", Json::object({{"text", "x"}})}}, {}, {}});
    m.push_back({Message::Role::Tool, "1: class A {}", {}, "t1", "read_file"});
    m.push_back({Message::Role::Tool, "no matches", {}, "t2", "search"});
    return m;
}
}  // namespace

TEST("ai provider: OpenAI-compatible requests and replies (Ollama, Groq, ...)") {
    FakeTransport t;
    t.reply.status = 200;
    t.reply.body = R"({"choices":[{"message":{"role":"assistant","content":null,"tool_calls":[
        {"id":"call_9","type":"function","function":{"name":"read_file","arguments":"{\"path\":\"b.java\",\"start_line\":3}"}}]}}],
        "usage":{"prompt_tokens":1200,"completion_tokens":40}})";
    Settings s;
    s.provider = "groq";
    s.name = "Groq";
    s.url = "https://api.groq.com/openai/v1";
    s.model = "llama";
    s.api_key_env = "CULPRIT_TEST_KEY";
#ifdef _WIN32
    _putenv_s("CULPRIT_TEST_KEY", "gsk_test");
#else
    setenv("CULPRIT_TEST_KEY", "gsk_test", 1);
#endif
    auto model = make_model(s, t);
    Reply r = model->chat("system text", conversation(), Investigator::tools(), nullptr);
    CHECK(r.ok);
    CHECK(r.calls.size() == 1 && r.calls[0].id == "call_9" && r.calls[0].arguments["start_line"].integer() == 3);
    CHECK_EQ(r.input_tokens, 1200L);
    CHECK_EQ(t.last.url, std::string("https://api.groq.com/openai/v1/chat/completions"));
    CHECK_EQ(header(t.last, "Authorization"), std::string("Bearer gsk_test"));
    auto body = Json::parse(t.last.body);
    CHECK(body.has_value());
    if (!body) return;
    const Json& msgs = (*body)["messages"];
    CHECK_EQ(msgs.size(), 5u);
    CHECK_EQ(msgs[0]["role"].str(), std::string("system"));
    CHECK_EQ(msgs[2]["tool_calls"][0]["function"]["arguments"].str(), std::string("{\"path\":\"a.java\"}"));
    CHECK_EQ(msgs[3]["role"].str(), std::string("tool"));
    CHECK_EQ(msgs[3]["tool_call_id"].str(), std::string("t1"));
    CHECK_EQ((*body)["tools"].size(), Investigator::tools().size());
    CHECK_EQ((*body)["tools"][0]["function"]["name"].str(), std::string("read_file"));

    t.reply.status = 429;
    t.reply.body = R"({"error":{"message":"Rate limit reached"}})";
    Reply e = model->chat("s", conversation(), {}, nullptr);
    CHECK(!e.ok);
    CHECK(e.error.find("429") != std::string::npos && e.error.find("Rate limit reached") != std::string::npos);
    t.reply.status = 0;
    t.reply.error = "Couldn't connect to server";
    s.provider = "ollama";
    s.name = "Ollama";
    auto ollama = make_model(s, t);
    CHECK(ollama->chat("s", conversation(), {}, nullptr).error.find("Is Ollama running?") != std::string::npos);
}

TEST("ai provider: Anthropic messages, tool results grouped, prompt caching") {
    FakeTransport t;
    t.reply.status = 200;
    t.reply.body = R"({"content":[{"type":"text","text":"Looking at the caller."},
        {"type":"tool_use","id":"toolu_1","name":"search","input":{"text":"place("}}],
        "usage":{"input_tokens":50,"cache_read_input_tokens":2000,"output_tokens":30}})";
    Settings s;
    s.provider = "anthropic";
    s.name = "Anthropic (Claude)";
    s.url = "https://api.anthropic.com/v1";
    s.model = "claude-test";
    s.api_key_env = "CULPRIT_TEST_KEY";
    auto model = make_model(s, t);
    Reply r = model->chat("system text", conversation(), Investigator::tools(), nullptr);
    CHECK(r.ok);
    CHECK_EQ(r.text, std::string("Looking at the caller."));
    CHECK(r.calls.size() == 1 && r.calls[0].name == "search" && r.calls[0].arguments["text"].str() == "place(");
    CHECK_EQ(r.input_tokens, 2050L);
    CHECK_EQ(t.last.url, std::string("https://api.anthropic.com/v1/messages"));
    CHECK_EQ(header(t.last, "anthropic-version"), std::string("2023-06-01"));
    CHECK(!header(t.last, "x-api-key").empty());
    auto body = Json::parse(t.last.body);
    CHECK(body.has_value());
    if (!body) return;
    const Json& msgs = (*body)["messages"];
    CHECK_EQ(msgs.size(), 3u);  // user, assistant, one user message with both tool results
    CHECK_EQ(msgs[1]["content"][0]["type"].str(), std::string("tool_use"));
    CHECK_EQ(msgs[2]["content"].size(), 2u);
    CHECK_EQ(msgs[2]["content"][1]["tool_use_id"].str(), std::string("t2"));
    CHECK_EQ(msgs[2]["content"][1]["cache_control"]["type"].str(), std::string("ephemeral"));
    CHECK_EQ((*body)["system"][0]["cache_control"]["type"].str(), std::string("ephemeral"));
    CHECK((*body)["tools"][0].has("input_schema"));
}
