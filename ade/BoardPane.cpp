#include "BoardPane.h"

#include <QCryptographicHash>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

#include "Crew.h"
#include "Theme.h"

namespace odv {
namespace {

QString esc(const QString& s) { return s.toHtmlEscaped(); }

}  // namespace

BoardPane::BoardPane(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    // ---- Director bar ----
    directorBar_ = new QWidget(this);
    auto* bar = new QHBoxLayout(directorBar_);
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(6);
    auto* icon = new QLabel(QStringLiteral("🧭"), directorBar_);
    steer_ = new QLineEdit(directorBar_);
    steer_->setPlaceholderText(tr("Steer a coder — e.g. \"2: focus on tests\" or \"all: …\""));
    auto* send = new QPushButton(tr("Send"), directorBar_);
    send->setProperty("cta", true);
    bar->addWidget(icon);
    bar->addWidget(steer_, 1);
    bar->addWidget(send);
    root->addWidget(directorBar_);
    directorBar_->hide();  // shown only while a run is active

    connect(send, &QPushButton::clicked, this, &BoardPane::sendSteer);
    connect(steer_, &QLineEdit::returnPressed, this, &BoardPane::sendSteer);

    // ---- columns ----
    columns_ = new QWidget(this);
    colsLayout_ = new QHBoxLayout(columns_);
    colsLayout_->setContentsMargins(0, 0, 0, 0);
    colsLayout_->setSpacing(8);
    root->addWidget(columns_, 1);

    timer_ = new QTimer(this);
    timer_->setInterval(1500);
    connect(timer_, &QTimer::timeout, this, &BoardPane::poll);

    rebuild();
}

void BoardPane::refreshTheme() {
    fingerprint_.clear();
    rebuild();
}

void BoardPane::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    poll();
    timer_->start();
}

void BoardPane::hideEvent(QHideEvent* e) {
    timer_->stop();
    QWidget::hideEvent(e);
}

// held → its own column; done → Done; todo → To-do; everything else (doing,
// flagged) → Doing. Same mapping as Tasks.crewCol().
QString BoardPane::colFor(const QString& state) {
    if (state == "held") return QStringLiteral("held");
    if (state == "done") return QStringLiteral("done");
    if (state == "todo") return QStringLiteral("todo");
    return QStringLiteral("doing");
}

void BoardPane::poll() {
    board_ = Crew::boardState();

    held_.clear();
    for (const Decision& d : Board::pending()) {
        if (d.kind != QLatin1String("crew_branch")) continue;
        Held h;
        h.decisionId = d.id;
        h.diff = d.detail;
        h.files = d.data.value("files").toArray().size();
        h.reason = d.data.value("reason").toString();
        held_.insert(d.data.value("n").toInt(), h);
    }

    // Rebuilding on every tick would fight the user: it would drop the diff they
    // just expanded and steal focus from the steer box. Hash what we render and
    // only rebuild when it actually differs.
    QByteArray sig = QJsonDocument(board_).toJson(QJsonDocument::Compact);
    for (auto it = held_.constBegin(); it != held_.constEnd(); ++it)
        sig += QByteArray::number(it.key()) + it->decisionId.toUtf8() +
               QByteArray::number(it->files);
    const QString fp = QString::fromLatin1(
        QCryptographicHash::hash(sig, QCryptographicHash::Sha1).toHex());
    if (fp == fingerprint_) return;
    fingerprint_ = fp;
    rebuild();
}

void BoardPane::rebuild() {
    directorBar_->setVisible(board_.value("active").toBool(false));

    while (QLayoutItem* item = colsLayout_->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }

    const QJsonArray subs = board_.value("subtasks").toArray();
    QVector<QPair<QString, QString>> cols{{QStringLiteral("todo"), tr("To-do")},
                                          {QStringLiteral("doing"), tr("Doing")},
                                          {QStringLiteral("done"), tr("Done")}};
    // The Held column exists only when something is actually held — otherwise it
    // is a permanently empty fourth column.
    bool anyHeld = false;
    for (const QJsonValue& v : subs)
        if (v.toObject().value("state").toString() == QLatin1String("held")) anyHeld = true;
    if (anyHeld) cols.append({QStringLiteral("held"), tr("Held")});

    for (const auto& c : cols) {
        QVector<QJsonObject> in;
        for (const QJsonValue& v : subs) {
            const QJsonObject s = v.toObject();
            if (colFor(s.value("state").toString()) == c.first) in.append(s);
        }
        QWidget* col = makeColumn(c.first, c.second, in.size());
        auto* body = col->findChild<QWidget*>(QStringLiteral("colBody"));
        auto* bodyLayout = qobject_cast<QVBoxLayout*>(body->layout());
        for (const QJsonObject& s : in) bodyLayout->addWidget(makeCard(s));
        if (in.isEmpty()) {
            auto* empty = new QLabel(QStringLiteral("—"), body);
            empty->setAlignment(Qt::AlignCenter);
            empty->setStyleSheet(
                QStringLiteral("color:%1;padding:12px;").arg(Theme::currentColors().faint.name()));
            bodyLayout->addWidget(empty);
        }
        bodyLayout->addStretch(1);
        colsLayout_->addWidget(col, 1);
    }
}

