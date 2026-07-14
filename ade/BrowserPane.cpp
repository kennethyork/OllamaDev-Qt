#include "BrowserPane.h"

#include <QHBoxLayout>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include "Theme.h"

// Two browsers behind one pane, chosen at build time:
//   ODV_HAS_WEBENGINE  → QWebEngineView, a real Chromium engine (JS, CSS, media).
//   otherwise          → a QTextBrowser reader (no JS), so a build without the
//                        WebEngine module still has a working Browser pane.
// The CMake option ODV_WEBENGINE (default ON) finds QtWebEngine and defines the
// macro; install qt6-webengine-dev to switch a fallback build to the full engine.
#if defined(ODV_HAS_WEBENGINE)
#include <QWebEngineHistory>
#include <QWebEngineView>
#else
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTextBrowser>
#include <QTextCursor>
#endif

namespace odv {
namespace {

QUrl toUrl(const QString& text) {
    QString s = text.trimmed();
    if (s.isEmpty()) return {};
    // Bare host → assume https; a term with no dot and no scheme is a search.
    if (!s.contains(QStringLiteral("://"))) {
        if (s.contains(QLatin1Char(' ')) || !s.contains(QLatin1Char('.')))
            return QUrl(QStringLiteral("https://duckduckgo.com/?q=") +
                        QString::fromUtf8(QUrl::toPercentEncoding(s)));
        s.prepend(QStringLiteral("https://"));
    }
    return QUrl::fromUserInput(s);
}

#if defined(ODV_HAS_WEBENGINE)

// The real browser.
class BrowserWidget : public QWidget {
public:
    explicit BrowserWidget(PaneHost& host, QWidget* parent = nullptr)
        : QWidget(parent), host_(host) {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(6, 6, 6, 6);
        root->setSpacing(6);

        auto* bar = new QHBoxLayout;
        back_ = new QPushButton(QStringLiteral("◀"), this);
        fwd_ = new QPushButton(QStringLiteral("▶"), this);
        reload_ = new QPushButton(QStringLiteral("⟳"), this);
        url_ = new QLineEdit(this);
        url_->setPlaceholderText(tr("Search or enter a URL"));
        auto* goBtn = new QPushButton(tr("Go"), this);
        back_->setEnabled(false);
        fwd_->setEnabled(false);
        bar->addWidget(back_);
        bar->addWidget(fwd_);
        bar->addWidget(reload_);
        bar->addWidget(url_, 1);
        bar->addWidget(goBtn);
        root->addLayout(bar);

        view_ = new QWebEngineView(this);
        root->addWidget(view_, 1);

        progress_ = new QProgressBar(this);
        progress_->setMaximumHeight(3);
        progress_->setTextVisible(false);
        progress_->hide();
        root->addWidget(progress_);

        connect(url_, &QLineEdit::returnPressed, this, [this] { navigate(url_->text()); });
        connect(goBtn, &QPushButton::clicked, this, [this] { navigate(url_->text()); });
        connect(back_, &QPushButton::clicked, view_, &QWebEngineView::back);
        connect(fwd_, &QPushButton::clicked, view_, &QWebEngineView::forward);
        connect(reload_, &QPushButton::clicked, view_, &QWebEngineView::reload);

        connect(view_, &QWebEngineView::urlChanged, this, [this](const QUrl& u) {
            url_->setText(u.toString());
            syncNav();
        });
        connect(view_, &QWebEngineView::loadStarted, this, [this] {
            progress_->setValue(0);
            progress_->show();
        });
        connect(view_, &QWebEngineView::loadProgress, this,
                [this](int p) { progress_->setValue(p); });
        connect(view_, &QWebEngineView::loadFinished, this, [this](bool ok) {
            progress_->hide();
            syncNav();
            host_.setStatus(ok ? tr("loaded %1").arg(view_->url().host())
                               : tr("load failed"));
        });
        connect(view_, &QWebEngineView::titleChanged, this,
                [this](const QString& t) { host_.setStatus(t); });

        view_->setUrl(QUrl(QStringLiteral("https://duckduckgo.com")));
    }

private:
    void navigate(const QString& text) {
        const QUrl u = toUrl(text);
        if (u.isValid()) view_->setUrl(u);
    }
    void syncNav() {
        back_->setEnabled(view_->history()->canGoBack());
        fwd_->setEnabled(view_->history()->canGoForward());
    }

