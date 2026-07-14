#pragma once
#include <QMap>
#include <QString>
namespace odv {
// Token/usage meter from /api/chat prompt_eval_count / eval_count.
class Usage {
public:
    struct Tally {
        qint64 prompt = 0;
        qint64 eval = 0;
        qint64 total() const { return prompt + eval; }
    };

    static void record(const QString& model, int promptTokens, int evalTokens);
    static QString report();  // human-readable cumulative

    // Per-model totals right now, for computing a delta across a crew run.
    static QMap<QString, Tally> snapshot();
};
}  // namespace odv
