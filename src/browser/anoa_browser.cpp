#include "anoa_browser.h"

#include <QDir>
#include <QWebEnginePage>
#include <QWebEngineProfile>

AnoaBrowser::AnoaBrowser(const Config &config, QWidget *parent)
    : QWebEngineView(parent)
    , m_config(config)
    , m_profile(nullptr)
{
    // QTWEBENGINE_CHROMIUM_FLAGS must be set before WebEngine initializes its
    // profile/page. Setting it here, before creating QWebEngineProfile and
    // calling setPage(), ensures Chromium picks up the remote-debugging port.
    QByteArray flags = "--remote-debugging-port=" + QByteArray::number(m_config.port);
    if (m_config.noSandbox)
        flags += " --no-sandbox";
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);

    if (m_config.headless)
        qputenv("QT_QPA_PLATFORM", "offscreen");

    if (!m_config.profileDir.isEmpty() && !m_config.profileName.isEmpty()) {
        m_profile = new QWebEngineProfile(m_config.profileName, this);
        m_profile->setPersistentStoragePath(
            QDir(m_config.profileDir).filePath(m_config.profileName));
    } else {
        m_profile = QWebEngineProfile::defaultProfile();
    }

    setPage(new QWebEnginePage(m_profile, this));
}

void AnoaBrowser::init()
{
    if (!m_config.headless)
        show();
}
