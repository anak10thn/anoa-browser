
## bug: input does not reach a background tab

Found during task-014. Reads work on any tab; input only affects the ACTIVE one.

| operation | active tab | background tab |
|---|---|---|
| `get text`, `eval`, `/render/html`, screenshot, viewport geometry | works | works |
| click, type, key, scroll | works | **does nothing** |

Both input paths fail identically and both report success:

    POST /render/click?x=360&y=425&tab=t2   -> "clicked",      window.__hit stays 0
    anoa click "#b" --port 9500 --tab t2    -> "clicked #b",   window.__hit stays 0

Running `anoa tab select t2` first makes the very same click land, which pins
the cause: AnoaBrowser holds tabs in a QStackedLayout, which HIDES every view
but the current one, and a hidden QWebEngineView processes no input — not
through Qt synthetic events, and not through CDP Input.dispatchMouseEvent
either.

Reporting success for input that did nothing is the worst part: an agent has no
way to notice.

A fix means keeping background views visible-but-covered (same geometry, active
view raised) instead of hidden, which changes what every open tab costs to keep
alive. That belongs in its own task rather than being smuggled into task-014.

Until then `--tab` is read-only for input, and the workaround is
`anoa tab select <id>` first.

Filed by hand: `jonggrang bug` rejects `--feature <id>` and `--feature=<id>`
alike with "Multiple features found. Use --feature <featureId>.", so the flag it
asks for is not honoured.

### FIXED

`AnoaBrowser` no longer uses `QStackedLayout`. Views are children at the
container's geometry with the active one raised, so a background tab is
*covered* rather than hidden — and a covered `QWebEngineView` takes input.

Both paths now land:

    anoa click "#bg" --tab t2               -> window.__bgHit == 1
    POST /render/click?x=..&y=..&tab=t2     -> window.__bgHit == 1

A view must also be `show()`n when it is created: one that was never shown takes
no input even after it is raised.

Screenshot isolation was the risk in removing the layout, and it holds — with
t1 red and t2 blue, `?tab=t1` samples #ff0000, `?tab=t2` samples #0000ff, and no
`?tab=` samples the active one. Locked in by AGENT-28 in the agent CLI e2e suite.
