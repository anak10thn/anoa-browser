#include "agent/agent_skill.h"

#include <QTextStream>

namespace {

const char kCore[] = R"(# Driving a browser with `anoa`

`anoa` controls a browser that is already running. Each command is a separate
process that attaches, does one thing, and exits — the browser keeps the page,
the cookies and the scroll position between them. Commands can be chained with
`&&` safely and cheaply.

## Start the browser once

```bash
anoa --headless --port 9222 &
```

Everything below attaches to it. `anoa status` says whether one is listening;
exit code 3 means nothing is.

## The loop

```bash
anoa open example.com          # 1. go somewhere
anoa snapshot -i               # 2. see what is interactive, with refs
anoa click @e2                 # 3. act by ref
anoa snapshot -i               # 4. look again — the page changed
```

`snapshot` prints one line per interactive element:

```
  @e1   link       Documentation
  @e3   textbox    Search  [required]
  @e7   button     Sign in
```

The `@e1` refs are written onto the DOM nodes, so they stay valid across
commands until the page replaces those nodes. **Re-snapshot after anything that
changes the page** — a navigation, a submit, an expanded menu. A ref that no
longer resolves reports "no element for @e4"; that means the snapshot is stale,
not that the command is wrong.

Any CSS selector works wherever a ref does: `anoa click "#submit"`.

## Reading a page

```bash
anoa get text                  # all visible text
anoa get text @e5              # one element
anoa get attr @e1 href
anoa eval "document.querySelectorAll('article').length"
```

Prefer `get text` over `get html` — HTML costs far more tokens and rarely says
more. Use `snapshot` when you need to *act*, `get text` when you need to *read*.

## Filling forms

```bash
anoa fill @e3 "user@example.com"
anoa fill @e4 "hunter2"
anoa click @e7
anoa wait --load
```

`fill` sets the value through the native setter and fires `input`/`change`, so
React and other frameworks see it. `type` sends keystrokes to whatever has
focus; `press Enter` submits.

## When a click fails

```
anoa: @e7 is covered by <div> Accept cookies — dismiss it, then re-snapshot
```

Clicks are hit-tested against the point they land on, so a consent banner or
modal is reported rather than clicked through. Deal with the covering element,
take a fresh snapshot, then retry the original ref.

## Waiting

```bash
anoa wait --load                       # navigation finished
anoa wait --selector ".results"        # element appeared
anoa wait --ms 500                     # last resort
```

Prefer `--selector` over `--ms`: it is both faster when the page is quick and
more reliable when it is slow.

## Output for programs

Add `--json` to any command for structured output. Exit codes: `0` success,
`1` the command failed, `2` bad usage, `3` no browser is listening.

## Watching it happen

`anoa terminal` renders the live page in the terminal and forwards clicks and
typing. It attaches to the same running browser, so it can be left open in one
pane while commands run in another.
)";

const char kIndex[] = R"(core   the workflow: start a browser, snapshot, act by ref
)";

QTextStream &out()
{
    static QTextStream s(stdout);
    return s;
}

} // namespace

int runSkillsCommand(const QStringList &args)
{
    QTextStream err(stderr);

    if (args.isEmpty() || args.first() == QStringLiteral("list")) {
        out() << QLatin1String(kIndex);
        return 0;
    }
    if (args.first() == QStringLiteral("get")) {
        const QString name = args.size() > 1 ? args.at(1) : QStringLiteral("core");
        if (name == QStringLiteral("core")) {
            out() << QLatin1String(kCore);
            return 0;
        }
        err << "anoa: no skill named '" << name << "' — try: anoa skills list" << Qt::endl;
        return 1;
    }
    err << "anoa: usage: anoa skills list | anoa skills get core" << Qt::endl;
    return 2;
}
