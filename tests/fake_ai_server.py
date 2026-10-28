"""A stand-in for an AI provider, for testing `culprit investigate` without a model.

Speaks both wire formats culprit uses: OpenAI chat completions (what Ollama, Groq, OpenRouter ... speak)
and Anthropic messages. It plays a fixed investigation of testdata/kedai: read the failed migration,
search for the duplicate column, try to read .env (culprit must refuse), then answer.

    python3 tests/fake_ai_server.py <port> <requests.jsonl>

Every request is appended to requests.jsonl, so a test can check what culprit sent.
"""
import http.server
import json
import sys

LOG = open(sys.argv[2], "w")

FINISH = {
    "summary": "V7__add_refunds.sql adds the column orders.currency, which V5__currency.sql already added.",
    "explanation": "Flyway runs V7, PostgreSQL rejects the duplicate column, so the app doesn't start and nginx answers 502.",
    "fix": ["Remove the ALTER TABLE line from V7, or add the column with IF NOT EXISTS.",
            "If V7 already ran somewhere, add a V8 instead and run flyway repair."],
    "evidence": [
        {"file": "src/main/resources/db/migration/V7__add_refunds.sql", "line": 1,
         "quote": "ALTER TABLE orders ADD COLUMN currency char(3) NOT NULL DEFAULT 'MYR';"},
        {"file": "V5__currency.sql", "line": 3, "quote": "ALTER TABLE orders ADD COLUMN currency char(3)"},
        {"file": "src/main/resources/db/migration/V7__add_refunds.sql", "line": 2, "quote": "DROP TABLE orders;"},
    ],
    "confidence": "high",
}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, obj, status=200):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if "/models" in self.path:
            self.reply({"data": [{"id": "fake-coder"}, {"id": "fake-small"}]})
        else:
            self.reply({"error": {"message": "not found"}}, 404)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        LOG.write(json.dumps({"path": self.path, "headers": dict(self.headers), "body": body}) + "\n")
        LOG.flush()
        anthropic = self.path.endswith("/messages")
        messages = body["messages"]
        if anthropic:
            done = sum(1 for m in messages if m["role"] == "user" and isinstance(m["content"], list)
                       for c in m["content"] if c.get("type") == "tool_result")
        else:
            done = sum(1 for m in messages if m["role"] == "tool")
            if "tools" not in body:  # culprit ai status / setup's connection test
                return self.reply({"choices": [{"message": {"role": "assistant", "content": "OK"}}]})
        if done == 0:
            calls = [("read_file", {"path": "V7__add_refunds.sql"}), ("search", {"text": "ADD COLUMN currency"})]
        elif done == 2:
            calls = [("read_file", {"path": ".env"}), ("read_file", {"path": "../../../etc/passwd"})]
        else:
            calls = [("finish", FINISH)]
        if anthropic:
            content = [{"type": "text", "text": "Looking."}] + [
                {"type": "tool_use", "id": f"toolu_{done}_{i}", "name": n, "input": a} for i, (n, a) in enumerate(calls)]
            self.reply({"content": content, "stop_reason": "tool_use", "usage": {"input_tokens": 1000, "output_tokens": 50}})
        else:
            tool_calls = [{"id": f"call_{done}_{i}", "type": "function", "function": {"name": n, "arguments": json.dumps(a)}}
                          for i, (n, a) in enumerate(calls)]
            self.reply({"choices": [{"message": {"role": "assistant", "content": None, "tool_calls": tool_calls}}],
                        "usage": {"prompt_tokens": 1000, "completion_tokens": 50}})


http.server.HTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
