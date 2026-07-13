#include "WebSearch.h"

#include <QByteArray>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QUrl>
#include <QUrlQuery>
#include <atomic>

#include "Config.h"

namespace odv {
namespace {

// -1 = not overridden, fall through to config. 0/1 = the CLI said --no-web / --web.
std::atomic<int> g_webOverride{-1};

constexpr int kMaxTextChars = 200000;

struct HttpReply {
    int status = 0;
    QByteArray body;
    QString error;  // empty on success
};

// Blocking HTTP with the network stack owned by THIS frame, so a crew coder on a
// worker thread can search without touching anyone else's QNetworkAccessManager
// (the class is thread-affine).
HttpReply http(const QString& verb, const QUrl& url, const QByteArray& body,
               const QList<QPair<QByteArray, QByteArray>>& headers, int timeoutMs) {
    HttpReply r;
    if (!url.isValid() || url.host().isEmpty()) {
        r.error = QStringLiteral("invalid url");
        return r;
    }
    // Only real web schemes. Without this a model could hand `fetch` a file:// or
    // qrc: URL and use it as an arbitrary-file-read tool that skips the sandbox
    // confinement every filesystem tool goes through.
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) {
        r.error = QStringLiteral("refusing non-http(s) url scheme '%1'").arg(scheme);
        return r;
    }

    QNetworkAccessManager nam;
    QNetworkRequest req{url};
    // TLS stays verified. The PHP set CURLOPT_SSL_VERIFYPEER => false here, which
    // means any proxy on the path could have fed the model forged search results
    // or a forged page. Qt verifies the chain by default — do not "fix" a cert
    // error by relaxing this.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);  // never https → http
    req.setMaximumRedirectsAllowed(5);
    req.setTransferTimeout(timeoutMs);
    for (const auto& h : headers) req.setRawHeader(h.first, h.second);

    QEventLoop loop;
    QScopedPointer<QNetworkReply> reply(verb == QLatin1String("POST") ? nam.post(req, body)
                                                                      : nam.get(req));
    QObject::connect(reply.data(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    r.body = reply->readAll();
    r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError) r.error = reply->errorString();
    return r;
}

const char* kBrowserAgent = "Mozilla/5.0 (X11; Linux x86_64) ollamadev";

// Named + numeric HTML entities. Qt's decoder lives in QTextDocument (QtGui), and
// core may not link Gui, so we own the handful that actually show up in titles
// and snippets.
QString decodeEntities(QString s) {
    if (!s.contains(QLatin1Char('&'))) return s;

    static const QRegularExpression num(QStringLiteral("&#(x?)([0-9a-fA-F]+);"));
    QString out;
    int last = 0;
    auto it = num.globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next();
        out += s.mid(last, m.capturedStart() - last);
        bool ok = false;
        const uint cp = m.captured(1).isEmpty() ? m.captured(2).toUInt(&ok, 10)
                                                : m.captured(2).toUInt(&ok, 16);
        if (ok && cp > 0 && cp <= 0x10FFFF)
            out += QString::fromUcs4(reinterpret_cast<const char32_t*>(&cp), 1);
        last = m.capturedEnd();
    }
    out += s.mid(last);
    s = out;

