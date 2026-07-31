#include "terminal/terminal_app.h"

#include <QTextStream>

#include "config/config.h"

int runTerminal(const Config &config)
{
    // Stub: the viewer (raw-mode termios, frame timer, render backends) lands in
    // the next phase. Echo the resolved target so the dispatch path is
    // observable from the command line in the meantime.
    QTextStream out(stdout);
    out << "anoa-browser terminal: not implemented yet" << Qt::endl;
    out << "  target: "
        << (config.cdpUrl.isEmpty()
                ? QStringLiteral("http://%1:%2").arg(config.termHost).arg(config.termPort)
                : config.cdpUrl)
        << Qt::endl;
    out << "  fps: " << config.fps << ", gfx: " << config.gfxMode << Qt::endl;
    return 0;
}
