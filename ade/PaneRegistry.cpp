#include "PaneRegistry.h"

namespace odv {

PaneRegistry& PaneRegistry::instance() {
    static PaneRegistry r;
    return r;
}

void PaneRegistry::add(const PaneSpec& spec) {
    // Last registration of a kind wins, so a pane can be overridden in tests.
    for (auto& s : specs_) {
        if (s.kind == spec.kind) {
            s = spec;
            return;
        }
    }
    specs_.append(spec);
}

const PaneSpec* PaneRegistry::find(const QString& kind) const {
    for (const auto& s : specs_)
        if (s.kind == kind) return &s;
    return nullptr;
}

}  // namespace odv
