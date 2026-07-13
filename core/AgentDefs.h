#pragma once
#include <QString>
#include <QStringList>
namespace odv {
// File-defined agent personas (.ollamadev/agents/*.md), output styles, statusline. STUB.
struct AgentDef { QString name, description, model, permission, prompt; QStringList tools; };
class AgentDefs {
public:
    static QStringList list();
    static AgentDef get(const QString& name);
};
class OutputStyles { public: static QString suffix(const QString& style); static QStringList names(); };
class StatusLine { public: static QString render(const QString& model, const QString& mode); };
}  // namespace odv
