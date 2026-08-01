#pragma once

// Grouped help. Kept apart from config.cpp's QCommandLineParser output because
// the two answer different questions: the parser lists the flags that start a
// browser, this lists the commands you send to one that is already running.
// Printing them as one wall of options is what makes a CLI with both feel
// unlearnable.

#include <QString>

// The whole thing: usage, then one block per group.
void printAgentHelp();

// One group by name ("navigate", "inspect", "interact", "capture", "browser",
// "agents"). Returns false when the name is not a group, so the caller can
// fall through to something else rather than printing an empty section.
bool printAgentHelpGroup(const QString &group);
