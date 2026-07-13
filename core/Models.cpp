#include "Models.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>
#include <limits>

#include "Config.h"

namespace odv {
namespace {

QString baseOf(const QString& tag) {
    const QString t = tag.trimmed();
    const int colon = t.indexOf(u':');
    return colon < 0 ? t : t.left(colon);
}

// The size token lives AFTER the colon (…:7b, :14b, :30b-q5); before it sits the
// family version (qwen2.5, llama3.2), which must never be read as a size.
double sizeFromToken(const QString& text) {
    // MoE first: 8x7b is experts x size (~56B), not 7B. Matching the plain "<n>b"
    // rule first would understate it by an order of magnitude and make escalation
    // pick a "bigger" model that is in fact smaller.
    static const QRegularExpression moe(QStringLiteral(R"((\d+)\s*x\s*(\d+(?:\.\d+)?)\s*b\b)"),
                                        QRegularExpression::CaseInsensitiveOption);
    if (const auto m = moe.match(text); m.hasMatch()) {
        return m.captured(1).toDouble() * m.captured(2).toDouble();
    }
    static const QRegularExpression plain(QStringLiteral(R"((\d+(?:\.\d+)?)\s*b\b)"),
                                          QRegularExpression::CaseInsensitiveOption);
    if (const auto m = plain.match(text); m.hasMatch()) return m.captured(1).toDouble();
    return 0.0;
}

}  // namespace

QVector<Preset> Models::presets() {
    return {
        {"qwen3.5", "qwen3.5:9b", "~6.6 GB", true, false, "agentic coding",
         "Recommended default — reliable native tool-calling, fits 24 GB / CPU-only. Backs every crew role."},
        {"qwen2.5-coder", "qwen2.5-coder:7b", "~4.7 GB", true, false, "agentic coding",
         "Best all-round local coder for tool use."},
        {"qwen2.5-coder-14b", "qwen2.5-coder:14b", "~9 GB", true, false, "agentic coding",
         "Stronger reasoning; needs more VRAM."},
        {"qwen2.5-coder-32b", "qwen2.5-coder:32b", "~20 GB", true, false, "agentic coding",
         "Top local coder; for big GPUs / lots of RAM."},
        {"llama3.1", "llama3.1:8b", "~4.9 GB", true, false, "general + tools",
         "Solid tool-caller and generalist."},
        {"mistral", "mistral:latest", "~4.1 GB", true, false, "general + tools",
         "Fast, dependable tool-calling."},
        {"codestral", "codestral:latest", "~13 GB", true, false, "coding",
         "Strong code model with tool support."},
        {"deepseek-coder-v2", "deepseek-coder-v2:16b", "~9 GB", true, false, "coding",
         "Good code completion and edits."},
        {"llama3.2", "llama3.2:latest", "~2 GB", false, false, "small / chat",
         "Tiny & fast; tool use is unreliable — chat only."},
        {"llava", "llava:7b", "~4.7 GB", false, true, "vision",
         "Image understanding."},
        {"llama3.2-vision", "llama3.2-vision:11b", "~7.8 GB", false, true, "vision",
         "Stronger image understanding; needs more VRAM."},
        {"moondream", "moondream:latest", "~1.7 GB", false, true, "vision / tiny",
         "Tiny, fast vision model for quick image Q&A."},
        {"nomic-embed-text", "nomic-embed-text", "~270 MB", false, false, "embeddings",
         "Powers semantic code search."},
    };
}

QVector<Preset> Models::cloudPresets() {
    return {
        {"qwen3-coder-cloud", "qwen3-coder:480b-cloud", "", true, false, "agentic coding",
         "Frontier coding agent (480B) — top cloud pick for the crew."},
        {"gpt-oss-cloud", "gpt-oss:120b-cloud", "", true, false, "general + tools",
         "Strong open generalist with reliable tool-calling."},
        {"gpt-oss-mini-cloud", "gpt-oss:20b-cloud", "", true, false, "general + tools",
         "Smaller, faster gpt-oss — cheaper cloud turns."},
        {"deepseek-cloud", "deepseek-v3.1:671b-cloud", "", true, false, "reasoning + coding",
         "Big reasoning/coding model (671B)."},
        {"qwen3-max-cloud", "qwen3-max:cloud", "", true, false, "general + tools",
         "Qwen3 flagship, cloud-hosted."},
        {"glm-cloud", "glm-4.6:cloud", "", true, false, "coding", "GLM-4.6 — capable coder."},
        {"kimi-cloud", "kimi-k2:1t-cloud", "", true, false, "general",
         "Kimi K2 (1T) — very large general model."},
        {"minimax-cloud", "minimax-m2:cloud", "", true, false, "general",
         "MiniMax M2 — general assistant."},
    };
}

bool Models::isCloud(const QString& tag) {
    const QString t = tag.trimmed().toLower();
    if (t.isEmpty()) return false;
    return t.endsWith(QLatin1String("-cloud")) || t.endsWith(QLatin1String(":cloud"));
}

QString Models::firstCloud(const QStringList& models) {
    for (const QString& m : models) {
        if (isCloud(m)) return m;
    }
    return {};
}

QString Models::match(const QString& want, const QStringList& installed) {
    const QString w = want.trimmed();
    if (w.isEmpty()) return {};
    if (installed.contains(w)) return w;

    const QString latest = w + QStringLiteral(":latest");
    if (!w.contains(u':') && installed.contains(latest)) return latest;

    // Family prefix: "qwen2.5-coder" should find an installed "qwen2.5-coder:7b".
    const QString base = baseOf(w);
    for (const QString& m : installed) {
        if (m == w || m == base + QStringLiteral(":latest") ||
            m.startsWith(base + QStringLiteral(":"))) {
            return m;
        }
    }
    return {};
}

int Models::toolsSupported(const QString& tag) {
    const QString base = baseOf(tag);
    const auto check = [&](const QVector<Preset>& list) -> int {
        for (const Preset& p : list) {
            if (p.tag == tag || baseOf(p.tag) == base) return p.tools ? 1 : 0;
        }
        return -1;
    };
    const int local = check(presets());
    if (local >= 0) return local;
    return check(cloudPresets());
}

QStringList Models::defaultChain() {
    return {"qwen3.5:9b",         "qwen2.5-coder:7b",  "llama3.1:8b",
            "mistral:latest",     "qwen2.5-coder:14b", "codestral:latest",
            "deepseek-coder-v2:16b"};
}

QStringList Models::chain() {
    const QJsonValue v = Config::get(QStringLiteral("model.fallback"));
    QStringList out;
    if (v.isString()) {
        // CSV is accepted because it is what a shell-set config key looks like.
        for (const QString& part : v.toString().split(u',', Qt::SkipEmptyParts)) {
            const QString t = part.trimmed();
            if (!t.isEmpty()) out << t;
        }
    } else if (v.isArray()) {
        for (const QJsonValue& e : v.toArray()) {
            const QString t = e.toString().trimmed();
            if (!t.isEmpty()) out << t;
        }
    }
    return out.isEmpty() ? defaultChain() : out;
}

double Models::paramSizeB(const QString& tag) {
    const QString t = tag.trimmed();
    if (t.isEmpty()) return 0.0;

    const int colon = t.indexOf(u':');
    const double direct = sizeFromToken(colon < 0 ? t : t.mid(colon + 1));
    if (direct > 0.0) return direct;

    // No size in the tag (`qwen2.5-coder:latest`, plain `mistral`) — take it from
    // the catalogued preset for the family, whose tag does carry one. Without this
    // the very common `:latest` pull would be unrankable and escalation would give
    // up on it.
    const QString base = baseOf(t);
    for (const Preset& p : presets()) {
        if (baseOf(p.tag) != base) continue;
        const double s = sizeFromToken(p.tag);
        if (s > 0.0) return s;
    }
    return 0.0;
}

QString Models::escalate(const QString& current, const QStringList& installed) {
    const QString cur = current.trimmed();

    const QJsonValue ladderVal = Config::get(QStringLiteral("models.escalation"));
    if (ladderVal.isArray()) {
        QStringList ladder;
        for (const QJsonValue& e : ladderVal.toArray()) {
            const QString t = e.toString().trimmed();
            if (!t.isEmpty()) ladder << t;
        }
        if (!ladder.isEmpty()) {
            int pos = ladder.indexOf(cur);
            if (pos < 0) {
                // Alias-aware: a ladder written as `qwen2.5-coder` still has to find
                // a resolved `qwen2.5-coder:7b`, or an explicit ladder is silently
                // ignored — the worst possible outcome for a config key.
                for (int i = 0; i < ladder.size(); ++i) {
                    if (match(ladder.at(i), {cur}) == cur) {
                        pos = i;
                        break;
                    }
                }
            }
            if (pos >= 0) {
                for (int i = pos + 1; i < ladder.size(); ++i) {
                    const QString hit = match(ladder.at(i), installed);
                    if (!hit.isEmpty()) return hit;
                }
                return {};  // on the ladder, but nothing bigger is installed
            }
        }
    }

    const double curSize = paramSizeB(cur);
    if (curSize <= 0.0) return {};  // unrankable: escalating blind would be a coin flip

    QString best;
    double bestSize = std::numeric_limits<double>::max();
    for (const QString& tag : installed) {
        if (tag == cur) continue;
        const double s = paramSizeB(tag);
        if (s <= curSize || s >= bestSize) continue;
        bestSize = s;
        best = tag;
    }
    return best;
}

QString Models::bestInstalled(const QStringList& installed) {
    for (const QString& want : chain()) {
        const QString hit = match(want, installed);
        if (!hit.isEmpty()) return hit;
    }
    // Nothing from the chain: settle for anything catalogued as tool-capable
    // rather than handing back a model that cannot run the agent loop at all.
    for (const QString& m : installed) {
        if (toolsSupported(m) == 1) return m;
    }
    return {};
}

QString Models::toolFallback(const QStringList& installed, const QString& current) {
    for (const QString& want : chain()) {
        const QString hit = match(want, installed);
        if (!hit.isEmpty() && hit != current) return hit;
    }
    for (const QString& m : installed) {
        if (m != current && toolsSupported(m) == 1) return m;
    }
    return {};
}

QString Models::resolveTag(const QString& nameOrAlias) {
    const QString n = nameOrAlias.trimmed();
    for (const Preset& p : presets()) {
        if (p.alias == n) return p.tag;
    }
    for (const Preset& p : cloudPresets()) {
        if (p.alias == n) return p.tag;
    }
    return n;  // already a tag, or unknown — pass it through to the daemon
}

}  // namespace odv
