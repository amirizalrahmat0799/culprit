# Investigating with AI

culprit's rules recognise known problems. For the errors they don't explain, or when you want to know
*where in your code* it went wrong, culprit can ask an AI to investigate: it gives the model the error and
what culprit already knows, and lets it read your project, step by step, until it can say what caused it.

AI is optional and off until you set it up. Everything else in culprit works without it.

```bash
culprit ai setup                     # once: choose the AI
culprit investigate app.log          # investigate the likely root cause
culprit investigate --line 214 app.log
docker compose logs --no-color | culprit investigate
```

In the viewer, select an error (or a problem in the `a` panel) and press `i`.

## Choosing an AI

`culprit ai setup` asks which kind you want:

| | Install | Cost | Leaves your computer |
|---|---|---|---|
| **Ollama** (a model on this computer) | [Ollama](https://ollama.com/download) and a model | free | nothing |
| **Free cloud services**: Groq, OpenRouter, Cerebras, Google Gemini | nothing; a free account and key | free, with daily limits | the error and the files read |
| **Your API key**: Anthropic (Claude), OpenAI, Mistral, DeepSeek | nothing | pay per use | the error and the files read |
| **Your company's server**: any OpenAI-compatible URL | nothing | your company's | to your company's server |
| **None** | | | nothing |

**Ollama** is the default suggestion: free, private, and no limits. A model has to fit in memory:

| Model | Download | Memory | |
|---|---|---|---|
| `qwen3:8b` | about 5 GB | 8 GB GPU, or 16 GB RAM | fast on most gaming laptops; a good start |
| `qwen3-coder:30b` | about 18 GB | 24 GB GPU, or 32 GB RAM | reads code better; slower if it doesn't fit the GPU |

```bash
ollama pull qwen3:8b
culprit ai setup        # choose 1, then the model
```

**Cloud keys** are read from an environment variable (`GROQ_API_KEY`, `ANTHROPIC_API_KEY`, ...). culprit
never stores the key itself. On Windows: `setx GROQ_API_KEY "..."` and open a new terminal; on macOS and
Linux: `export GROQ_API_KEY=...` in `~/.bashrc` or `~/.zshrc`.

Free tiers have daily limits, and one investigation takes several requests (one per step, up to 12), so
a limit of 50 requests a day is a handful of investigations.

The settings are in `~/.config/culprit/ai.toml` (`%APPDATA%\culprit\ai.toml` on Windows):

```toml
enabled     = true
provider    = "ollama"
name        = "Ollama"
url         = "http://localhost:11434/v1"
model       = "qwen3:8b"
api_key_env = ""          # the variable that holds the key; never the key
max_steps   = 12          # model calls per investigation
timeout     = 180         # seconds to wait for one answer
confirm     = true        # ask before sending anything to a provider that isn't on this computer
```

`culprit ai status` shows them and tests the connection. `culprit ai off` and `culprit ai on` switch it
without changing anything else.

## What it does

1. **culprit gathers the error.** The entry and its stack trace, the log lines before it, what the rules
   say about it, the analysis of the whole log (the likely root cause, what followed it, the first error),
   and the stack frames that are in your project, already matched to files.
2. **The model investigates, with read-only tools:**

   | Tool | |
   |---|---|
   | `read_file` | lines of a file, with line numbers (stack trace paths like `OrderService.java` or `/app/src/server.ts` work) |
   | `search` | text across the project |
   | `find_files`, `list_dir` | finding its way around |
   | `git_history` | uncommitted changes and recent commits of a file, with diffs |
   | `search_log` | the rest of the log |

   You see each step as it happens: `▸ reading src/main/.../OrderService.java`.
3. **It answers** with the root cause, an explanation, the fix, quotes from your files as evidence, and how
   sure it is.
4. **culprit checks the evidence.** Every quote is looked up in the real file: `✓` when it's there (with the
   line number corrected if the model got it wrong), `✗` when it isn't. If quotes don't check out, the
   confidence is lowered and the answer says so.

AI can be wrong. Treat the answer as a lead, and trust the `✓` evidence more than the explanation.

## What is sent, and what never is

With Ollama, nothing leaves your computer. With a cloud provider, culprit sends:

- the error, its stack trace, the 20 log lines before it, and a summary of the log
- the names of your project's files, and the contents of the files the model chooses to read

And never:

- **anything outside the project folder** (the nearest folder with `.git` or `.culprit` above where you
  run culprit, or `--project`). Paths are checked after following symbolic links.
- **files that may hold secrets**: `.env` files (but not `.env.example`), keys and certificates (`.pem`,
  `.key`, `.p12`, `.jks` ...), `id_rsa` and other SSH keys, `.npmrc`, `.netrc`, credential files, Terraform
  state, and secret manifests (`secrets.yaml`)
- **files git ignores** (local settings usually are), and the `.git` folder
- **secret values in the files it does read**: anything after `password`, `secret`, `token`, `api_key`,
  `credential` ... in config files becomes `[redacted]`, quoted values next to those words in code too, and
  things that look like keys anywhere (`sk-...`, `ghp_...`, `AKIA...`, private key blocks)

Before an investigation sends anything to a provider that isn't on this computer, culprit says where it's
going and asks. `--show-prompt` prints exactly what the first request contains, without sending it.

## Switching it off

| | |
|---|---|
| One run | `--no-ai` |
| This terminal, or a machine | `CULPRIT_AI=off` |
| A project, for everyone | `ai = false` in `.culprit/config.toml`, committed with the code. This wins over personal settings. |
| Your settings | `culprit ai off` |
| The program | build with `-DCULPRIT_AI=OFF` |

## In CI

```yaml
- name: What went wrong
  if: failure()
  env:
    GROQ_API_KEY: ${{ secrets.GROQ_API_KEY }}
  run: |
    npm install -g culprit-cli
    mkdir -p ~/.config/culprit
    printf 'enabled = true\nprovider = "groq"\nname = "Groq"\nurl = "https://api.groq.com/openai/v1"\nmodel = "llama-3.3-70b-versatile"\napi_key_env = "GROQ_API_KEY"\n' > ~/.config/culprit/ai.toml
    docker compose logs --no-color > compose.log
    culprit analyze --investigate --yes --format markdown compose.log >> "$GITHUB_STEP_SUMMARY"
```

`--yes` is needed in CI, where there's nobody to ask.

## When it doesn't work

| | |
|---|---|
| `can't reach Ollama ... Is Ollama running?` | Start the Ollama app, or run `ollama serve`. |
| `answered 404 ... ollama pull` | The model isn't downloaded: `ollama pull <model>`. |
| `answered 401` | The API key is wrong, or isn't set in this terminal. |
| `answered 429` | The free tier's limit is used up for now. Try later, or use Ollama. |
| It answers in plain text, without evidence | Small models sometimes don't use tools well. Try a bigger model, such as `qwen3-coder:30b`. |
| `no answer within 180 seconds` | The model is too slow for this computer. Raise `timeout`, or use a smaller model. |
| `libcurl isn't installed` (Linux) | `sudo apt install libcurl4` (or your distribution's curl package). |
