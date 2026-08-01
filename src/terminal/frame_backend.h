#pragma once

// The seam between the terminal UI and whatever is actually driving the page.
// Two transports implement it: the /render/* HTTP endpoints of a local
// anoa-browser (the default) and a remote CDP endpoint (`--cdp`). Nothing
// HTTP- or CDP-specific may appear in this file — terminal_ui.cpp is written
// against this interface alone and never opens a connection itself.

#include <QByteArray>
#include <QMetaType>
#include <QObject>
#include <QString>

// One captured frame. Which pixel format `bytes` carries depends on which
// request produced it: requestRgbFrame() answers with packed RGB already
// scaled for the halfblock renderer, requestPngFrame() answers with the
// encoded PNG untouched (the terminal image protocols transmit it verbatim).
struct FrameData {
    QByteArray bytes;
    int width = 0;      // pixel width of the frame in `bytes`
    int height = 0;     // pixel height of the frame in `bytes`
    int viewportW = 0;  // page viewport width, in logical/CSS pixels
    int viewportH = 0;  // page viewport height, in logical/CSS pixels
};

// Button identity, in SGR mouse order. Backends translate to their own
// spelling ("left"/"middle"/"right" for /render/click, mouse button ids for
// Input.dispatchMouseEvent).
enum class MouseButton { Left, Middle, Right };

class FrameBackend : public QObject
{
    Q_OBJECT

public:
    explicit FrameBackend(QObject *parent = nullptr) : QObject(parent) {}
    ~FrameBackend() override = default;

    // Frame requests. Each call is answered by exactly one frameReady() or
    // frameFailed(), possibly emitted before the call returns (a synchronous
    // transport is free to do that).
    //
    // requestRgbFrame() asks for the page scaled to fit width x height pixels
    // with the aspect ratio kept, so the returned frame is usually smaller
    // than requested; the UI maps coordinates through the size it gets back,
    // never through the size it asked for.
    virtual void requestRgbFrame(int width, int height) = 0;
    virtual void requestPngFrame() = 0;

    // Input forwarding. Coordinates are page pixels in the same space as
    // FrameData::viewportW/H, already mapped from terminal cells by the UI.
    virtual void sendClick(int pageX, int pageY, MouseButton button) = 0;
    // pageX/pageY below zero mean the pointer was outside the page image:
    // scroll the page without a position rather than dropping the event.
    virtual void sendScroll(int pageX, int pageY, int dy) = 0;
    // Raw UTF-8 bytes as they arrived from the terminal. Not a QString: a
    // read burst can end mid-character and the tail must survive to the next
    // send unmangled.
    virtual void sendText(const QByteArray &utf8) = 0;
    // Named control key: "enter", "backspace", "tab", "up", "down", …
    virtual void sendKey(const QString &namedKey) = 0;

    // Navigation, driven by the viewer's own shortcuts rather than by the page.
    // Same fire-and-forget contract as the input methods above: nothing is
    // reported back directly, the next frame is the answer. A backend whose
    // transport cannot express one of these leaves it a no-op rather than
    // failing — the UI has no way to show an error for a keystroke.
    virtual void navigate(const QString &url) = 0;
    virtual void goBack() = 0;
    virtual void goForward() = 0;
    virtual void reloadPage() = 0;

    // Short description of the target for the status bar, e.g. "127.0.0.1:9222".
    virtual QString description() const = 0;

signals:
    void frameReady(const FrameData &frame);
    void frameFailed(const QString &reason);
    // Connection state in words, for the status bar. Empty text clears it.
    void statusChanged(const QString &text);
};

Q_DECLARE_METATYPE(FrameData)
