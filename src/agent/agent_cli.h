#pragma once

// The agent command layer: `anoa open`, `anoa snapshot`, `anoa click @e2`, …
//
// Every one of these is a one-shot process that attaches to an anoa that is
// already running and leaves it running. That is the whole design: an agent
// composes shell commands, and a command that had to boot a browser first
// would cost seconds per step and lose all page state between them. The
// browser is the session; these commands are statements against it.
//
// State that has to survive between invocations lives in the page, not here —
// see agent_script.h for why the @e1/@e2 refs are DOM attributes.

#include <QString>
#include <QStringList>

struct Config;

// True when `verb` is one of the agent commands. main.cpp asks before doing
// anything else, because the answer decides which application class to build.
bool isAgentCommand(const QString &verb);

// Runs one command against the endpoint in `config` (--port / --host).
// Returns the process exit code: 0 on success, 1 on a command failure, 2 on
// usage error, 3 when no browser is listening.
int runAgentCommand(const Config &config, const QString &verb, const QStringList &args);
