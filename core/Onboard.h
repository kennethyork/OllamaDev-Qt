#pragma once
#include <QString>
#include <functional>

namespace odv {

// ONBOARDING — the `setup` command (port of the setup path in src/99-main.php
// plus the hardware probe from src/53-context-tuner.php).
//
// A 60-second start: check Ollama is up, read how much memory this machine has
// (GPU VRAM if an NVIDIA card is present, else system RAM), recommend a preset
// that fits, offer to pull it, and persist it as ollama.defaultModel.
//
// Detection is split out as pure functions so the desktop can reuse the same
// recommendation without driving the CLI flow.
class Onboard {
public:
    struct HwInfo {
        double vramGb = 0.0;  // largest NVIDIA GPU, 0 when none/unknown
        double ramGb = 0.0;   // total system RAM, 0 when unreadable
        double budgetGb = 0.0;  // VRAM if present, else RAM — what the model must fit in
        QString summary;        // human-readable one-liner
    };

    struct Recommendation {
        QString tag;       // the model to pull/set as default
        QString why;       // one-line rationale
        QString stronger;  // a bigger option this machine could also run, or empty
    };

    static HwInfo detectHw();
    static Recommendation recommend(const HwInfo& hw);

    // Full interactive `setup`. `confirm` answers the one y/N question (pull the
    // recommended model now?) — core owns no terminal, so the caller supplies it,
    // exactly like GitFlow::ship. Returns a process exit code.
    static int run(const std::function<bool(const QString&)>& confirm);
};

}  // namespace odv
