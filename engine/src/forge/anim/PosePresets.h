#pragma once

#include "forge/anim/Pose.h"

#include <optional>
#include <string>

namespace forge {

struct Skeleton;

// Canned FK poses expressed AS DATA: a table of (joint-name, euler-degrees) rows.
// A preset is applied by name against a specific skeleton — rows whose joint name is
// absent from the rig are silently skipped, so a preset works on any humanoid that
// uses the standard glTF/Mixamo bone names and is a harmless partial/no-op otherwise.
// Returns nullopt for an unknown preset name (rest/t-pose/a-pose/sit are known). #147.
std::optional<Pose> MakePresetPose(const Skeleton& skeleton, const std::string& preset);

} // namespace forge
