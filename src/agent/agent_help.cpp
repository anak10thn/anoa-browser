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
      --timeout <ms>                give up after this long (default 15000))"},

    {"inspect", "INSPECT", R"(  anoa snapshot                     page outline + interactive elements with refs
  anoa snapshot -i                  interactive elements only
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
  anoa scroll --top | --bottom      jump to either end)"},

    {"capture", "CAPTURE", R"(  anoa screenshot [file]            PNG of the viewport (default screenshot.png)
  anoa pdf [file]                   PDF of the page (default page.pdf))"},

    {"agents", "AGENTS", R"(  anoa skills list                  what skill documents this binary carries
  anoa skills get core              the core workflow, for an agent to read

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
          << "browser, navigate, inspect, interact, capture, agents)" << Qt::endl;
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
