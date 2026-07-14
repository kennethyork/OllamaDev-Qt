#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace odv {

// PLUGINS — a bundle of the extension points that already exist.
//
// The PHP "plugin system" downloaded a .php file into ~/.ollamadev/plugins and
// then never loaded it: no manifest, no hooks, no require, anywhere. So this is
// not a port; it is the design it never had.
//
// A plugin does NOT get to run arbitrary code of its own. It CONTRIBUTES to the
// four extension points the app already has and already trusts:
//
//   skills/     — skill folders, exactly as ~/.ollamadev/skills
//   commands/   — slash-command prompt templates
//   hooks[]     — shell commands fired at defined events   ← the dangerous one
//   mcp[]       — MCP servers to connect
//
// That is a deliberate limit, not a shortcut. There is no scripting runtime to
// embed (Qt is the only dependency) and dlopen-ing native code from the internet
// into a process that holds your API keys and can write to your repo is not a
// feature, it is a vulnerability. Everything a plugin can express here is
// something you could already have written by hand — a plugin just saves you
// writing it.
//
// SECURITY: an installed plugin is DISABLED until you say otherwise. Installing
// only ever writes files; it never executes anything from the plugin. Enabling is
// the moment a plugin's hooks become live shell commands, so that is where the
// consent lives — the CLI prints exactly what it is about to trust and asks.

struct PluginHook {
    QString event;
    QString command;
    QString matcher;
};

struct Plugin {
    QString name;
    QString version;
    QString description;
    QString homepage;
    QString dir;  // ~/.ollamadev/plugins/<name>
    bool enabled = false;

    QVector<PluginHook> hooks;
    QJsonObject mcp;  // name -> {command,args,url} — same shape as config mcpServers

    bool hasSkills = false;
    bool hasCommands = false;

    // A one-line "what this actually gets to do to you", for the enable prompt.
    QString capabilities() const;
};

class Plugins {
public:
    static QString dir();  // ~/.ollamadev/plugins

    static QVector<Plugin> all();      // installed, sorted by name
    static QVector<Plugin> active();   // enabled only
    static bool get(const QString& name, Plugin* out);

    // `source` is a local directory or an https git URL. Copies/clones it in,
    // validates the manifest, and leaves it DISABLED. Never executes plugin code.
    static bool install(const QString& source, QString* err, QString* installedName = nullptr);

    static bool remove(const QString& name, QString* err);
    static bool setEnabled(const QString& name, bool on, QString* err);

    // ---- what enabled plugins contribute -----------------------------------
    // Appended AFTER the user's own project/home dirs, so a plugin can never
    // shadow a skill or a command you wrote yourself.
    static QStringList skillDirs();
    static QStringList commandDirs();
    static QVector<PluginHook> hooksFor(const QString& event);
    static QJsonObject mcpServers();
};

}  // namespace odv
