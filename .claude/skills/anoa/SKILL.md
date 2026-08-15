---
name: anoa
description: Drive a real browser from the command line — open pages, snapshot the interactive elements as refs, click and fill by ref, read text, run JS, screenshot. Use when a task needs a live browser: checking a page renders, walking a login or checkout flow, scraping something that only exists after JavaScript runs, or reproducing a UI bug.
---

# anoa

`anoa` controls a browser that stays running between commands, so each command
is a cheap one-shot and the page keeps its state.

**Load the full workflow before using it:**

```bash
anoa skills get core
```

That prints the current instructions straight from the installed binary, so they
can never drift from the CLI you actually have. Read it once per session, then
work from it.

Three things worth knowing before you even do that:

- `anoa help` lists every command, grouped. `anoa help interact` narrows it.
- Exit code `3` means no browser is listening — start one with
  `anoa --headless --port 9222 &` and retry.
- One browser holds many tabs. `anoa tab new <url>` prints an id you pass back
  as `--tab <id>`; without it, commands act on the active tab. Refs never cross
  tabs — they live in the page — so snapshot the tab you are about to act on.
  Cookies are shared unless you open the tab with `--isolated` or
  `--profile <name>`.
