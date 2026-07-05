-- Camera control from a script (#91): read the pose, frame a subject, dolly
-- around it, store/recall bookmarks. Camera state is viewer state — none of
-- this lands in the undo history.
-- Run via the execute_script MCP tool.

local subject = forge.spawn{primitive='torus', name='tour_subject'}
forge.set_transform{id=subject.id, position={0, 1, 0}}

-- Frame it (same engine path the look_at tool uses).
forge.look_at{id=subject.id}

-- Read the orbit pose: focalPoint / distance / pitchDeg / yawDeg / fovDeg.
local pose = forge.camera()
print(('start: yaw=%.0f pitch=%.0f dist=%.2f fov=%.0f')
      :format(pose.yawDeg, pose.pitchDeg, pose.distance, pose.fovDeg))

-- Remember this framing in bookmark slot 1 (the editor's F1).
forge.store_view{slot=1}

-- Low three-quarter close-up with a longer lens.
forge.set_camera{yawDeg=45, pitchDeg=12, distance=pose.distance * 0.6, fovDeg=30}

-- Top-down plan view.
forge.set_camera{pitchDeg=85, yawDeg=0, fovDeg=60}

-- Back to where we started.
local restored = forge.recall_view{slot=1}
return {finalYaw=restored.yawDeg, finalFov=restored.fovDeg}
