#pragma once
#include <QString>

namespace odv {

// Sizes num_ctx against the RAM/VRAM this machine actually has.
class ContextTuner {
public:
    static QString report();
    static int suggest();

    // Is this machine ACTUALLY short of resources?
    //
    // `ollama.lowResource` shipped as a hardcoded `true` and nothing ever looked at
    // the hardware — so a 24GB RTX 3090 got throttled to an 8192-token context
    // exactly as hard as a laptop with no GPU at all, even when the user's own
    // config asked for 16384 and the model could do 262144. The safe default was
    // safe for the wrong machine, and silent about it.
    //
    // Now it is measured. An explicit `ollama.lowResource` in config, or
    // OLLAMADEV_POWER in the environment, still wins — this is only the DEFAULT.
    //
    // Cached: it shells out to nvidia-smi, and chatOptions() runs on the hot path of
    // every single request.
    static bool lowResourceMachine();
};

}  // namespace odv
