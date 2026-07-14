#include "BrainPane.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVBoxLayout>

#include "Backend.h"
#include "Config.h"
#include "Crew.h"
#include "Models.h"
#include "Router.h"
#include "Theme.h"
#include "Usage.h"

namespace odv {
namespace {

// Every faculty of the crew's brain, in pipeline order. `optIn` parts (debate,
// dedupe, security) are drawn dimmer — they exist but only fire when asked for.
struct Part {
    QString key;
    QString label;
    QString role;    // what this faculty does, one line
    bool optIn;
};

const QVector<Part>& parts() {
    static const QVector<Part> p{
        {"researcher", "Researcher", "reads the codebase (read-only)", false},
        {"router",     "Router",     "picks the model for each role by difficulty", false},
        {"director",   "Director",   "decomposes the task into subtasks", false},
        {"roles",      "Roles",      "assigns a persona to each subtask", false},
        {"skills",     "Skills",     "loads know-how matched to the focus", false},
        {"coders",     "Coders",     "build in parallel sandboxes", false},
        {"auditor",    "Auditor",    "reviews every changeset", false},
        {"debate",     "Debate",     "advocate vs skeptic vs judge", true},
        {"dedupe",     "Dedupe",     "holds duplicated work", true},
        {"security",   "Security",   "read-only vulnerability hunt", true},
        {"secret",     "Secret gate", "never lands a leaked credential", false},
        {"overlap",    "Overlap guard", "first-writer-wins on a shared file", false},
        {"landing",    "Landing",    "copies accepted files into your folder", false},
        {"memory",     "Memory",     "remembers facts for next time", false},
    };
    return p;
}

// The painted brain map: all parts as connected nodes flowing top→bottom, the
// active one during a live run highlighted.
class BrainMap : public QWidget {
public:
    explicit BrainMap(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(int(parts().size()) * 46 + 20);
    }
    void setActive(const QString& key) {
        active_ = key;
        update();
    }
    void setCoderStates(const QStringList& states) {
        coderStates_ = states;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const Theme::Colors c = Theme::currentColors();

        const int n = int(parts().size());
        const qreal rowH = qreal(height() - 20) / n;
        const qreal x = 14, w = width() - 28;

        QVector<QRectF> boxes(n);
        for (int i = 0; i < n; ++i)
            boxes[i] = QRectF(x, 10 + i * rowH, w, rowH - 10);

        // Connecting spine between consecutive parts.
        p.setPen(QPen(c.border, 1.5));
        for (int i = 0; i + 1 < n; ++i) {
            const QPointF a(boxes[i].center().x(), boxes[i].bottom());
            const QPointF b(boxes[i + 1].center().x(), boxes[i + 1].top());
            p.drawLine(a, b);
        }

        for (int i = 0; i < n; ++i) {
            const Part& part = parts().at(i);
            const bool isActive = (part.key == active_);
            QColor accent = part.optIn ? c.warn : c.accent;
            if (part.key == "secret" || part.key == "overlap") accent = c.err;
            if (part.key == "router") accent = c.accent2;

            QRectF r = boxes[i];
            const int baseA = part.optIn ? 16 : 30;
            p.setPen(QPen(accent, isActive ? 2.5 : 1.0));
            p.setBrush(QColor(accent.red(), accent.green(), accent.blue(),
                              isActive ? 70 : baseA));
            p.drawRoundedRect(r, 9, 9);

            // Label + role.
            QFont f = font();
            f.setBold(true);
            p.setFont(f);
            p.setPen(part.optIn && !isActive ? c.dim : c.fg);
            p.drawText(r.adjusted(14, 4, -14, 0), Qt::AlignLeft | Qt::AlignTop, part.label);
            f.setBold(false);
            p.setFont(f);
            p.setPen(c.dim);
            QString role = part.role;
            if (part.optIn) role += QStringLiteral("   · opt-in");
            p.drawText(r.adjusted(14, 0, -14, -4), Qt::AlignLeft | Qt::AlignBottom, role);

            // Coders row shows the live per-coder state as dots.
            if (part.key == "coders" && !coderStates_.isEmpty()) {
                qreal dx = r.right() - 16;
                for (int s = coderStates_.size() - 1; s >= 0; --s) {
                    const QString st = coderStates_.at(s);
                    QColor dc = st == "done"    ? c.ok
                                : st == "held"  ? c.warn
                                : st == "doing" ? c.accent2
                                                : c.dim;
                    p.setPen(Qt::NoPen);
                    p.setBrush(dc);
                    p.drawEllipse(QPointF(dx, r.center().y()), 5, 5);
                    dx -= 15;
                }
            }
        }
    }

private:
    QString active_;
    QStringList coderStates_;
};

class BrainWidget : public QWidget {
public:
    explicit BrainWidget(PaneHost& host, QWidget* parent = nullptr) : QWidget(parent), host_(host) {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(10, 10, 10, 10);
        root->setSpacing(8);

        auto* title = new QLabel(tr("The crew's brain"), this);
        QFont tf = title->font();
        tf.setBold(true);
        tf.setPointSizeF(tf.pointSizeF() + 2);
        title->setFont(tf);
        root->addWidget(title);

        map_ = new BrainMap(this);
        root->addWidget(map_, 1);

        // Router tiers — pick the model each difficulty routes to. Writing to the
        // shared ade-prefs.json means the CLI's brain uses the same choices.
        auto* tierBox = new QGroupBox(tr("Brain — model per difficulty tier"), this);
        auto* form = new QFormLayout(tierBox);
        const QStringList models = installedModels();
        for (const QString& tier : {QStringLiteral("simple"), QStringLiteral("moderate"),
                                    QStringLiteral("hard")}) {
            auto* combo = new QComboBox(tierBox);
            combo->addItem(tr("(auto)"), QString());   // empty → auto-derived default
            for (const QString& m : models) combo->addItem(m, m);
            const QString saved = Config::str(QStringLiteral("router.%1").arg(tier));
            combo->setCurrentIndex(qMax(0, combo->findData(saved)));
            connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, tier](int) {
                Config::setPref(QStringLiteral("router.%1").arg(tier),
                                combo->currentData().toString());
                refresh();
            });
            tierCombos_.insert(tier, combo);
            const QString label = tier == QLatin1String("simple")   ? tr("Simple (trivia)")
                                  : tier == QLatin1String("hard")   ? tr("Hard (design/debug)")
                                                                    : tr("Moderate (general)");
            form->addRow(label, combo);
        }
        root->addWidget(tierBox);