    // &amp; is replaced LAST: doing it first would turn "&amp;lt;" into a literal
    // "<" that was never in the document.
    s.replace(QLatin1String("&lt;"), QStringLiteral("<"));
    s.replace(QLatin1String("&gt;"), QStringLiteral(">"));
    s.replace(QLatin1String("&quot;"), QStringLiteral("\""));
    s.replace(QLatin1String("&apos;"), QStringLiteral("'"));
    s.replace(QLatin1String("&nbsp;"), QStringLiteral(" "));
    s.replace(QLatin1String("&#039;"), QStringLiteral("'"));
    s.replace(QLatin1String("&amp;"), QStringLiteral("&"));
    return s;
}

QString stripTags(const QString& html) {
    static const QRegularExpression tag(QStringLiteral("<[^>]*>"));
    return QString(html).remove(tag);
}

QString cleanInline(const QString& html) {
    return decodeEntities(stripTags(html)).simplified();
}

// DuckDuckGo sometimes hands back a redirector (…/l/?uddg=<percent-encoded>) instead
// of the destination. Unwrap it so the model gets a URL it can actually fetch.
QString unwrapDdg(QString url) {
    url = decodeEntities(url);
    static const QRegularExpression uddg(QStringLiteral("[?&]uddg=([^&]+)"));
    const auto m = uddg.match(url);
    if (m.hasMatch()) url = QUrl::fromPercentEncoding(m.captured(1).toUtf8());
    if (url.startsWith(QLatin1String("//"))) url = QStringLiteral("https:") + url;
    return url;
}

SearchResult searchDuckDuckGo(const QString& query, int limit) {
    SearchResult r;
    r.provider = QStringLiteral("duckduckgo");

    QUrlQuery form;
    form.addQueryItem(QStringLiteral("q"), query);
    const HttpReply resp =
        http(QStringLiteral("POST"), QUrl(QStringLiteral("https://html.duckduckgo.com/html/")),
             form.toString(QUrl::FullyEncoded).toUtf8(),
             {{"Content-Type", "application/x-www-form-urlencoded"}, {"User-Agent", kBrowserAgent}},
             20000);
    if (!resp.error.isEmpty()) {
        r.error = resp.error;
        return r;
    }
    const QString html = QString::fromUtf8(resp.body);

    // The html endpoint's result anchor. Attribute order is not fixed (href comes
    // after class today), so match the class first and the href wherever it lands.
    static const QRegularExpression link(
        QStringLiteral("<a[^>]*class=\"result__a\"[^>]*href=\"([^\"]+)\"[^>]*>(.*?)</a>"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression snip(
        QStringLiteral("class=\"result__snippet\"[^>]*>(.*?)</a>"),
        QRegularExpression::DotMatchesEverythingOption);

    // Snippets are paired with titles by document order — the html endpoint emits
    // exactly one of each per result, in the same order.
    QStringList snippets;
    auto sit = snip.globalMatch(html);
    while (sit.hasNext()) snippets << cleanInline(sit.next().captured(1));

    auto lit = link.globalMatch(html);
    int i = 0;
    while (lit.hasNext() && r.hits.size() < limit) {
        const auto m = lit.next();
        SearchHit h;
        h.url = unwrapDdg(m.captured(1));
        h.title = cleanInline(m.captured(2));
        h.snippet = snippets.value(i);
        ++i;
        if (!h.url.isEmpty()) r.hits.append(h);
    }
    r.ok = !r.hits.isEmpty();
    if (!r.ok) r.error = QStringLiteral("no results (the html endpoint may have changed shape)");
    return r;
}

SearchResult searchSearxng(const QString& query, int limit, const QString& hostIn) {
    SearchResult r;
    r.provider = QStringLiteral("searxng");
    QString host = hostIn;
    while (host.endsWith(QLatin1Char('/'))) host.chop(1);
    if (host.isEmpty()) {
        r.error = QStringLiteral(
            "SearXNG selected but no instance configured. Set search.host "
            "(e.g. http://localhost:8888).");
        return r;
    }

    QUrl url(host + QStringLiteral("/search"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("q"), query);
    q.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    url.setQuery(q);

    const HttpReply resp = http(QStringLiteral("GET"), url, {}, {{"User-Agent", "ollamadev"}}, 20000);
    if (!resp.error.isEmpty()) {
        r.error = resp.error;
        return r;
    }
    const QJsonArray arr =
        QJsonDocument::fromJson(resp.body).object().value(QStringLiteral("results")).toArray();
    for (const QJsonValue& v : arr) {
        if (r.hits.size() >= limit) break;
        const QJsonObject o = v.toObject();
        SearchHit h;
        h.title = o.value(QStringLiteral("title")).toString().trimmed();
        h.url = o.value(QStringLiteral("url")).toString();
        h.snippet = o.value(QStringLiteral("content")).toString().trimmed();
        if (!h.url.isEmpty()) r.hits.append(h);
    }
    r.ok = !r.hits.isEmpty();
    if (!r.ok && r.error.isEmpty())
        r.error = QStringLiteral("no results from %1 (is the JSON format enabled?)").arg(host);
    return r;
}

SearchResult searchBrave(const QString& query, int limit, const QString& key) {
    SearchResult r;
    r.provider = QStringLiteral("brave");
    if (key.isEmpty()) {
        r.error = QStringLiteral(
            "Brave selected but no API key set. Set search.key in config or the "
            "BRAVE_API_KEY env var.");
        return r;
    }

    QUrl url(QStringLiteral("https://api.search.brave.com/res/v1/web/search"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("q"), query);
    q.addQueryItem(QStringLiteral("count"), QString::number(limit));
    url.setQuery(q);

    const HttpReply resp = http(QStringLiteral("GET"), url, {},
                                {{"Accept", "application/json"},
                                 {"X-Subscription-Token", key.toUtf8()},
                                 {"User-Agent", "ollamadev"}},
                                20000);
    if (!resp.error.isEmpty()) {
        r.error = resp.error;
        return r;
    }
    const QJsonArray arr = QJsonDocument::fromJson(resp.body)
                               .object()
                               .value(QStringLiteral("web"))
                               .toObject()
                               .value(QStringLiteral("results"))
                               .toArray();
    for (const QJsonValue& v : arr) {
        if (r.hits.size() >= limit) break;
        const QJsonObject o = v.toObject();
        SearchHit h;
        h.title = o.value(QStringLiteral("title")).toString().trimmed();
        h.url = o.value(QStringLiteral("url")).toString();
        // Brave marks the matched terms with <strong> inside the description.
        h.snippet = cleanInline(o.value(QStringLiteral("description")).toString());
        if (!h.url.isEmpty()) r.hits.append(h);
    }
    r.ok = !r.hits.isEmpty();
    if (!r.ok && r.error.isEmpty()) r.error = QStringLiteral("no results");
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------

bool WebSearch::webEnabled() {
    const int o = g_webOverride.load();
    if (o >= 0) return o != 0;  // --no-web wins over the config file for this run
    return Config::boolean(QStringLiteral("web.enabled"), true);
}

void WebSearch::setWebEnabled(bool on) { g_webOverride.store(on ? 1 : 0); }

SearchResult WebSearch::search(const QString& query, int limit, const QString& providerOverride) {
    SearchResult r;
    if (query.trimmed().isEmpty()) {
        r.error = QStringLiteral("missing query");
        return r;
    }
    limit = qBound(1, limit, 10);

    QString provider = providerOverride.trimmed().toLower();
    if (provider.isEmpty())
        provider = Config::str(QStringLiteral("search.provider"), QStringLiteral("duckduckgo"))
                       .trimmed()
                       .toLower();

    if (provider == QLatin1String("searxng") || provider == QLatin1String("searx"))
        return searchSearxng(query, limit, Config::str(QStringLiteral("search.host")));
    if (provider == QLatin1String("brave")) {
        QString key = Config::str(QStringLiteral("search.key"));
        if (key.isEmpty()) key = QString::fromLocal8Bit(qgetenv("BRAVE_API_KEY")).trimmed();
        return searchBrave(query, limit, key);
    }
    return searchDuckDuckGo(query, limit);
}

FetchedPage WebSearch::fetch(const QString& url, int timeoutSec) {
    FetchedPage p;
    if (url.trimmed().isEmpty()) {
        p.error = QStringLiteral("missing url");
        return p;
    }
    const HttpReply r = http(QStringLiteral("GET"), QUrl(url.trimmed()), {},
                             {{"User-Agent", kBrowserAgent}, {"Accept", "*/*"}},
                             qBound(1, timeoutSec, 120) * 1000);
    p.status = r.status;
    if (!r.error.isEmpty()) {
        p.error = r.error;
        return p;
    }
    if (r.body.isEmpty()) {
        p.error = QStringLiteral("empty response");
        return p;
    }

    // A binary body (pdf, image, tarball) would flood the model's context with
    // mojibake, so it is reported rather than decoded.
    if (r.body.left(4096).contains('\0')) {
        p.error = QStringLiteral("binary response (%1 bytes) — not text").arg(r.body.size());
        return p;
    }

    const QString body = QString::fromUtf8(r.body);
    p.text = htmlToText(body);
    if (p.text.size() > kMaxTextChars) {
        p.text = p.text.left(kMaxTextChars) +
                 QStringLiteral("\n…[truncated at %1 characters]").arg(kMaxTextChars);
    }
    p.ok = true;
    return p;
}

QString WebSearch::htmlToText(const QString& html) {
    // Not HTML at all (JSON, plain text, source) — hand it back untouched rather
    // than mangling it through a tag stripper.
    const QString head = html.left(2048).toLower();
    const bool looksHtml = head.contains(QLatin1String("<html")) ||
                           head.contains(QLatin1String("<!doctype html")) ||
                           head.contains(QLatin1String("<body")) ||
                           head.contains(QLatin1String("<div")) || head.contains(QLatin1String("<p>"));
    if (!looksHtml) return html.trimmed();

    QString s = html;
    // Script/style bodies are code, not prose, and comments are invisible to a
    // reader — all three must go before the tags are stripped or their contents
    // would survive as text.
    static const QRegularExpression script(
        QStringLiteral("<(script|style|noscript|template)\\b[^>]*>.*?</\\1>"),
        QRegularExpression::DotMatchesEverythingOption |
            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression comment(QStringLiteral("<!--.*?-->"),
                                            QRegularExpression::DotMatchesEverythingOption);
    s.remove(script);
    s.remove(comment);

    // Block-level tags become newlines so the text keeps its paragraph structure
    // instead of collapsing into one wall of words.
    static const QRegularExpression breaks(
        QStringLiteral("</?(br|p|div|li|tr|h[1-6]|section|article|header|footer|nav|ul|ol|"
                       "table|blockquote|pre)\\b[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    s.replace(breaks, QStringLiteral("\n"));

    s = decodeEntities(stripTags(s));

    QStringList lines;
    for (QString line : s.split(QLatin1Char('\n'))) {
        line = line.simplified();
        if (!line.isEmpty()) lines << line;
    }
    return lines.join(QLatin1Char('\n'));
}

}  // namespace odv
