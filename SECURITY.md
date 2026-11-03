# Security

culprit reads logs, and logs often hold secrets: passwords, tokens, customer data. Some parts of culprit
exist to keep those safe, and a weakness in them matters.

## Reporting a problem

**Please don't open a public issue.** Report it privately with GitHub's
[private vulnerability reporting](https://github.com/amirizalrahmat0799/culprit/security/advisories/new)
(the repository's **Security** tab, then **Report a vulnerability**).

Include what you did, what you could see or do that you shouldn't, and the culprit version and system. A
small reproduction helps a lot.

You'll get an answer within a week. Once it's fixed and released, the advisory is published with credit to
you, unless you'd rather not be named.

## What counts

Anything that lets someone read data or do things they shouldn't, for example:

- **`culprit web`:** reading logs or starting an investigation without the key in the address, from another
  website, another host name (DNS rebinding), or another user on the same computer; a log line that runs code
  in the page.
- **The AI investigator:** the AI reading files outside the project, files that may hold secrets (`.env`,
  keys, credentials, git-ignored files), or seeing values culprit should have redacted; anything sent to a
  cloud provider without the user agreeing first.
- **Rule files:** a rule file (for example in a project you cloned) that makes culprit run commands, read
  files, or open links the user didn't choose.
- **Logs themselves:** a crafted log line that crashes culprit, makes it run code, or drives the terminal
  (escape sequences that change the title, the clipboard or the screen).

## Supported versions

Fixes go into the latest release. Please check the problem still happens with it
(`npm install -g culprit-cli@latest`).
