-- Sculpt a sphere into a pear (#105): grab the top up into a stem, fatten the
-- base, dent one cheek, relax the waist, then nudge the tip for asymmetry.
-- One execute_script call = one undo entry; Ctrl+Z restores the sphere.
--
-- Contracts worth copying:
--   * center/point/radius/offset are WORLD-space; the handler converts.
--   * grab/inflate strength = world units the full-weight center moves;
--     negative inflate strength dents inward.
--   * smooth strength = 0..1 relaxation factor per stroke.
--   * strokes=N repeats a brush (inflate re-derives normals between strokes).

local s = forge.spawn{primitive = 'sphere', name = 'pear', position = {0, 1, 0}}
forge.subdivide{id = s.id} -- more verts = smoother sculpt

-- Pull the top into a stem (sphere spans y = 0.5 .. 1.5).
forge.sculpt{id = s.id, brush = 'grab', center = {0, 1.5, 0}, radius = 0.4,
             direction = {0, 1, 0}, strength = 0.35}

-- Fatten the lower half into the pear body.
forge.sculpt{id = s.id, brush = 'inflate', center = {0, 0.75, 0}, radius = 0.55,
             strength = 0.06, strokes = 2}

-- Dent one cheek: negative inflate pushes along inverted normals.
forge.sculpt{id = s.id, brush = 'inflate', center = {0.5, 0.9, 0}, radius = 0.25,
             strength = -0.08}

-- Relax the waist where the stem pull meets the fattened body.
forge.sculpt{id = s.id, brush = 'smooth', center = {0, 1.25, 0}, radius = 0.35,
             strength = 0.5, strokes = 2}

-- Tilt the stem tip sideways — real pears are not symmetric.
forge.move_verts{id = s.id, point = {0, 1.85, 0}, radius = 0.25,
                 offset = {0.12, 0, 0}, falloff = 'smooth'}

return forge.mesh_stats{id = s.id}
