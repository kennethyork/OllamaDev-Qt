#include "BrowserPane.h"

#include <QHBoxLayout>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QTextBrowser>
#include <QTextCursor>
#include <QVBoxLayout>

#include "Theme.h"

namespace odv {
namespace {

// A deliberately lightweight web reader. QtWebEngine is banned (it drags in a
// whole Chromium — the opposite of this app's zero-dependency, Qt-only rule), so
// this is QTextBrowser fed by QNetworkAccessManager: it fetches a page and renders
// the HTML QTextBrowser understands (a rich-text subset). The tradeoff is
// explicit and intentional — NO JavaScript, no CSS layout engine, no media. It is
// a reader for docs, READMEs, and API pages, not an app runtime. Back/forward is
// our own URL stack (QTextBrowser's own history tracks in-document anchors, not
// network fetches).
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
        url_->setPlaceholderText(tr("Enter a URL — a reader view (no JavaScript)"));
        auto* goBtn = new QPushButton(tr("Go"), this);
        back_->setEnabled(false);
        fwd_->setEnabled(false);
        bar->addWidget(back_);
        bar->addWidget(fwd_);
        bar->addWidget(url_, 1);
        bar->addWidget(goBtn);
        root->addLayout(bar);

        view_ = new QTextBrowser(this);
        view_->setOpenLinks(false);   // route every click through our fetcher instead
        view_->setOpenExternalLinks(false);
        root->addWidget(view_, 1);

        connect(url_, &QLineEdit::returnPressed, this, [this] { go(url_->text()); });
        connect(goBtn, &QPushButton::clicked, this, [this] { go(url_->text()); });
        connect(back_, &QPushButton::clicked, this, [this] { back(); });
        connect(fwd_, &QPushButton::clicked, this, [this] { forward(); });
        // A link click resolves against the page we're on and fetches it.
        connect(view_, &QTextBrowser::anchorClicked, this, [this](const QUrl& u) {
            navigate(current().resolved(u), true);
        });

        view_->setHtml(tr("<h3>Reader</h3><p>Type a URL above. This is a lightweight, "
                          "JavaScript-free reader — the zero-dependency alternative to a bundled "
                          "browser engine.</p>"));
    }

private:
    QUrl current() const { return cur_ >= 0 && cur_ < hist_.size() ? hist_[cur_] : QUrl(); }

    void go(const QString& text) {
        QString s = text.trimmed();
        if (s.isEmpty()) return;
        // Bare host → assume https; that is the common case for a reader.
        if (!s.contains(QStringLiteral("://"))) s.prepend(QStringLiteral("https://"));
        navigate(QUrl::fromUserInput(s), true);
    }

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

    void navigate(const QUrl& u, bool pushHistory) {
        if (!u.isValid() || (u.scheme() != QLatin1String("http") &&
                             u.scheme() != QLatin1String("https"))) {
            host_.setStatus(tr("only http/https URLs are supported"));
            return;
        }
        if (pushHistory) {
            // Truncate any forward entries — a new navigation forks the history.
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
                host_.setStatus(tr("load failed: %1").arg(reply->errorString()));
                return;
            }
            const QByteArray body = reply->readAll();
            const QString ctype =
                reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
            // Base URL lets QTextBrowser resolve relative links for display; clicks
            // are re-resolved in anchorClicked before fetching.
            view_->document()->setBaseUrl(u);
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

}  // namespace

PaneSpec makeBrowserPaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("browser");
    s.title = QStringLiteral("🌐 Browser");
    s.group = QStringLiteral("Views");
    s.singleton = true;
    s.factory = [](PaneHost& host) -> QWidget* { return new BrowserWidget(host); };
    return s;
}

}  // namespace odv
