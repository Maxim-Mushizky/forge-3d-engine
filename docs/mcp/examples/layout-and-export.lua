-- Scene structure + selection + export from a script (#91): build a lamp from
-- parts, group it, drop it to the ground, drive the selection like a user
-- would, tune gizmo snapping, and export printable STL.

-- Parts, deliberately floating.
local base = forge.spawn{primitive='cylinder', name='lamp_base'}
forge.set_transform{id=base.id, position={2, 3.0, 1}, scale={0.4, 0.05, 0.4}}
local stem = forge.spawn{primitive='cylinder', name='lamp_stem'}
forge.set_transform{id=stem.id, position={2, 3.5, 1}, scale={0.05, 0.5, 0.05}}
local shade = forge.spawn{primitive='cone', name='lamp_shade'}
forge.set_transform{id=shade.id, position={2, 4.2, 1}, scale={0.3, 0.25, 0.3}}

-- One entity out of three parts, then settle it on the floor.
local lamp = forge.group{names={'lamp_base', 'lamp_stem', 'lamp_shade'}}
forge.drop_to_ground{id=lamp.id}

-- Selection surface: exactly what clicking and marquee-dragging would do.
forge.select{id=lamp.id}
forge.toggle_select{name='lamp_base'}         -- additive toggle
print('selected now: ' .. #forge.get_selection().ids)
forge.clear_selection()
forge.box_select{min={0.0, 0.0}, max={1.0, 1.0}} -- marquee the whole viewport
print('after marquee: ' .. #forge.get_selection().ids)

-- Gizmo snapping prefs (persisted editor settings, not undoable).
local snap = forge.snap_settings{enabled=true, translate=0.25, rotateDeg=15, scale=0.1}
print(('snap: %s translate=%.2f'):format(tostring(snap.enabled), snap.translate))

-- Print-ready STL of just the lamp subtree, 100 mm per world unit.
local stl = forge.export_stl{path='lamp.stl', ids={lamp.id}, scale=100}
print(('stl: %d triangles, watertight=%s'):format(stl.triangles, tostring(stl.watertight)))

-- Ungroup demo: dissolve the container again (children keep world poses).
forge.ungroup{id=lamp.id}
return {exported=stl.ok, triangles=stl.triangles}