    PaneHost& host_;
    QLineEdit* url_ = nullptr;
    QWebEngineView* view_ = nullptr;
    QPushButton* back_ = nullptr;
    QPushButton* fwd_ = nullptr;
    QPushButton* reload_ = nullptr;
    QProgressBar* progress_ = nullptr;
};

#else  // ---- QTextBrowser reader fallback (no QtWebEngine) --------------------

class BrowserWidget : public QWidget {
public:
    explicit BrowserWidget(PaneHost& host, QWidget* parent = nullptr)
        : QWidget(parent), host_(host) {
        net_ = new QNetworkAccessManager(this);

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(6, 6, 6, 6);
        root->setSpacing(6);

        auto* bar = new QHBoxLayout;
        back_ = new QPushButton(QStringLiteral("◀"), this);
        fwd_ = new QPushButton(QStringLiteral("▶"), this);
        url_ = new QLineEdit(this);
        url_->setPlaceholderText(tr("Enter a URL — reader view (no JavaScript)"));
        auto* goBtn = new QPushButton(tr("Go"), this);
        back_->setEnabled(false);
        fwd_->setEnabled(false);
        bar->addWidget(back_);
        bar->addWidget(fwd_);
        bar->addWidget(url_, 1);
        bar->addWidget(goBtn);
        root->addLayout(bar);

        view_ = new QTextBrowser(this);
        view_->setOpenLinks(false);
        view_->setOpenExternalLinks(false);
        root->addWidget(view_, 1);

        connect(url_, &QLineEdit::returnPressed, this, [this] { navigate(toUrl(url_->text()), true); });
        connect(goBtn, &QPushButton::clicked, this, [this] { navigate(toUrl(url_->text()), true); });
        connect(back_, &QPushButton::clicked, this, [this] { back(); });
        connect(fwd_, &QPushButton::clicked, this, [this] { forward(); });
        connect(view_, &QTextBrowser::anchorClicked, this,
                [this](const QUrl& u) { navigate(current().resolved(u), true); });

        view_->setHtml(tr("<h3>Reader</h3><p>Type a URL above. This build has no "
                          "QtWebEngine, so this is a JavaScript-free reader. Install "
                          "qt6-webengine-dev and rebuild for the full browser.</p>"));
    }

private:
    QUrl current() const { return cur_ >= 0 && cur_ < hist_.size() ? hist_[cur_] : QUrl(); }
    void back() {
        if (cur_ <= 0) return;
        --cur_;
        load(current());
        syncNav();
    }
    void forward() {
        if (cur_ + 1 >= hist_.size()) return;
        ++cur_;
        load(current());
        syncNav();
    }
    void navigate(const QUrl& u, bool push) {
        if (!u.isValid() || (u.scheme() != QLatin1String("http") &&
                             u.scheme() != QLatin1String("https"))) {
            host_.setStatus(tr("only http/https URLs are supported"));
            return;
        }
        if (push) {
            if (cur_ + 1 < hist_.size()) hist_.resize(cur_ + 1);
            hist_.push_back(u);
            cur_ = hist_.size() - 1;
        }
        load(u);
        syncNav();
    }
    void load(const QUrl& u) {
        url_->setText(u.toString());
        host_.setStatus(tr("loading %1…").arg(u.host()));
        QNetworkRequest req(u);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("OllamaDev-Reader/1.0 (Qt; no-JS)"));
        req.setTransferTimeout(15000);
        QNetworkReply* reply = net_->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, u] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                view_->setHtml(QStringLiteral("<h3>%1</h3><p>%2</p>")
                                   .arg(tr("Could not load page").toHtmlEscaped(),
                                        reply->errorString().toHtmlEscaped()));
                return;
            }
            view_->document()->setBaseUrl(u);
            const QString ctype =
                reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
            const QByteArray body = reply->readAll();
            if (ctype.contains(QStringLiteral("html")) || ctype.isEmpty())
                view_->setHtml(QString::fromUtf8(body));
            else
                view_->setPlainText(QString::fromUtf8(body));
            view_->moveCursor(QTextCursor::Start);
            host_.setStatus(tr("loaded %1").arg(u.host()));
        });
    }
    void syncNav() {
        back_->setEnabled(cur_ > 0);
        fwd_->setEnabled(cur_ + 1 < hist_.size());
    }

    PaneHost& host_;
    QNetworkAccessManager* net_ = nullptr;
    QLineEdit* url_ = nullptr;
    QTextBrowser* view_ = nullptr;
    QPushButton* back_ = nullptr;
    QPushButton* fwd_ = nullptr;
    QVector<QUrl> hist_;
    int cur_ = -1;
};

#endif

}  // namespace

PaneSpec makeBrowserPaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("browser");
    s.title = QStringLiteral("Browser");
    s.group = QStringLiteral("Views");
    s.singleton = true;
    s.factory = [](PaneHost& host) -> QWidget* { return new BrowserWidget(host); };
    return s;
}

}  // namespace odv
