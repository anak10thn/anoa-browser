#pragma once

#include <QList>
#include <QNetworkCookie>
#include <QPoint>
#include <QUrl>
#include <QWebEngineView>

#include "../config/config.h"

class QWebEngineProfile;

class AnoaBrowser : public QWebEngineView
{
    Q_OBJECT

public:
    explicit AnoaBrowser(const Config &config, QWidget *parent = nullptr);
    void init();

    void loadExtensions(const QStringList &paths);
    void setupNamedProfile(const QString &name, const QString &baseDir);
    QList<QNetworkCookie> getCookies(const QUrl &origin);
    void setCookie(const QNetworkCookie &cookie, const QUrl &origin);
    void clearStorage(const QUrl &origin);

    void sendClick(const QPoint &pos, Qt::MouseButton button);
    void sendScroll(const QPoint &pos, int angleDeltaY);

private:
    Config m_config;
    QWebEngineProfile *m_profile;
};
