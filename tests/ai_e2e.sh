#!/usr/bin/env bash
# culprit investigate against a fake AI server, over real HTTP, for both API formats.
#   bash tests/ai_e2e.sh <path to culprit>
set -euo pipefail

exe=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
repo=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'kill $server 2>/dev/null || true; rm -rf "$work"' EXIT

port=$((20000 + RANDOM % 20000))
python3 "$repo/tests/fake_ai_server.py" "$port" "$work/requests.jsonl" &
server=$!
for _ in $(seq 50); do curl -s "http://127.0.0.1:$port/v1/models" >/dev/null 2>&1 && break; sleep 0.1; done

export XDG_CONFIG_HOME="$work/config" NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost
export CULPRIT_FAKE_KEY=sk-test-not-real
mkdir -p "$XDG_CONFIG_HOME/culprit"
cp -r "$repo/testdata/kedai" "$work/kedai"
cd "$work/kedai"

check() {
  local out=$1
  grep -q '"ok":true' "$out" || { echo "no finding"; cat "$out"; exit 1; }
  python3 - "$out" "$work/requests.jsonl" <<'PY'
import json, sys
f = json.load(open(sys.argv[1]))
ev = f["evidence"]
assert [e["verified"] for e in ev] == [True, True, False], ev
assert ev[1]["file"].endswith("V5__currency.sql") and ev[1]["line"] == 1, ev[1]   # line corrected
assert f["confidence"] == "medium", f["confidence"]                              # lowered: one quote was made up
sent = open(sys.argv[2]).read()
assert "not-a-real-password" not in sent, "a secret was sent"
assert "may hold secrets" in sent, ".env wasn't refused"
assert "outside the project" in sent or "no file" in sent, "../ wasn't refused"
print("  ok:", f["summary"][:60], "| evidence", [e["verified"] for e in ev])
PY
}

echo "OpenAI-compatible (Ollama, Groq, ...):"
cat > "$XDG_CONFIG_HOME/culprit/ai.toml" <<TOML
enabled = true
provider = "ollama"
name = "Fake Ollama"
url = "http://127.0.0.1:$port/v1"
model = "fake-coder"
TOML
"$exe" ai status > /dev/null
"$exe" investigate --format json "$repo/testdata/incident-migration.log" > "$work/openai.json" 2> /dev/null
check "$work/openai.json"

echo "Anthropic (Claude):"
: > "$work/requests.jsonl"
cat > "$XDG_CONFIG_HOME/culprit/ai.toml" <<TOML
enabled = true
provider = "anthropic"
name = "Fake Claude"
url = "http://127.0.0.1:$port/v1"
model = "fake-claude"
api_key_env = "CULPRIT_FAKE_KEY"
TOML
"$exe" investigate --format json "$repo/testdata/incident-migration.log" > "$work/anthropic.json" 2> /dev/null
check "$work/anthropic.json"
grep -q '"x-api-key": "sk-test-not-real"' "$work/requests.jsonl" || { echo "no API key header"; exit 1; }

echo "Switched off:"
out=$(CULPRIT_AI=off "$exe" investigate "$repo/testdata/incident-migration.log" 2>&1 || true)
grep -q "turned off by CULPRIT_AI" <<< "$out" || { echo "CULPRIT_AI=off didn't stop it: $out"; exit 1; }
mkdir -p .culprit && echo "ai = false" > .culprit/config.toml
out=$("$exe" investigate "$repo/testdata/incident-migration.log" 2>&1 || true)
grep -q "turned off for this project" <<< "$out" || { echo "ai = false didn't stop it: $out"; exit 1; }
echo "  ok"
