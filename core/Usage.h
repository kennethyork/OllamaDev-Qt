#pragma once
#include <QString>
namespace odv {
// Token/usage meter from /api/chat counts. STUB.
class Usage {
public:
    static void record(const QString& model, int promptTokens, int evalTokens);
    static QString report();  // human-readable cumulative
};
}  // namespace odv