        // The model each tier resolves to right now (incl. auto-derived defaults).
        tiers_ = new QLabel(this);
        tiers_->setTextFormat(Qt::RichText);
        tiers_->setWordWrap(true);
        root->addWidget(tiers_);

        // Live classifier.
        auto* tryBox = new QGroupBox(tr("Try the router — where would this go?"), this);
        auto* tl = new QVBoxLayout(tryBox);
        probe_ = new QLineEdit(tryBox);
        probe_->setPlaceholderText(tr("type a request…  e.g. \"design a lock-free queue\""));
        verdict_ = new QLabel(tryBox);
        verdict_->setTextFormat(Qt::RichText);
        verdict_->setWordWrap(true);
        tl->addWidget(probe_);
        tl->addWidget(verdict_);
        root->addWidget(tryBox);
        connect(probe_, &QLineEdit::textChanged, this, [this](const QString& s) { classify(s); });

        tokens_ = new QLabel(this);
        tokens_->setTextFormat(Qt::RichText);
        root->addWidget(tokens_);

        refresh();
        auto* t = new QTimer(this);
        connect(t, &QTimer::timeout, this, [this] { refresh(); });
        t->start(1500);
    }

private:
    static QStringList installedModels() {
        auto b = Backends::get(QStringLiteral("ollama"));
        return b ? b->models() : QStringList{};
    }

    void classify(const QString& s) {
        if (s.trimmed().isEmpty()) {
            verdict_->clear();
            return;
        }
        const RouteDecision d = Router::pick(s);
        verdict_->setText(tr("→ <b>%1</b> · <b>%2</b> &nbsp;<span style='color:%3'>%4</span>")
                              .arg(d.tier.toUpper(), d.model,
                                   Theme::currentColors().dim.name(), d.reason));
    }

    void refresh() {
        const Theme::Colors c = Theme::currentColors();
        auto chip = [&](const QString& tier, const QColor& col) {
            return tr("<b style='color:%1'>%2</b> %3")
                .arg(col.name(), tier.toUpper(), Router::modelForTier(tier));
        };
        tiers_->setText(tr("Router tiers: &nbsp; %1 &nbsp;·&nbsp; %2 &nbsp;·&nbsp; %3")
                            .arg(chip("simple", c.ok), chip("moderate", c.accent),
                                 chip("hard", c.err)));

        // Live crew state → which faculty is active + coder dots.
        const QJsonObject board = Crew::boardState();
        const QJsonArray subs = board.value("subtasks").toArray();
        QStringList states;
        QString active;
        if (board.value("active").toBool() && !subs.isEmpty()) {
            bool anyDoing = false, anyTodo = false, allDone = true;
            for (const auto& v : subs) {
                const QString st = v.toObject().value("state").toString();
                states << st;
                if (st == "doing") anyDoing = true;
                if (st == "todo") anyTodo = true;
                if (st != "done") allDone = false;
            }
            active = anyDoing ? "coders" : anyTodo ? "director" : allDone ? "landing" : "auditor";
        }
        map_->setActive(active);
        map_->setCoderStates(states);

        qint64 local = 0, cloud = 0;
        const QMap<QString, Usage::Tally> snap = Usage::snapshot();
        for (auto it = snap.constBegin(); it != snap.constEnd(); ++it)
            (Models::isCloud(it.key()) ? cloud : local) += it.value().total();
        const qint64 total = local + cloud;
        tokens_->setText(total > 0
                             ? tr("<b>Tokens:</b> %1 · <span style='color:%2'>%3%% free "
                                  "local</span> · %4%% cloud")
                                   .arg(total)
                                   .arg(c.ok.name())
                                   .arg(local * 100 / total)
                                   .arg(cloud * 100 / total)
                             : tr("<i>no tokens recorded yet</i>"));
    }

    PaneHost& host_;
    BrainMap* map_ = nullptr;
    QMap<QString, QComboBox*> tierCombos_;
    QLabel* tiers_ = nullptr;
    QLineEdit* probe_ = nullptr;
    QLabel* verdict_ = nullptr;
    QLabel* tokens_ = nullptr;
};

}  // namespace

PaneSpec makeBrainPaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("brain");
    s.title = QStringLiteral("Brain");
    s.group = QStringLiteral("Crew");
    s.singleton = true;
    s.factory = [](PaneHost& host) -> QWidget* { return new BrainWidget(host); };
    return s;
}

}  // namespace odv
