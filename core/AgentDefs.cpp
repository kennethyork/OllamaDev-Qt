#include "AgentDefs.h"
namespace odv {
// STUB.
QStringList AgentDefs::list() { return {}; }
AgentDef AgentDefs::get(const QString&) { return {}; }
QString OutputStyles::suffix(const QString&) { return {}; }
QStringList OutputStyles::names() { return {"default","concise","explanatory","formal","bullets"}; }
QString StatusLine::render(const QString&, const QString&) { return {}; }
}  // namespace odv
