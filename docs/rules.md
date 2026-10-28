# Writing rules

A rule teaches culprit to recognise one problem in a log and explain it: what it means, and what to do.
culprit ships with over a hundred (`culprit rules` lists them). You can add your own for the problems your
team keeps meeting: an error from an internal library, a payment provider's decline code, a misconfiguration
that only your setup has.

Rules are [TOML](https://toml.io) files:

| Where | Used for |
|---|---|
| `.culprit/rules/*.toml` in your project (or any folder above where you run culprit) | your team's rules; commit them with the code |
| `~/.config/culprit/rules/*.toml` (`%APPDATA%\culprit\rules` on Windows) | your own rules, for every project |
| `--rules <file or folder>` | anything else, for one run |

## A rule

```toml
[[rule]]
id = "kedai.payment-declined"
title = "Payment declined by the bank: {reason}"
severity = "warning"
tags = ["kedai", "payments"]
contains = ["payment declined"]
regex = 'payment declined: (\w+)'
captures = ["reason"]
explain = """
The payment gateway accepted the request, but the customer's bank refused it ({reason}). The order stays \
unpaid; nothing failed in our code."""
fix = [
  "Nothing to fix in code; the customer has to use another card.",
  "If many come at once, check the gateway's status page.",
]
examples = [
  '2026-10-28T10:00:00.000+08:00  WARN 1 --- [kedai] [nio-8080-exec-2] c.e.kedai.payment.PaymentClient : payment declined: insufficient_funds',
]
```

A file can hold any number of `[[rule]]` sections.

## Keys

| Key | | |
|---|---|---|
| `id` | required | Unique name: a-z, 0-9, `.`, `-`, `_`. Start with your team or project name (`kedai.`) so it never clashes with culprit's own. |
| `title` | required | One line, shown in lists. Can use `{captures}`. |
| `contains` | required (or `field`) | One or more pieces of text; the rule is tried on entries that contain any of them, ignoring case. Keep them specific: they're what makes matching fast. |
| `regex` | | A regular expression ([ECMAScript syntax](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Guide/Regular_expressions)), tried on each line that contains one of the `contains` texts. Case-sensitive. Use 'single quotes' so backslashes stay as they are. |
| `captures` | | Names for the regex's groups, in order: `captures = ["port"]` makes the first group `{port}`. |
| `field` + `values` | | Match a parsed field instead of text: `field = "status"`, `values = ["502", "503"]`. The field's value is available as `{status}`. |
| `level` | default `">=warn"` | Entries with a known level below this don't match. `"any"` matches any level; `">=error"` only errors. Lines without a level always qualify. |
| `severity` | default `"error"` | `info`, `warning`, `error` or `critical`. Orders the problems list, and `--fail-on` uses it. |
| `symptom` | default `false` | `true` for problems that are usually caused by something else (connection refused, 502, timeouts). The root cause is looked for among the others first. |
| `priority` | default `0` | When several rules match one entry, the highest wins. culprit's general rules use -10, specific ones 10 to 25. |
| `explain` | | What it means, in plain words. Can use `{captures}`. |
| `fix` | | A list of steps, most useful first. Can use `{captures}`. |
| `docs` | | A link for more. |
| `examples` | | Log text this rule must recognise (several lines for a stack trace). `culprit rules --check` makes sure it does. |
| `tags` | | Words to group rules by. |
| `disabled` | | `true` turns off the rule with this `id`, e.g. one of culprit's you don't want. |

## What a rule sees

For text logs, the whole entry as written: the line, plus its stack trace or other lines that belong to it.
For JSON logs, the message, the stack trace and every field's value (so `"err":"context deadline exceeded"`
matches too). The prefixes Docker Compose and kubectl add (`api-1  | `) are there as well, so don't anchor
a regex with `^`.

## Replacing culprit's rules

A rule with the same `id` as one of culprit's replaces it, so you can change an explanation or the steps to
fit how your team works, e.g. point `hikari.pool-exhausted` at your runbook:

```toml
[[rule]]
id = "hikari.pool-exhausted"
title = "Connection pool {pool} ran out of connections"
contains = ["connection is not available, request timed out"]
regex = '(\S+) - Connection is not available, request timed out after (\d+)ms'
captures = ["pool", "timeout"]
explain = "See the runbook: https://wiki.example.com/runbooks/db-pool"
```

To turn one off:

```toml
[[rule]]
id = "net.timeout"
disabled = true
```

## Checking your rules

```bash
culprit rules --check        # syntax, regexes, placeholders, and every example
culprit rules kedai          # list the rules whose id starts with "kedai"
culprit rules kedai.payment-declined
```

`--check` exits with 1 when something is wrong, so it can run in CI next to your tests. Mistakes are
reported with the file and line:

```
.culprit/rules/kedai.toml:14: {reason} isn't one of the captures
.culprit/rules/kedai.toml:31: kedai.refund-stuck example 1 is recognised by java.npe instead (raise this rule's priority?)
```

## Tips

- Start from a real log line. Put it in `examples` first, then write `contains` and `regex` until
  `--check` passes.
- Prefer one specific rule over a broad one. "Payment declined: insufficient funds" helps; "Payment error"
  doesn't say anything the log didn't.
- Write `explain` for someone who has never seen the error, and `fix` for someone who has to act now.
- Mark consequences as `symptom = true`, so culprit looks past them for the cause.
