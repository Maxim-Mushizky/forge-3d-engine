#pragma once

#include "forge/core/Math.h"
#include "forge/renderer/Material.h"

namespace forge {

struct TransformComponent {
    vec3 translation{0.0f};
    vec3 rotation{0.0f}; // euler, radians
    vec3 scale{1.0f};

    mat4 World() const
    {
        return glm::translate(mat4(1.0f), translation) *
               glm::mat4_cast(quat(rotation)) *
               glm::scale(mat4(1.0f), scale);
    }
};

// Material lives in renderer/ (the renderer consumes it); alias keeps scene naming consistent.
using MaterialComponent = Material;

} // namespace forge
