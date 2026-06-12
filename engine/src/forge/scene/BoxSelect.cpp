#include "BoxSelect.h"

#include <algorithm>

namespace forge {

std::optional<RectUV> ProjectAABBToScreen(const mat4& viewProj, const AABB& worldBox)
{
    if (!worldBox.Valid())
        return std::nullopt;

    vec2 mn(FLT_MAX), mx(-FLT_MAX);
    int behind = 0;
    for (int c = 0; c < 8; ++c) {
        vec3 corner{(c & 1) ? worldBox.max.x : worldBox.min.x,
                    (c & 2) ? worldBox.max.y : worldBox.min.y,
                    (c & 4) ? worldBox.max.z : worldBox.min.z};
        vec4 clip = viewProj * vec4(corner, 1.0f);
        if (clip.w <= 0.0f) {
            ++behind;
            continue;
        }
        // NDC -> viewport UV (y flipped: UV is top-down like the mouse).
        vec2 uv{(clip.x / clip.w) * 0.5f + 0.5f, 1.0f - ((clip.y / clip.w) * 0.5f + 0.5f)};
        mn = glm::min(mn, uv);
        mx = glm::max(mx, uv);
    }

    if (behind == 8)
        return std::nullopt; // fully behind the camera: never selectable
    if (behind > 0)
        return RectUV{{0.0f, 0.0f}, {1.0f, 1.0f}}; // straddles the near plane: over-select

    return RectUV{mn, mx};
}

} // namespace forge