QWidget* BoardPane::makeColumn(const QString& key, const QString& label, int count) {
    const Theme::Colors c = Theme::currentColors();
    auto* col = new QFrame(columns_);
    col->setObjectName("boardCol");
    col->setStyleSheet(QStringLiteral("#boardCol{background:%1;border:1px solid %2;border-radius:6px;}")
                           .arg(c.bg2.name(), c.border.name()));
    auto* v = new QVBoxLayout(col);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(6);

    const QColor dot = key == "held"  ? c.warn
                       : key == "done" ? c.ok
                       : key == "doing" ? c.accent
                                        : c.faint;
    auto* head = new QLabel(
        QStringLiteral("<span style='color:%1'>●</span> <b>%2</b> "
                       "<span style='color:%3'>%4</span>")
            .arg(dot.name(), esc(label), c.faint.name(), QString::number(count)),
        col);
    v->addWidget(head);

    auto* scroll = new QScrollArea(col);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget(scroll);
    body->setObjectName("colBody");
    auto* bl = new QVBoxLayout(body);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(6);
    scroll->setWidget(body);
    v->addWidget(scroll, 1);
    return col;
}

QWidget* BoardPane::makeCard(const QJsonObject& s) {
    const Theme::Colors c = Theme::currentColors();
    const int n = s.value("n").toInt();
    const QString state = s.value("state").toString();
    const bool isHeld = state == QLatin1String("held");
    const Held h = held_.value(n);

    auto* card = new QFrame;
    card->setObjectName("card");
    card->setStyleSheet(
        QStringLiteral("#card{background:%1;border:1px solid %2;border-left:3px solid %3;"
                       "border-radius:5px;}")
            .arg(c.bg3.name(), c.border.name(), (isHeld ? c.warn : c.accent).name()));
    auto* v = new QVBoxLayout(card);
    v->setContentsMargins(8, 7, 8, 7);
    v->setSpacing(4);

    QString role;
    if (!s.value("role").toString().isEmpty() && s.value("role").toString() != "coder")
        role = QStringLiteral(" <span style='color:%1'>· %2</span>")
                   .arg(c.accent2.name(), esc(s.value("role").toString()));
    auto* title = new QLabel(QStringLiteral("🤖 <b>#%1 %2</b>%3")
                                 .arg(n)
                                 .arg(esc(s.value("title").toString()), role),
                             card);
    title->setWordWrap(true);
    v->addWidget(title);

    // backend + model, so a mixed crew (one local coder, three cloud) is legible.
    const QString engine = QStringLiteral("%1 · %2")
                               .arg(s.value("backend").toString(QStringLiteral("ollama")),
                                    s.value("model").toString(QStringLiteral("—")));
    auto* meta = new QLabel(
        QStringLiteral("<span style='color:%1'>%2</span>").arg(c.faint.name(), esc(engine)), card);
    meta->setWordWrap(true);
    v->addWidget(meta);

    QString stateLine;
    if (isHeld) {
        stateLine = QStringLiteral("⚠ held");
        if (!h.reason.isEmpty()) stateLine += " · " + esc(h.reason);
        if (h.files) stateLine += QStringLiteral(" · %1 file%2").arg(h.files).arg(h.files > 1 ? "s" : "");
    } else if (state == "doing") {
        stateLine = QStringLiteral("● working");
    } else {
        stateLine = esc(state);
    }
    auto* st = new QLabel(QStringLiteral("<span style='color:%1'>%2</span>")
                              .arg((isHeld ? c.warn : c.dim).name(), stateLine),
                          card);
    st->setWordWrap(true);
    v->addWidget(st);

    if (!isHeld || h.decisionId.isEmpty()) return card;

    // ---- held: inline diff + accept/discard ----
    auto* acts = new QHBoxLayout;
    acts->setSpacing(5);
    auto* diffBtn = new QPushButton(expanded_.contains(n) ? tr("⌃ hide") : tr("⌄ diff"), card);
    auto* accept = new QPushButton(
        h.files ? tr("✓ Accept (%1 files)").arg(h.files) : tr("✓ Accept"), card);
    accept->setProperty("cta", true);
    auto* discard = new QPushButton(tr("✕ Discard"), card);
    discard->setProperty("danger", true);
    acts->addWidget(diffBtn);
    acts->addWidget(accept);
    acts->addWidget(discard);
    acts->addStretch(1);
    v->addLayout(acts);

    auto* diff = new QTextEdit(card);
    diff->setReadOnly(true);
    diff->setLineWrapMode(QTextEdit::NoWrap);
    diff->setMinimumHeight(160);
    diff->setHtml(diffHtml(h.diff));
    diff->setVisible(expanded_.contains(n));
    if (h.diff.isEmpty()) diffBtn->setEnabled(false);
    v->addWidget(diff);

    connect(diffBtn, &QPushButton::clicked, this, [this, n, diff, diffBtn] {
        const bool open = !diff->isVisible();
        diff->setVisible(open);
        if (open) expanded_.insert(n);
        else expanded_.remove(n);
        diffBtn->setText(open ? tr("⌃ hide") : tr("⌄ diff"));
    });
    connect(accept, &QPushButton::clicked, this, [this, n] { acceptHeld(n); });
    connect(discard, &QPushButton::clicked, this, [this, n] { discardHeld(n); });
    return card;
}

