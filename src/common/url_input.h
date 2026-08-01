#pragma once

// Turning what a person typed into something QUrl and /render/navigate accept.
//
// Shared by the two places that take an address from a human: the terminal
// viewer's Ctrl-L prompt and the GUI window's address field. It lives here
// rather than in either of them because the rule has to be the same in both —
// a url that works in one and is rejected by the other is the kind of
// difference nobody reports as a bug, they just stop using the one that failed.

#include <QString>

// Trims surrounding space and supplies "https://" when no scheme was typed.
// A string that already names a scheme is returned unchanged, including
// file: and about:. "localhost:8080" is treated as host-and-port, not as a
// scheme, because that is what someone typing it means.
// Returns an empty string for input that is empty or only whitespace.
QString normalizeUserUrl(const QString &raw);
