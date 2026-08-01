#include "common/url_input.h"

QString normalizeUserUrl(const QString &raw)
{
    const QString url = raw.trimmed();
    if (url.isEmpty())
        return QString();

    const int colon = url.indexOf(QLatin1Char(':'));
    if (colon > 0) {
        // A scheme is letters, digits, '+', '-' and '.' before the colon, and
        // must be more than one character — a bare "c:" is a Windows path, not
        // a scheme. The test matters because "localhost:8080" has a colon too
        // and is a host with a port.
        bool schemeLike = colon > 1;
        for (int i = 0; i < colon && schemeLike; ++i) {
            const QChar ch = url.at(i);
            schemeLike = ch.isLetterOrNumber() || ch == QLatin1Char('+')
                         || ch == QLatin1Char('-') || ch == QLatin1Char('.');
        }
        // "host:8080" ends the discussion: a digit straight after the colon is
        // a port, and no scheme is spelled that way.
        const bool portLike = colon + 1 < url.size() && url.at(colon + 1).isDigit();
        if (schemeLike && !portLike)
            return url;
    }
    return QStringLiteral("https://") + url;
}
