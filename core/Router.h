#pragma once
#include <QString>
#include <QStringList>

namespace odv {

// Auto-pick the model for a turn. The idea: a cheap, fast model answers the easy
// asks (arithmetic, "what is X", a rename) and a strong model is reserved for the
// hard ones (design, refactor, debugging, proofs) — so you neither wait on a big
// model for trivia nor hand a hard problem to a weak one.
//
// Classification is a transparent heuristic, NOT another model call: the whole
// point of routing is to be faster and cheaper, and paying an LLM round-trip just
// to decide which LLM to call would defeat it. Every decision carries a `reason`
// so the choice is never a black box.
struct RouteDecision {
    QString backend;  // "ollama" (routing is over local/cloud Ollama models)
    QString model;    // the chosen tag
    QString tier;     // "simple" | "moderate" | "hard"
    QString reason;   // human-readable why, e.g. "design/architecture keywords"
};

class Router {
public:
    // Decide which model should answer `prompt`.
    static RouteDecision pick(const QString& prompt);

    // The model configured (or defaulted) for each tier, given what is installed.
    // Config keys router.simple / router.moderate / router.hard override the
    // auto-derived defaults; each is a plain Ollama tag.
    static QString modelForTier(const QString& tier);

    // Just the classification, exposed for the UI's "why".
    static QString classify(const QString& prompt, QString* reason = nullptr);
};

}  // namespace odv
