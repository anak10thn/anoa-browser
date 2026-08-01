#pragma once

// The window chrome around AnoaBrowser: an address field and back / forward /
// reload buttons.
//
// It is a *wrapper*, and that is load-bearing rather than a style choice.
// HttpServer captures frames with m_browser->grab() and reports
// m_browser->width()/height() as the viewport that /render/click coordinates
// are measured in. Had the toolbar been added inside AnoaBrowser, it would
// appear in every screenshot and shift the viewport by its own height, so the
// terminal viewer would draw the toolbar and land every click that many pixels
// too high. Keeping the toolbar in a parent leaves the view — and therefore
// everything the render endpoints see — exactly as it was.
//
// Only built in headed mode. With --headless there is no window to put chrome
// on, and the toolbar would be a widget nobody can reach.

#include <QIcon>
#include <QList>
#include <QPair>
#include <QString>
#include <QUrl>
#include <QWidget>

#include "../config/config.h"

class AnoaBrowser;
class QAction;
class QLineEdit;
class QMenu;
class QResizeEvent;
class QTimer;
class QToolButton;
class QVBoxLayout;

class BrowserWindow : public QWidget
{
    Q_OBJECT

public:
    // `view` is reparented into this window's layout; ownership follows Qt's
    // usual parent rules from that point on.
    BrowserWindow(AnoaBrowser *view, const Config &config, QWidget *parent = nullptr);
    // Detaches the view again. Adding a widget to a layout makes this window
    // its parent, and a Qt parent deletes its children — which would be a
    // double free, because main.cpp owns the view on the stack. Releasing it
    // here means the window can be destroyed in any order relative to it.
    ~BrowserWindow() override;

protected:
    // The toolbar is positioned by hand rather than by the layout, so it has to
    // be repositioned whenever the window changes size. See setAutoHide().
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onUrlEntered();
    void onUrlChanged(const QUrl &url);
    void refreshHistoryButtons();
    // Stars the current page into the hamburger menu's list.
    void onBookmark();
    // Auto-hide: turn the toolbar into an overlay that appears when the pointer
    // reaches the top edge, the way the macOS menu bar does in full screen.
    void setAutoHide(bool on);
    void pollPointer();

private:
    // Draws one character into an icon, for the two glyphs that have to live
    // *inside* the address pill — QLineEdit::addAction takes a QIcon only.
    static QIcon glyphIcon(const QString &glyph, int pointSize);
    // A flat, borderless toolbar button whose label is a single glyph.
    QToolButton *makeGlyphButton(const QString &glyph, const QString &tip, int pointSize);
    void rebuildMenu();

    // Places the toolbar across the top and, when it is not overlaying, leaves
    // the view a top margin the same height so the two do not overlap.
    void layoutToolbar();

    AnoaBrowser *m_view = nullptr;
    QWidget *m_toolbar = nullptr;
    QVBoxLayout *m_root = nullptr;
    QLineEdit *m_urlEdit = nullptr;
    QToolButton *m_back = nullptr;
    QToolButton *m_forward = nullptr;
    QToolButton *m_menuButton = nullptr;
    QMenu *m_menu = nullptr;
    QAction *m_star = nullptr;
    QAction *m_autoHideAction = nullptr;
    QTimer *m_pointerTimer = nullptr;
    QList<QPair<QString, QUrl>> m_bookmarks;
    bool m_autoHide = false;
};
