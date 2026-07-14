#include "ContextTuner.h"

#include <QByteArray>
#include <QFile>
#include <QLocale>
#include <QProcess>
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <memory>

#include "Backend.h"
#include "Config.h"
#include "OllamaBackend.h"

namespace odv {
namespace {

// Total system RAM in bytes (Linux /proc/meminfo; macOS sysctl hw.memsize), or 0.
qint64 ramBytes() {
    QFile mi(QStringLiteral("/proc/meminfo"));
    if (mi.open(QIODevice::ReadOnly)) {
        const QString s = QString::fromUtf8(mi.readAll());
        mi.close();
        static const QRegularExpression re(QStringLiteral("MemTotal:\\s+(\\d+)\\s*kB"));
        const auto m = re.match(s);
        if (m.hasMatch()) return m.captured(1).toLongLong() * 1024;
    }
    QProcess p;
    p.start(QStringLiteral("sysctl"), {QStringLiteral("-n"), QStringLiteral("hw.memsize")});
    if (p.waitForFinished(1500)) {
        bool ok = false;
        const qint64 n = QString::fromUtf8(p.readAllStandardOutput()).trimmed().toLongLong(&ok);
        if (ok) return n;
    }
    return 0;
}

// Largest GPU's VRAM in bytes via nvidia-smi, or 0 (CPU / non-NVIDIA / unknown).
qint64 vramBytes() {
    QProcess p;
    p.start(QStringLiteral("nvidia-smi"), {QStringLiteral("--query-gpu=memory.total"),
                                           QStringLiteral("--format=csv,noheader,nounits")});
    if (!p.waitForFinished(2000)) {
        p.kill();
        p.waitForFinished(500);
        return 0;
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) return 0;
    qint64 maxMiB = 0;
    for (const QString& line : QString::fromUtf8(p.readAllStandardOutput()).split(QLatin1Char('\n'))) {
        bool ok = false;
        const qint64 v = line.trimmed().toLongLong(&ok);
        if (ok) maxMiB = std::max(maxMiB, v);
    }
    return maxMiB * 1024 * 1024;  // MiB → bytes
}

// Recommend a num_ctx that fits in `budget` after the model weights, clamped to
// the model's native max. Conservative ~6k tokens of KV per free GB; rounded to a
// 4k multiple. Mirrors the PHP tuner so the two agree on a machine.
int suggestCtx(qint64 budget, qint64 modelBytes, int modelMaxCtx) {
    if (budget <= 0) return 16384;  // unknown hardware → safe default
    const qint64 overhead = 1500000000;  // runtime + activations headroom
    const qint64 free = budget - modelBytes - overhead;
    if (free <= 0) return 4096;
    const qint64 tokens = qint64((double(free) / 1e9) * 6000.0);
    const int ceil = modelMaxCtx > 0 ? modelMaxCtx : 131072;
    qint64 n = std::max<qint64>(4096, std::min<qint64>({tokens, qint64(ceil), 131072}));
    n = (n / 4096) * 4096;
    return int(n <= 0 ? 4096 : n);
}

struct Probe {
    qint64 ram = 0, vram = 0, budget = 0, modelBytes = 0;
    int modelMax = 0, suggested = 0;
    QString model;
};

Probe probe() {
    Probe p;
    p.ram = ramBytes();
    p.vram = vramBytes();
    p.budget = p.vram > 0 ? p.vram : p.ram;
    p.model = Config::str(QStringLiteral("ollama.defaultModel"));

    if (const auto be = std::dynamic_pointer_cast<OllamaBackend>(Backends::get("ollama"))) {
        if (!p.model.isEmpty()) {
            p.modelMax = be->contextLength(p.model);
            // Weight size is only known once the model is resident (/api/ps); 0 is
            // fine, the suggestion just gets a little more conservative.
            const QJsonObject ps = be->psInfo(p.model);
            p.modelBytes = qint64(ps.value(QStringLiteral("size")).toDouble());
        }
    }
    p.suggested = suggestCtx(p.budget, p.modelBytes, p.modelMax);
    return p;
}

QString gb(qint64 b) {
    return b > 0 ? QStringLiteral("%1 GB").arg(double(b) / 1e9, 0, 'f', 1)
                 : QStringLiteral("unknown");
}

}  // namespace

int ContextTuner::suggest() { return probe().suggested; }

bool ContextTuner::lowResourceMachine() {
    // nvidia-smi is a subprocess and chatOptions() runs on every request, so this is
    // answered once per process.
    static const bool low = [] {
        const qint64 gib = 1024LL * 1024 * 1024;
        const qint64 vram = vramBytes();

        // 8GB+ of VRAM comfortably holds a 9-13B model with a large KV cache. This
        // is not a low-resource machine, and pretending it is costs the user most of
        // their context window for no reason at all.
        if (vram >= 8 * gib) return false;

        // No usable GPU: it will run on the CPU, where RAM is the constraint. 32GB is
        // plenty to be generous with; below that, be careful.
        if (vram == 0) return ramBytes() < 32 * gib;

        // A small GPU (<8GB) genuinely is tight once the weights are resident.
        return true;
    }();
    return low;
}

QString ContextTuner::report() {
    const Probe p = probe();
    const QLocale loc = QLocale::system();
    const QString where = p.vram > 0 ? QStringLiteral("GPU VRAM") : QStringLiteral("system RAM");

    QString o = QStringLiteral("\nOllamaDev — context tuner\n");
    o += QString(46, QChar(0x2500)) + QLatin1Char('\n');
    o += QStringLiteral("  System RAM       %1\n").arg(gb(p.ram));
    o += QStringLiteral("  GPU VRAM         %1\n")
             .arg(p.vram > 0 ? gb(p.vram) : QStringLiteral("none detected"));
    o += QStringLiteral("  Budget (%1) %2\n").arg(where, gb(p.budget));
    QString modelLine = p.model.isEmpty() ? QStringLiteral("(none)") : p.model;
    modelLine += QStringLiteral(" · weights ") + gb(p.modelBytes);
    if (p.modelMax > 0)
        modelLine += QStringLiteral(" · native max %1 tok").arg(loc.toString(p.modelMax));
    o += QStringLiteral("  Model            %1\n").arg(modelLine);

    const bool autoCtx = Config::boolean(QStringLiteral("ollama.autoContext"), true);
    const int curMax = Config::integer(QStringLiteral("ollama.maxContextWindow"), 32768);
    const int curBase = Config::integer(QStringLiteral("ollama.contextWindow"), 16384);
    o += QStringLiteral("  Current num_ctx  %1 tok\n")
             .arg(autoCtx ? QStringLiteral("auto → up to %1").arg(loc.toString(curMax))
                          : QStringLiteral("pinned %1").arg(loc.toString(curBase)));

    o += QStringLiteral("\n  Suggested: %1 tokens (estimate — KV cache scales with num_ctx)\n")
             .arg(loc.toString(p.suggested));
    o += QStringLiteral("  Set it for a session:  ollamadev --num-ctx %1\n").arg(p.suggested);
    o += QStringLiteral("  Or persist in ~/.ollamadev/config.json: "
                        "\"ollama\": { \"maxContextWindow\": %1 }\n")
             .arg(p.suggested);
    if (p.budget == 0)
        o += QStringLiteral("  ⚠ couldn't read memory — suggestion is a safe default; tune to "
                            "taste.\n");
    o += QStringLiteral("  Bigger = more room but more memory + slower. For tasks too big for any "
                        "window, split with `crew`.\n\n");
    return o;
}

}  // namespace odv
