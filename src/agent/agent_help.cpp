#include "agent/agent_help.h"

#include <QTextStream>

namespace {

struct Group {
    const char *name;
    const char *title;
    const char *body;
};

// Ordered the way the work is: start something, go somewhere, look at it, touch
// it, take a copy. An agent reading top to bottom gets a usable workflow.
const Group kGroups[] = {
    {"browser", "BROWSER  — start and view the browser itself", R"(  anoa [options]                    start a browser (add --headless for no window)
  anoa terminal [options]           watch and drive it from this terminal
  anoa --help-browser               every flag the browser itself takes

  The browser is the session. Agent commands below attach to one that is
  already running and leave it running, so page state survives between them.)"},

    {"navigate", "NAVIGATE", R"(  anoa open <url>                   go to a url (scheme optional)
  anoa back | forward | reload      move through history
  anoa wait --load                  wait for the page to finish loading
  anoa wait --selector <css>        wait for an element to appear
  anoa wait --ms <n>                wait a fixed time
  anoa wait --text "<text>"         wait for text to appear on the page
  anoa wait --url "<fragment>"      wait for the url to contain something
  anoa wait --fn "<js>"             wait for a JS expression to be truthy
  anoa wait <css> --state hidden    wait for an element to go away
      --timeout <ms>                give up after this long (default 15000)

  A bare argument is read as a duration when it is a number and as a selector
  otherwise, so `anoa wait 500` and `anoa wait "#results"` both do what they look
  like.)"},

    {"inspect", "INSPECT", R"(  anoa snapshot                     page outline + interactive elements with refs
  anoa snapshot -i                  interactive elements only
  anoa find role <role>             locate by role (button, link, textbox, …)
  anoa find text <text>             locate by visible text, deepest match wins
  anoa find selector <css>          locate by CSS, returned as refs
      --nth <n>                     keep only the nth match (1-based)
  anoa get text [<target>]          visible text of the page or one element
  anoa get html <target>            outer HTML
  anoa get value <target>           current form value
  anoa get attr <target> <name>     one attribute
  anoa eval <js>                    evaluate an expression in the page
  anoa status                       what the browser is attached to right now

  <target> is a ref from a snapshot (@e2) or any CSS selector.)"},

    {"interact", "INTERACT", R"(  anoa click <target>               click, hit-tested — fails if something covers it
  anoa fill <target> <text>         set a field's value and fire input/change
  anoa type <text>                  type into whatever has focus
  anoa press <key>                  Enter, Tab, Escape, ArrowDown, …
  anoa scroll [--up] [--by <px>]    scroll the page
  anoa scroll --top | --bottom      jump to either end
  anoa mouse move <x> <y>           move the pointer
  anoa mouse down | up [x] [y]      press or release, for drags
  anoa mouse wheel <dy> [x] [y]     wheel at a position)"},

    {"capture", "CAPTURE", R"(  anoa screenshot [file]            PNG of the viewport (default screenshot.png)
  anoa pdf [file]                   PDF of the page (default page.pdf))"},

    {"state", "STATE  — cookies, storage and emulation", R"(  anoa cookies                      list cookies
  anoa cookies set <name> <value>   set one, scoped to the current page
      --url <url>                   scope it somewhere else instead
  anoa cookies clear                clear them all

  anoa storage local                everything in localStorage
  anoa storage local <key>          one key
  anoa storage local set <k> <v>    write one
  anoa storage local remove <k>     delete one
  anoa storage local clear          empty it
  anoa storage session ...          the same, for sessionStorage

  anoa set viewport <w> <h> [scale] resize the page
  anoa set device [name]            a preset; no name lists them
  anoa set geo <lat> <lng>          override geolocation
  anoa set offline [on|off]         cut the page off from the network
  anoa set headers '<json>'         extra HTTP headers on every request
  anoa set media dark|light         emulate prefers-color-scheme)"},

    {"debug", "DEBUG  — what the page did", R"(  anoa console [--level <lvl>]      console output, newest last
  anoa errors                       uncaught exceptions and rejections
  anoa network                      fetch/XHR the page made: method, status, ms
      --clear                       forget what has been recorded so far

  These are recorded *inside the page*, so they cover what happened before the
  command ran — a one-shot process could never have subscribed in time. The
  buffer starts empty on every page load and holds the last 500 entries. Only
  fetch and XHR are seen; document and subresource loads are not.)"},

    {"agents", "AGENTS", R"(  anoa skills list                  what skill documents this binary carries
  anoa skills get core              the core workflow, for an agent to read
  anoa skills get commands          every command, with its arguments
  anoa close                        ask the browser to exit

  Add --json to any command for machine-readable output.
  Exit codes: 0 ok · 1 command failed · 2 usage · 3 no browser listening)"},
};

QTextStream &out()
{
    static QTextStream s(stdout);
    return s;
}

} // namespace

void printAgentHelp()
{
    out() << R"(anoa — a browser you drive from the command line

USAGE
  anoa --headless --port 9222       start a browser, leave it running
  anoa open example.com             then talk to it
  anoa snapshot -i                  find what is on the page
  anoa click @e2                    act on it by ref

)";
    for (const Group &g : kGroups)
        out() << g.title << Qt::endl << QLatin1String(g.body) << Qt::endl << Qt::endl;

    out() << "  anoa help <group>                one section on its own ("
          << "browser, navigate, inspect, interact, state, debug, capture, agents)" << Qt::endl;
}

bool printAgentHelpGroup(const QString &group)
{
    for (const Group &g : kGroups) {
        if (group.compare(QLatin1String(g.name), Qt::CaseInsensitive) == 0) {
            out() << g.title << Qt::endl << QLatin1String(g.body) << Qt::endl;
            return true;
        }
    }
    return false;
}
