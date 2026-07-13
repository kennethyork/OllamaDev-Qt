#include "Onboard.h"

#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>

#include <cstdio>

#include "Backend.h"
#include "Config.h"
#include "Models.h"
#include "Puller.h"

namespace odv {
namespace {

QTextStream& out() {
    static QTextStream s(stdout);
    return s;
}

QString cyan(const QString& s) { return QStringLiteral("\033[36m") + s + QStringLiteral("\033[0m"); }
QString dim(const QString& s) { return QStringLiteral("\033[2m") + s + QStringLiteral("\033[0m"); }
QString green(const QString& s) { return QStringLiteral("\033[32m") + s + QStringLiteral("\033[0m"); }

// nvidia-smi flags verified against `nvidia-smi --help` / `--help-query-gpu`:
// --query-gpu=memory.total is a valid field, csv is mandatory, noheader/nounits
// drop the header row and the "MiB" suffix. Read-only probe, so cwd is irrelevant.
double nvidiaVramGb() {
    QProcess p;
    p.start(QStringLiteral("nvidia-smi"),
            {QStringLiteral("--query-gpu=memory.total"),
             QStringLiteral("--format=csv,noheader,nounits")});
    if (!p.waitForStarted(3000)) return 0.0;  // no NVIDIA driver / binary
    if (!p.waitForFinished(5000)) {
        p.kill();  // our own child handle, not a kill-by-name
        p.waitForFinished(1000);
        return 0.0;
    }
    const QString outText = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    double maxMib = 0.0;
    for (const QString& line : outText.split(u'\n', Qt::SkipEmptyParts)) {
        bool ok = false;
        const double mib = line.trimmed().toDouble(&ok);
        if (ok && mib > maxMib) maxMib = mib;
    }
    return maxMib > 0 ? maxMib / 1024.0 : 0.0;  // MiB -> GiB
}

double systemRamGb() {
    QFile f(QStringLiteral("/proc/meminfo"));
    if (!f.open(QIODevice::ReadOnly)) return 0.0;
    const QString text = QString::fromUtf8(f.readAll());
    f.close();
    static const QRegularExpression re(QStringLiteral(R"(MemTotal:\s+(\d+)\s*kB)"));
    const auto m = re.match(text);
    if (!m.hasMatch()) return 0.0;
    return m.captured(1).toDouble() / 1048576.0;  // kB -> GiB
}

}  // namespace

Onboard::HwInfo Onboard::detectHw() {
    HwInfo hw;
    hw.vramGb = nvidiaVramGb();
    hw.ramGb = systemRamGb();
    hw.budgetGb = hw.vramGb > 0 ? hw.vramGb : hw.ramGb;

    const auto g = [](double v) { return QString::number(v, 'f', 1); };
    if (hw.vramGb > 0)
        hw.summary = g(hw.vramGb) + QStringLiteral(" GB GPU VRAM");
    else if (hw.ramGb > 0)
        hw.summary = g(hw.ramGb) + QStringLiteral(" GB RAM (no NVIDIA GPU detected — CPU inference)");
    else
        hw.summary = QStringLiteral("unknown");
    return hw;
}

Onboard::Recommendation Onboard::recommend(const HwInfo& hw) {
    Recommendation r;
    const double b = hw.budgetGb;
    if (b >= 8) {
        r.tag = QStringLiteral("qwen3.5:9b");
        r.why = QStringLiteral("reliable native tool-calling, fits comfortably");
    } else if (b >= 6) {
        r.tag = QStringLiteral("qwen2.5-coder:7b");
        r.why = QStringLiteral("best all-round local coder for tool use");
    } else {
        // Below the presets: the smallest thing that runs on modest hardware. Tool
        // use is weak here, so chat mode is the honest recommendation.
        r.tag = QStringLiteral("llama3.2:3b");
        r.why = QStringLiteral("tiny — runs on modest hardware (tool use is limited; use chat)");
    }
    if (b >= 20)
        r.stronger = QStringLiteral("qwen2.5-coder:32b");
    else if (b >= 12)
        r.stronger = QStringLiteral("qwen2.5-coder:14b");
    return r;
}

int Onboard::run(const std::function<bool(const QString&)>& confirm) {
    Config::load();
    out() << QStringLiteral("\033[1;36m\n⚙  OllamaDev setup\033[0m\n");
    out().flush();

    auto ollama = Backends::get(QStringLiteral("ollama"));
    if (!ollama || !ollama->available()) {
        const QString host = Config::str("ollama.host", "http://localhost:11434");
        out() << QStringLiteral("\033[31m✗ Can't reach Ollama at ") << host
              << QStringLiteral("\033[0m\n");
        const bool haveBinary =
            !QStandardPaths::findExecutable(QStringLiteral("ollama")).isEmpty();
        if (!haveBinary)
            out() << QStringLiteral("  1. Install Ollama:  ")
                  << cyan(QStringLiteral("https://ollama.com/download")) << "\n"
                  << QStringLiteral("  2. Start it:        ") << cyan(QStringLiteral("ollama serve"))
                  << "\n";
        else
            out() << QStringLiteral("  Start the server:  ") << cyan(QStringLiteral("ollama serve"))
                  << "\n";
        out() << QStringLiteral("  Then re-run:        ") << cyan(QStringLiteral("ollamadev setup"))
              << "\n\n";
        out().flush();
        return 1;
    }

    const HwInfo hw = detectHw();
    out() << QStringLiteral("  Hardware:  ") << cyan(hw.summary) << "\n";

    Recommendation rec = recommend(hw);
    out() << QStringLiteral("  Recommended:  ") << QStringLiteral("\033[1;32m") << rec.tag
          << QStringLiteral("\033[0m") << dim(QStringLiteral("  — ") + rec.why) << "\n";
    if (!rec.stronger.isEmpty())
        out() << QStringLiteral("  ")
              << dim(QStringLiteral("Stronger option for your hardware: ") + rec.stronger +
                     QStringLiteral(" (pull with: ollamadev models pull ") + rec.stronger +
                     QStringLiteral(")"))
              << "\n";
    out() << QStringLiteral("  ")
          << dim(QStringLiteral(
                 "Want frontier-scale without local VRAM? ollamadev models cloud (Ollama cloud, "
                 "opt-in)"))
          << "\n";
    out().flush();

    const QStringList installed = ollama->models();
    const QString have = Models::match(rec.tag, installed);
    if (!have.isEmpty()) {
        out() << green(QStringLiteral("✓ ") + rec.tag + QStringLiteral(" is already installed.\n"));
        rec.tag = have;  // persist the concrete installed tag
    } else {
        const bool pull = confirm(QStringLiteral("Pull ") + rec.tag + QStringLiteral(" now?"));
        if (pull) {
            out() << "\n";
            out().flush();
            if (!Puller::pull(rec.tag)) {
                out() << dim(QStringLiteral("  Pull failed — try: ollamadev models pull ") + rec.tag)
                      << "\n";
            }
        } else {
            out() << QStringLiteral("  ")
                  << dim(QStringLiteral("Skipped. Pull later: ollamadev models pull ") + rec.tag)
                  << "\n";
        }
    }

    Config::setPref(QStringLiteral("ollama.defaultModel"), rec.tag);
    out() << "\n" << green(QStringLiteral("✓ Default model set to ") + rec.tag + QStringLiteral("."))
          << "\n";
    out() << QStringLiteral("  Start coding:   ") << QStringLiteral("\033[1;36mollamadev\033[0m")
          << dim(QStringLiteral("   (or: ollamadev chat · ollamadev crew \"<task>\")")) << "\n\n";
    out().flush();
    return 0;
}

}  // namespace odv
