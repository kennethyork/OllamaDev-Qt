#pragma once
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

namespace odv {

// Layered config, same precedence as the PHP original so the two can share a
// home directory during the migration:
//
//   defaults  <  ~/.ollamadev/config.json  <  ~/.ollamadev/ade-prefs.json  <  env
//
// config.json stays MCP-only by convention; everything else the GUI touches
// goes to ade-prefs.json as flat dotted keys.
class Config {
public:
    static void load();

    static QJsonValue get(const QString& dottedKey, const QJsonValue& fallback = {});
    static QString str(const QString& dottedKey, const QString& fallback = {});
    static int integer(const QString& dottedKey, int fallback);
    static double number(const QString& dottedKey, double fallback);
    static bool boolean(const QString& dottedKey, bool fallback);

    // Persist to ade-prefs.json (flat dotted key). config.json is never written.
    static void setPref(const QString& dottedKey, const QJsonValue& value);

    static QString homeDir();     // ~/.ollamadev
    static QString dataDir();     // <cwd>/.ollamadev  (per-project state)
    static QString crewDir();     // ~/.ollamadev/crew
    static QString boardDir();    // ~/.ollamadev/board
    static QString terminalsDir();

private:
    static QJsonObject merged_;
    static bool loaded_;
};

}  // namespace odv
