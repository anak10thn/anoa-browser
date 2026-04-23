#pragma once

#include <QWebEngineView>

#include "../config/config.h"

class QWebEngineProfile;

class AnoaBrowser : public QWebEngineView
{
    Q_OBJECT

public:
    explicit AnoaBrowser(const Config &config, QWidget *parent = nullptr);
    void init();

private:
    Config m_config;
    QWebEngineProfile *m_profile;
};
