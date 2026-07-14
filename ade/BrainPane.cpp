#include "BrainPane.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QTimer>
#include <QVBoxLayout>

#include "Crew.h"
#include "Models.h"
#include "Router.h"
#include "Theme.h"
#include "Usage.h"

namespace odv {
namespace {

QColor tierColor(const QString& tier) {
    const Theme::Colors c = Theme::currentColors();
    if (tier == QLatin1String("simple")) return c.ok;
    if (tier == QLatin1String("hard")) return c.err;
    return c.accent;  // moderate
}

// One "lobe": a titled box that draws a tier and the model it maps to.
class Lobe : public QFrame {
public:
    Lobe(const QString& tier, QWidget* parent = nullptr) : QFrame(parent), tier_(tier) {
        setMinimumHeight(72);
        setFrameShape(QFrame::StyledPanel);
    }
    void setModel(const QString& m) {
        model_ = m;
        update();
    }
    void setActive(bool a) {
        active_ = a;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor col = tierColor(tier_);
        QRectF r = rect().adjusted(2, 2, -2, -2);
        p.setPen(QPen(col, active_ ? 2.5 : 1.0));
        p.setBrush(QColor(col.red(), col.green(), col.blue(), active_ ? 46 : 22));
        p.drawRoundedRect(r, 10, 10);

        p.setPen(Theme::currentColors().fg);
        QFont f = font();
        f.setBold(true);
        p.setFont(f);
        p.drawText(r.adjusted(12, 8, -12, 0), Qt::AlignLeft | Qt::AlignTop, tier_.toUpper());
        f.setBold(false);
        p.setFont(f);
        p.setPen(Theme::currentColors().dim);
        const QString sub = tier_ == QLatin1String("simple")   ? QStringLiteral("trivia, lookups")
                            : tier_ == QLatin1String("hard")   ? QStringLiteral("design, debug, proofs")
                                                               : QStringLiteral("general work");
        p.drawText(r.adjusted(12, 26, -12, 0), Qt::AlignLeft | Qt::AlignTop, sub);
        p.setPen(col);
        f.setBold(true);
        p.setFont(f);
        p.drawText(r.adjusted(12, 0, -12, -8), Qt::AlignLeft | Qt::AlignBottom,
                   model_.isEmpty() ? QStringLiteral("(no model)") : model_);
    }

private:
    QString tier_, model_;
    bool active_ = false;
};

class BrainWidget : public QWidget {
public:
    explicit BrainWidget(PaneHost& host, QWidget* parent = nullptr) : QWidget(parent), host_(host) {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(10, 10, 10, 10);
        root->setSpacing(10);

        auto* title = new QLabel(tr("🧠 Routing brain"), this);
        QFont tf = title->font();
        tf.setBold(true);
        tf.setPointSizeF(tf.pointSizeF() + 2);
        title->setFont(tf);
        root->addWidget(title);

        // The three lobes: tier → model it currently resolves to.
        auto* lobes = new QHBoxLayout;
        for (const QString& t : {QStringLiteral("simple"), QStringLiteral("moderate"),
                                 QStringLiteral("hard")}) {
            auto* l = new Lobe(t, this);
            lobes_.insert(t, l);
            lobes->addWidget(l);
        }
        root->addLayout(lobes);

        // Live classifier — type a prompt, watch the brain choose.
        auto* tryBox = new QGroupBox(tr("Try it — where would this go?"), this);
        auto* tl = new QVBoxLayout(tryBox);
        probe_ = new QLineEdit(tryBox);
        probe_->setPlaceholderText(tr("type a request…  e.g. \"design a lock-free queue\""));
        verdict_ = new QLabel(tryBox);
        verdict_->setWordWrap(true);
        tl->addWidget(probe_);
        tl->addWidget(verdict_);
        root->addWidget(tryBox);
        connect(probe_, &QLineEdit::textChanged, this, [this](const QString& s) { classify(s); });

        // The live crew's routed model plan.
        crewBox_ = new QGroupBox(tr("Crew model plan"), this);
        auto* cl = new QVBoxLayout(crewBox_);
        crewPlan_ = new QLabel(tr("no crew running"), crewBox_);
        crewPlan_->setTextFormat(Qt::RichText);
        cl->addWidget(crewPlan_);
        root->addWidget(crewBox_);

        // Token split — free local vs paid cloud.
        tokens_ = new QLabel(this);
        tokens_->setTextFormat(Qt::RichText);
        root->addWidget(tokens_);

        root->addStretch(1);

        refresh();
        auto* t = new QTimer(this);
        connect(t, &QTimer::timeout, this, [this] { refresh(); });
        t->start(1500);
    }

private:
    void classify(const QString& s) {
        if (s.trimmed().isEmpty()) {
            verdict_->clear();
            for (auto* l : lobes_) l->setActive(false);
            return;
        }
        const RouteDecision d = Router::pick(s);
        for (auto it = lobes_.constBegin(); it != lobes_.constEnd(); ++it)
            it.value()->setActive(it.key() == d.tier);
        verdict_->setText(tr("→ <b>%1</b> · <b>%2</b><br><span style='color:%3'>%4</span>")
                              .arg(d.tier.toUpper(), d.model,
                                   Theme::currentColors().dim.name(), d.reason));
    }

