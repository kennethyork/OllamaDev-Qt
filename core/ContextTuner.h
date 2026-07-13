#pragma once
#include <QString>
namespace odv {
// Suggest num_ctx from free RAM/VRAM. STUB.
class ContextTuner { public: static QString report(); static int suggest(); };
}  // namespace odv
