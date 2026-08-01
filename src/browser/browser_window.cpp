#include "browser/browser_window.h"

#include <QAction>
#include <QClipboard>
#include <QColor>
#include <QCursor>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPoint>
#include <QResizeEvent>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEngineHistory>

#include "browser/anoa_browser.h"
#include "common/url_input.h"

namespace {

// The toolbar in one place. Colours are the flat greys of the macOS reference:
// a light chrome strip, mid-grey glyphs, and a white pill for the address that
// gains a blue ring only while it has focus.
const char kToolbarStyle[] = R"(
QWidget#anoaToolbar {
    background: #ECECEC;
    border-bottom: 1px solid #D6D6D6;
}
QToolButton {
    border: none;
    background: transparent;
    color: #6E6E6E;
    padding: 0 6px;
}
QToolButton:hover  { color: #303030; }
QToolButton:pressed { color: #101010; }
QToolButton:disabled { color: #C2C2C2; }
QToolButton::menu-indicator { image: none; }
QLineEdit {
    background: #FFFFFF;
    border: 1px solid #DCDCDC;
    border-radius: 14px;
    padding: 4px 10px;
    color: #202020;
    selection-background-color: #B4D5FE;
}
QLineEdit:focus { border: 1px solid #4A90D9; }
)";

} // namespace

// Renders one character into a pixmap so it can sit inside the address field.
// QLineEdit::addAction takes a QIcon and nothing else, and shipping icon files
// for six glyphs would put binary assets in a repository that has none.
QIcon BrowserWindow::glyphIcon(const QString &glyph, int pointSize)
{
    QFont font;
    font.setPointSize(pointSize);
    const QFontMetrics fm(font);
    const int side = qMax(fm.height(), fm.horizontalAdvance(glyph)) + 2;

    QPixmap pixmap(side, side);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(font);
    painter.setPen(QColor(0x8A, 0x8A, 0x8A));
    painter.drawText(pixmap.rect(), Qt::AlignCenter, glyph);
    painter.end();
    return QIcon(pixmap);
}

QToolButton *BrowserWindow::makeGlyphButton(const QString &glyph, const QString &tip, int pointSize)
{
    auto *button = new QToolButton(this);
    button->setText(glyph);
    button->setToolTip(tip);
    button->setCursor(Qt::ArrowCursor);
    button->setAutoRaise(true);
    QFont font = button->font();
    font.setPointSize(pointSize);
    button->setFont(font);
    return button;
}

BrowserWindow::BrowserWindow(AnoaBrowser *view, const Config &config, QWidget *parent)
    : QWidget(parent)
    , m_view(view)
{
    setWindowTitle(QStringLiteral("anoa"));

    m_back = makeGlyphButton(QStringLiteral("‹"), // SINGLE LEFT-POINTING QUOTATION MARK
                             QStringLiteral("Back (Alt+Left)"), 26);
    m_forward = makeGlyphButton(QStringLiteral("›"), // SINGLE RIGHT-POINTING QUOTATION MARK
                                QStringLiteral("Forward (Alt+Right)"), 26);
    auto *reload = makeGlyphButton(QStringLiteral("⟳"), // CLOCKWISE GAPPED CIRCLE ARROW
                                   QStringLiteral("Reload (Ctrl+R)"), 20);

    m_urlEdit = new QLineEdit(this);
    m_urlEdit->setPlaceholderText(QString());
    m_urlEdit->setFrame(false);
    // The magnifier sits inside the pill on the left, and the bookmark star
    // inside it on the right, exactly as in the reference. addAction() is what
    // puts them *within* the field's rounded rect rather than beside it.
    m_urlEdit->addAction(glyphIcon(QStringLiteral("⌕"), 15), // TELEPHONE RECORDER (magnifier)
                         QLineEdit::LeadingPosition);
    m_star = m_urlEdit->addAction(glyphIcon(QStringLiteral("☆"), 16), // WHITE STAR
                                  QLineEdit::TrailingPosition);
    m_star->setToolTip(QStringLiteral("Bookmark this page"));
    connect(m_star, &QAction::triggered, this, &BrowserWindow::onBookmark);

    m_menuButton = makeGlyphButton(QStringLiteral("☰"), // TRIGRAM FOR HEAVEN (hamburger)
                                   QStringLiteral("Menu"), 17);
    m_menu = new QMenu(this);
    m_menuButton->setMenu(m_menu);
    m_menuButton->setPopupMode(QToolButton::InstantPopup);
    rebuildMenu();

    // A real widget rather than a bare layout, so the strip can carry its own
    // background and bottom rule. A QHBoxLayout has nothing to paint.
    m_toolbar = new QWidget(this);
    QWidget *toolbar = m_toolbar;
    toolbar->setObjectName(QStringLiteral("anoaToolbar"));
    auto *bar = new QHBoxLayout(toolbar);
    bar->setContentsMargins(8, 6, 8, 6);
    bar->setSpacing(2);
    bar->addWidget(m_back);
    bar->addWidget(m_forward);
    bar->addWidget(reload);
    bar->addSpacing(6);
    bar->addWidget(m_urlEdit, 1);
    bar->addSpacing(4);
    bar->addWidget(m_menuButton);
    setStyleSheet(QString::fromLatin1(kToolbarStyle));

    // The toolbar is deliberately NOT in the layout. Only the view is, and the
    // toolbar is positioned by hand across the top with the layout's top margin
    // reserving the space for it.
    //
    // That indirection is what makes auto-hide possible without touching the
    // view's geometry: revealing the bar over the page must not resize the
    // page. HttpServer reports the view's size as the viewport that
    // /render/click coordinates are measured in, so a toolbar that pushed the
    // view down and up again on every hover would move every click target with
    // it and reflow the page twice a second.
    m_root = new QVBoxLayout(this);
    m_root->setContentsMargins(0, 0, 0, 0);
    m_root->setSpacing(0);
    m_root->addWidget(m_view, 1);
    toolbar->raise();

    connect(m_back, &QToolButton::clicked, m_view, &AnoaBrowser::back);
    connect(m_forward, &QToolButton::clicked, m_view, &AnoaBrowser::forward);
    connect(reload, &QToolButton::clicked, m_view, &AnoaBrowser::reload);
    connect(m_urlEdit, &QLineEdit::returnPressed, this, &BrowserWindow::onUrlEntered);
    connect(m_view, &AnoaBrowser::urlChanged, this, &BrowserWindow::onUrlChanged);
    connect(m_view, &AnoaBrowser::loadFinished, this,
            [this](bool) { refreshHistoryButtons(); });

    refreshHistoryButtons();

    // The view keeps the size the config asked for; the window is whatever that
    // plus the toolbar comes to. Sizing the window to config.width/height
    // instead would quietly hand the page a shorter viewport than requested.
    // Polls the pointer instead of filtering mouse events. Once the toolbar is
    // hidden the whole window is the web view, and WebEngine delivers pointer
    // events inside its own render widget rather than up the parent chain, so
    // an event filter here would simply never see the pointer reach the top
    // edge. Polling is immune to that, and at 16 Hz it costs nothing.
    m_pointerTimer = new QTimer(this);
    m_pointerTimer->setInterval(60);
    connect(m_pointerTimer, &QTimer::timeout, this, &BrowserWindow::pollPointer);

    m_view->setMinimumSize(config.width, config.height);
    resize(config.width, config.height + toolbar->sizeHint().height());
    layoutToolbar();
}

void BrowserWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    layoutToolbar();
}

void BrowserWindow::layoutToolbar()
{
    if (!m_toolbar)
        return;
    const int barHeight = m_toolbar->sizeHint().height();
    m_toolbar->setGeometry(0, 0, width(), barHeight);
    // Overlaying costs the view nothing; docked, the margin is what keeps the
    // page out from under the bar.
    m_root->setContentsMargins(0, m_autoHide ? 0 : barHeight, 0, 0);
    m_toolbar->raise();
}

void BrowserWindow::setAutoHide(bool on)
{
    m_autoHide = on;
    if (on) {
        m_toolbar->hide();
        m_pointerTimer->start();
    } else {
        m_pointerTimer->stop();
        m_toolbar->show();
    }
    layoutToolbar();
}

void BrowserWindow::pollPointer()
{
    if (!m_autoHide)
        return;

    const int barHeight = m_toolbar->sizeHint().height();
    const QPoint local = mapFromGlobal(QCursor::pos());
    const bool insideHorizontally = local.x() >= 0 && local.x() < width();

    // Two different thresholds on purpose. Revealing takes a deliberate move
    // into the top few pixels; hiding waits until the pointer is clear of the
    // whole bar. One shared threshold would flicker the bar on and off while
    // the pointer sat on the boundary.
    static constexpr int kRevealZone = 3;
    if (!m_toolbar->isVisible()) {
        if (insideHorizontally && local.y() >= 0 && local.y() <= kRevealZone) {
            m_toolbar->show();
            m_toolbar->raise();
        }
        return;
    }

    // Keep it up while it is being used: the pointer is on it, a menu is open,
    // or the address field has focus and is being typed into.
    if (m_urlEdit->hasFocus() || (m_menu && m_menu->isVisible()))
        return;
    if (insideHorizontally && local.y() >= 0 && local.y() < barHeight)
        return;
    m_toolbar->hide();
}

// Session-only, and deliberately so: there is no bookmark store anywhere in
// this project, and inventing a file format for one is a larger decision than a
// toolbar button should make. The list lives as long as the window does.
void BrowserWindow::onBookmark()
{
    const QUrl current = m_view->url();
    if (current.isEmpty())
        return;
    const QString label = m_view->title().isEmpty() ? current.toString() : m_view->title();
    for (const auto &existing : m_bookmarks) {
        if (existing.second == current)
            return; // already starred; starring twice is not two bookmarks
    }
    m_bookmarks.append({label, current});
    rebuildMenu();
}

void BrowserWindow::rebuildMenu()
{
    m_menu->clear();
    m_menu->addAction(QStringLiteral("Back"), m_view, &AnoaBrowser::back);
    m_menu->addAction(QStringLiteral("Forward"), m_view, &AnoaBrowser::forward);
    m_menu->addAction(QStringLiteral("Reload"), m_view, &AnoaBrowser::reload);
    m_menu->addSeparator();
    m_menu->addAction(QStringLiteral("Copy address"), this, [this]() {
        QGuiApplication::clipboard()->setText(m_view->url().toString());
    });

    m_menu->addSeparator();
    // Rebuilt with the menu, so it has to carry its state rather than assume
    // the fresh action's default.
    m_autoHideAction = m_menu->addAction(QStringLiteral("Auto-hide toolbar"));
    m_autoHideAction->setCheckable(true);
    m_autoHideAction->setChecked(m_autoHide);
    m_autoHideAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H));
    connect(m_autoHideAction, &QAction::toggled, this, &BrowserWindow::setAutoHide);

    m_menu->addSeparator();
    if (m_bookmarks.isEmpty()) {
        QAction *empty = m_menu->addAction(QStringLiteral("No bookmarks"));
        empty->setEnabled(false);
    } else {
        for (const auto &bookmark : m_bookmarks) {
            const QUrl target = bookmark.second;
            m_menu->addAction(bookmark.first, this, [this, target]() { m_view->load(target); });
        }
    }
}

BrowserWindow::~BrowserWindow()
{
    if (m_view) {
        m_view->hide(); // so releasing it does not flash a bare top-level view
        m_view->setParent(nullptr);
    }
}

void BrowserWindow::onUrlEntered()
{
    const QString url = normalizeUserUrl(m_urlEdit->text());
    if (url.isEmpty())
        return;
    m_view->load(QUrl(url));
    m_view->setFocus();
}

void BrowserWindow::onUrlChanged(const QUrl &url)
{
    // Not while it is being typed into: a redirect landing mid-edit would
    // otherwise replace what the user is still writing.
    if (m_urlEdit->hasFocus())
        return;
    m_urlEdit->setText(url.toString());
}

void BrowserWindow::refreshHistoryButtons()
{
    m_back->setEnabled(m_view->history()->canGoBack());
    m_forward->setEnabled(m_view->history()->canGoForward());
}
