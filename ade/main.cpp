#include <QApplication>
#include <QFont>
#include <QFontDatabase>

#include "Config.h"
#include "MainWindow.h"
#include "Theme.h"
#include "Tools.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("OllamaDev");
    QCoreApplication::setApplicationName("OllamaDev ADE");

    // The UI is full of emoji (💬 🌿 👷) and every one of them was a tofu box. See
    // Theme::withEmoji — Qt does not fall back to a colour emoji font on its own.
    app.setFont(odv::Theme::withEmoji(app.font()));

    odv::Config::load();
    odv::Tools::registerAll();
    odv::Theme::apply(&app, odv::Theme::current());

    odv::MainWindow w;
    w.show();
    return app.exec();
}
