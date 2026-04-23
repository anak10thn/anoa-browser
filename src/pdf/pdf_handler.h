#pragma once

#include <QObject>
#include <QJsonObject>
#include <QString>

class QWebEnginePage;

class PdfHandler : public QObject {
    Q_OBJECT
public:
    explicit PdfHandler(QWebEnginePage *page, QObject *parent = nullptr);
    QString handlePrintToPdf(const QJsonObject &cmd);

private:
    QWebEnginePage *m_page;
};
