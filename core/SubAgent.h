#pragma once
#include <QString>
namespace odv {
// Nested agent (task tool), readonly by default, recursion-capped. STUB.
class SubAgent {
public:
    static QString run(const QString& task, int depth = 0);
    static void registerTools();  // registers `task`/`subagent` into Tools
};
}  // namespace odv