// Accept writes the changeset into the project — the one path that touches the
// user's tree — so it reports what landed.
void BoardPane::acceptHeld(int n) {
    QString err;
    if (Crew::accept(n, &err)) emit statusMessage(tr("✓ accepted #%1 — files added to your folder").arg(n));
    else emit statusMessage(tr("⚠ accept #%1 failed: %2").arg(n).arg(err));
    fingerprint_.clear();
    poll();
}

void BoardPane::discardHeld(int n) {
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Discard branch"));
    box.setText(tr("Discard coder #%1's work? Its changeset is deleted and cannot be recovered.")
                    .arg(n));
    QPushButton* yes = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
    QPushButton* no = box.addButton(tr("Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(no);
    box.exec();
    if (box.clickedButton() != yes) return;

    QString err;
    if (Crew::discard(n, &err)) emit statusMessage(tr("✓ discarded #%1").arg(n));
    else emit statusMessage(tr("⚠ discard #%1 failed: %2").arg(n).arg(err));
    fingerprint_.clear();
    poll();
}

// Same colouriser as Decisions._diffHtml: +/-/@@ lines, nothing else. A real
// syntax highlighter would be the first grammar table in the codebase.
QString BoardPane::diffHtml(const QString& text) {
    const Theme::Colors c = Theme::currentColors();
    if (text.trimmed().isEmpty())
        return QStringLiteral("<span style='color:%1'>(no diff)</span>").arg(c.faint.name());

    QString out = QStringLiteral("<pre style='margin:0;font-family:monospace;font-size:11px'>");
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("\r?\n")));
    for (const QString& ln : lines) {
        QString color = c.dim.name();
        if (ln.startsWith('+') && !ln.startsWith(QLatin1String("+++"))) color = c.ok.name();
        else if (ln.startsWith('-') && !ln.startsWith(QLatin1String("---"))) color = c.err.name();
        else if (ln.startsWith('@')) color = c.accent.name();
        out += QStringLiteral("<span style='color:%1'>%2</span>\n")
                   .arg(color, esc(ln).isEmpty() ? QStringLiteral(" ") : esc(ln));
    }
    return out + QStringLiteral("</pre>");
}

// "<n>: msg" targets one coder, "all: msg" the whole crew, no prefix = coder 1.
// Same parser as Tasks.wireDirector().
void BoardPane::sendSteer() {
    const QString raw = steer_->text().trimmed();
    if (raw.isEmpty()) return;

    static const QRegularExpression re(
        QStringLiteral("^(\\d+|all|\\*|everyone)\\s*[:>\\-]\\s*(.+)$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(raw);

    int coder = 1;
    QString msg = raw;
    if (m.hasMatch()) {
        const QString who = m.captured(1);
        bool isNum = false;
        const int n = who.toInt(&isNum);
        coder = isNum ? n : 0;  // 0 = the whole crew
        msg = m.captured(2).trimmed();
    }

    QString err;
    if (Crew::steer(coder, msg, &err)) {
        steer_->clear();
        emit statusMessage(coder == 0 ? tr("🧭 steered the crew") : tr("🧭 steered coder %1").arg(coder));
    } else {
        emit statusMessage(tr("⚠ steer failed: %1").arg(err));
    }
}

}  // namespace odv
