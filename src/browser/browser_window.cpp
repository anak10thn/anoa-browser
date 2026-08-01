#include "browser/browser_window.h"

#include <QAction>
#include <QClipboard>
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
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
    setWindowTitle(QStringLiteral("anoa-browser"));

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
    auto *toolbar = new QWidget(this);
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

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(toolbar);
    root->addWidget(m_view, 1);

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
    m_view->setMinimumSize(config.width, config.height);
    resize(config.width, config.height + toolbar->sizeHint().height());
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
