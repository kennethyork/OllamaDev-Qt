#include "ThemeDialog.h"

#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QVector>

#include "Theme.h"

namespace odv {
namespace {

// Role id (must match Theme::saveCustom's order) + the human label the editor
// shows for it. Same fifteen roles the Colors struct carries.
struct Role {
    const char* label;
};
const Role kRoles[] = {{"Background"}, {"Surface"},    {"Raised"},    {"Elevated"}, {"Text"},
                       {"Dim text"},   {"Faint text"}, {"Border"},    {"Accent"},   {"Accent 2"},
                       {"Success"},    {"Warning"},    {"Error"},     {"Canvas"},   {"Canvas grid"}};

// Assemble the 15 hex fields into a Colors, in the struct's declared order.
Theme::Colors readColors(const QVector<QLineEdit*>& edits) {
    Theme::Colors c;
    QColor* f[] = {&c.bg,     &c.bg2,   &c.bg3,    &c.elev,   &c.fg,
                   &c.dim,    &c.faint, &c.border, &c.accent, &c.accent2,
                   &c.ok,     &c.warn,  &c.err,    &c.canvas, &c.canvasGrid};
    for (int i = 0; i < 15; ++i) {
        QColor col(edits[i]->text().trimmed());
        *f[i] = col.isValid() ? col : QColor(QStringLiteral("#000000"));
    }
    return c;
}

void spreadColors(const Theme::Colors& c, QColor out[15]) {
    const QColor f[] = {c.bg,  c.bg2,   c.bg3,    c.elev,   c.fg,
                        c.dim, c.faint, c.border, c.accent, c.accent2,
                        c.ok,  c.warn,  c.err,    c.canvas, c.canvasGrid};
    for (int i = 0; i < 15; ++i) out[i] = f[i];
}

}  // namespace

void ThemeDialog::open(QWidget* parent) {
    auto* dlg = new QDialog(parent);
    dlg->setWindowTitle(QObject::tr("Theme editor"));
    dlg->setModal(true);
    dlg->resize(460, 640);
    dlg->setAttribute(Qt::WA_DeleteOnClose);

    // Remember what was showing so Cancel can put it back — the live preview
    // repaints the whole app, and a cancelled edit must not leave it altered.
    const QString original = Theme::current();

    auto* v = new QVBoxLayout(dlg);

    auto* top = new QGridLayout;
    top->addWidget(new QLabel(QObject::tr("Start from")), 0, 0);
    auto* base = new QComboBox;
    for (const QString& id : Theme::names()) base->addItem(Theme::label(id), id);
    base->setCurrentIndex(qMax(0, base->findData(original)));
    top->addWidget(base, 0, 1);
    top->addWidget(new QLabel(QObject::tr("Name")), 1, 0);
    auto* name = new QLineEdit(Theme::label(original) + QObject::tr(" (custom)"));
    top->addWidget(name, 1, 1);
    v->addLayout(top);

    // The fifteen colour rows in a scroll area — swatch + hex, both editable.
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* fieldsHost = new QWidget;
    auto* grid = new QGridLayout(fieldsHost);
    QVector<QLineEdit*> edits;
    QVector<QPushButton*> swatches;
    for (int i = 0; i < 15; ++i) {
        grid->addWidget(new QLabel(QObject::tr(kRoles[i].label)), i, 0);
        auto* sw = new QPushButton;
        sw->setFixedSize(30, 22);
        auto* hex = new QLineEdit;
        hex->setMaxLength(7);
        hex->setFixedWidth(90);
        grid->addWidget(sw, i, 1);
        grid->addWidget(hex, i, 2);
        edits << hex;
        swatches << sw;
    }
    scroll->setWidget(fieldsHost);
    v->addWidget(scroll, 1);

    auto paintSwatch = [swatches](int i, const QColor& col) {
        swatches[i]->setStyleSheet(
            QStringLiteral("background-color:%1; border:1px solid #888; border-radius:3px;")
                .arg(col.name()));
    };

    // Push the current field values into the app as a preview.
    auto preview = [edits] { Theme::applyColors(qApp, readColors(edits)); };

    // Seed all fifteen fields from a theme id.
    auto seed = [=](const QString& id) {
        QColor cols[15];
        spreadColors(Theme::colors(id), cols);
        for (int i = 0; i < 15; ++i) {
            edits[i]->setText(cols[i].name());
            paintSwatch(i, cols[i]);
        }
    };
    seed(original);

    // Wire each row: typing a hex or picking from the colour dialog previews live.
    for (int i = 0; i < 15; ++i) {
        QLineEdit* hex = edits[i];
        QObject::connect(hex, &QLineEdit::textChanged, dlg, [=](const QString& t) {
            QColor col(t.trimmed());
            if (col.isValid()) {
                paintSwatch(i, col);
                preview();
            }
        });
        QObject::connect(swatches[i], &QPushButton::clicked, dlg, [=] {
            QColor start(hex->text().trimmed());
            QColor picked = QColorDialog::getColor(start.isValid() ? start : Qt::black, dlg,
                                                   QObject::tr("Pick colour"));
            if (picked.isValid()) hex->setText(picked.name());  // textChanged does the rest
        });
    }

    QObject::connect(base, &QComboBox::currentIndexChanged, dlg, [=](int idx) {
        seed(base->itemData(idx).toString());
        preview();
    });

    auto* box = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto* save = box->addButton(QObject::tr("Save theme"), QDialogButtonBox::AcceptRole);
    save->setProperty("cta", true);
    v->addWidget(box);

    // Cancel restores the theme that was live before the editor opened.
    QObject::connect(box, &QDialogButtonBox::rejected, dlg, [=] {
        Theme::apply(qApp, original);
        if (QWidget* w = parent ? parent->window() : nullptr)
            QMetaObject::invokeMethod(w, "onThemeChanged", Qt::QueuedConnection,
                                      Q_ARG(QString, original));
        dlg->reject();
    });
    QObject::connect(save, &QPushButton::clicked, dlg, [=] {
        const QString id = Theme::saveCustom(name->text().trimmed(), readColors(edits));
        // Route through the window's slot so the canvas and board repaint too —
        // applyColors alone only recolours the standard-widget chrome.
        if (QWidget* w = parent ? parent->window() : nullptr)
            QMetaObject::invokeMethod(w, "onThemeChanged", Qt::QueuedConnection, Q_ARG(QString, id));
        else Theme::apply(qApp, id);
        dlg->accept();
    });

    dlg->show();
}

}  // namespace odv
