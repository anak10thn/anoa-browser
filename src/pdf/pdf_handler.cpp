#include "pdf/pdf_handler.h"

#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QPageLayout>
#include <QPageSize>
#include <QSizeF>
#include <QMarginsF>
#include <QTemporaryFile>
#include <QEventLoop>
#include <QTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

PdfHandler::PdfHandler(QWebEnginePage *page, QObject *parent)
    : QObject(parent), m_page(page)
{
}

QString PdfHandler::handlePrintToPdf(const QJsonObject &cmd)
{
    const int id = cmd.value(QStringLiteral("id")).toInt();
    const QJsonObject params = cmd.value(QStringLiteral("params")).toObject();

    const bool landscape = params.value(QStringLiteral("landscape")).toBool(false);
    const bool printBackground = params.value(QStringLiteral("printBackground")).toBool(false);
    const double scale = params.value(QStringLiteral("scale")).toDouble(1.0);
    Q_UNUSED(scale) // QWebEnginePage::printToPdf exposes no scale parameter
    const double paperWidthIn = params.value(QStringLiteral("paperWidth")).toDouble(8.5);
    const double paperHeightIn = params.value(QStringLiteral("paperHeight")).toDouble(11.0);
    const double marginTopIn = params.value(QStringLiteral("marginTop")).toDouble(0.4);
    const double marginBottomIn = params.value(QStringLiteral("marginBottom")).toDouble(0.4);
    const double marginLeftIn = params.value(QStringLiteral("marginLeft")).toDouble(0.4);
    const double marginRightIn = params.value(QStringLiteral("marginRight")).toDouble(0.4);

    // Convert inches to mm
    QPageSize pageSize(QSizeF(paperWidthIn * 25.4, paperHeightIn * 25.4), QPageSize::Millimeter);
    QMarginsF margins(marginLeftIn * 25.4, marginTopIn * 25.4,
                      marginRightIn * 25.4, marginBottomIn * 25.4);
    QPageLayout layout(pageSize,
                       landscape ? QPageLayout::Landscape : QPageLayout::Portrait,
                       margins,
                       QPageLayout::Millimeter);

    // Get a unique temporary file path
    QTemporaryFile tmpFile;
    if (!tmpFile.open()) {
        QJsonObject error;
        error[QStringLiteral("code")] = -32000;
        error[QStringLiteral("message")] = QStringLiteral("PDF generation timed out or failed");
        QJsonObject resp;
        resp[QStringLiteral("id")] = id;
        resp[QStringLiteral("error")] = error;
        return QJsonDocument(resp).toJson(QJsonDocument::Compact);
    }
    const QString filePath = tmpFile.fileName();
    tmpFile.setAutoRemove(false);
    tmpFile.close();

    m_page->settings()->setAttribute(QWebEngineSettings::PrintElementBackgrounds, printBackground);

    QEventLoop loop;
    bool success = false;
    bool finished = false;

    QMetaObject::Connection conn = connect(
        m_page, &QWebEnginePage::pdfPrintingFinished,
        [&](const QString &path, bool ok) {
            if (path == filePath) {
                success = ok;
                finished = true;
                loop.quit();
            }
        }
    );

    m_page->printToPdf(filePath, layout);
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    loop.exec();

    disconnect(conn);

    auto errorResponse = [&]() -> QString {
        QJsonObject error;
        error[QStringLiteral("code")] = -32000;
        error[QStringLiteral("message")] = QStringLiteral("PDF generation timed out or failed");
        QJsonObject resp;
        resp[QStringLiteral("id")] = id;
        resp[QStringLiteral("error")] = error;
        QFile::remove(filePath);
        return QJsonDocument(resp).toJson(QJsonDocument::Compact);
    };

    if (!finished || !success)
        return errorResponse();

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return errorResponse();

    const QByteArray pdfBytes = file.readAll();
    file.close();
    QFile::remove(filePath);

    QJsonObject result;
    result[QStringLiteral("data")] = QString::fromLatin1(pdfBytes.toBase64());
    QJsonObject resp;
    resp[QStringLiteral("id")] = id;
    resp[QStringLiteral("result")] = result;
    return QJsonDocument(resp).toJson(QJsonDocument::Compact);
}