    void refresh() {
        // Tier→model map can change as models are pulled/removed.
        for (auto it = lobes_.constBegin(); it != lobes_.constEnd(); ++it)
            it.value()->setModel(Router::modelForTier(it.key()));

        // Crew plan.
        const QJsonObject board = Crew::boardState();
        const QJsonArray subs = board.value("subtasks").toArray();
        if (board.value("active").toBool() && !subs.isEmpty()) {
            QString html;
            for (const auto& v : subs) {
                const QJsonObject s = v.toObject();
                const QString route = s.value("route").toString();
                const QString col = tierColor(route.isEmpty() ? QStringLiteral("moderate") : route)
                                        .name();
                html += tr("<div>coder #%1 <span style='color:%2'>%3</span> · %4%5</div>")
                            .arg(s.value("n").toInt())
                            .arg(col, s.value("model").toString(), s.value("backend").toString(),
                                 route.isEmpty() ? QString()
                                                 : QStringLiteral(" · %1").arg(route));
            }
            crewPlan_->setText(html);
        } else {
            crewPlan_->setText(tr("<i>no crew running</i>"));
        }

        // Token split.
        qint64 local = 0, cloud = 0;
        const QMap<QString, Usage::Tally> snap = Usage::snapshot();
        for (auto it = snap.constBegin(); it != snap.constEnd(); ++it)
            (Models::isCloud(it.key()) ? cloud : local) += it.value().total();
        const qint64 total = local + cloud;
        if (total > 0) {
            tokens_->setText(
                tr("<b>Tokens (project):</b> %1 total · "
                   "<span style='color:%2'>%3%% free local</span> · %4%% cloud")
                    .arg(total)
                    .arg(Theme::currentColors().ok.name())
                    .arg(local * 100 / total)
                    .arg(cloud * 100 / total));
        } else {
            tokens_->setText(tr("<i>no tokens recorded yet</i>"));
        }
    }

    PaneHost& host_;
    QMap<QString, Lobe*> lobes_;
    QLineEdit* probe_ = nullptr;
    QLabel* verdict_ = nullptr;
    QGroupBox* crewBox_ = nullptr;
    QLabel* crewPlan_ = nullptr;
    QLabel* tokens_ = nullptr;
};

}  // namespace

PaneSpec makeBrainPaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("brain");
    s.title = QStringLiteral("🧠 Brain");
    s.group = QStringLiteral("Crew");
    s.singleton = true;
    s.factory = [](PaneHost& host) -> QWidget* { return new BrainWidget(host); };
    return s;
}

}  // namespace odv
