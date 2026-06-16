#pragma once

#include <string>
#include <vector>

namespace forge {

// Transient corner notifications (#6). Push from any user-facing success/error
// site; Draw() ages and renders them each frame (top-right, fading out). Purely
// a UI affordance — it owns no engine state.
class ToastManager {
public:
    enum class Kind { Info, Success, Error };

    void Push(Kind kind, std::string text);
    void Draw(); // call once inside the ImGui frame

private:
    struct Toast {
        Kind kind;
        std::string text;
        float age = 0.0f; // seconds since Push
    };
    std::vector<Toast> m_Toasts;
};

} // namespace forge
