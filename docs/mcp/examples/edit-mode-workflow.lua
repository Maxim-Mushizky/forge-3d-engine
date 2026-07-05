-- Edit-mode element workflow from a script (#91): pick faces without a mouse,
-- extrude, locally subdivide, smooth, shade — the issue #91 acceptance run.
-- Element ids: face id == triangle id == a raycast's triIndex / 3;
-- mesh_elements lists ids with world centers so scripts can pick by geometry.
-- The whole script is ONE undo entry.

local box = forge.spawn{primitive='cube', name='edit_demo'}
forge.set_transform{id=box.id, position={0, 0.5, 0}}

-- 1. Select faces: everything whose centroid sits on the top of the cube.
local top = forge.mesh_elements{id=box.id, kind='face', point={0, 1.0, 0}, radius=0.4}
local faceIds = {}
for i, f in ipairs(top.elements) do faceIds[i] = f.id end
print(('top faces: %d of %d total'):format(top.returned, top.total))

-- 2. Extrude the top region upward as one cap.
forge.extrude_faces{id=box.id, ids=faceIds, distance=0.5}

-- 3. Re-query (ids are fresh after every topology change!) and locally
--    subdivide the new cap for detail.
local cap = forge.mesh_elements{id=box.id, kind='face', point={0, 1.5, 0}, radius=0.4}
local capIds = {}
for i, f in ipairs(cap.elements) do capIds[i] = f.id end
forge.subdivide_faces{id=box.id, ids=capIds}

-- 4. Pull one side edge out: map a side face to its boundary edges.
local side = forge.mesh_elements{id=box.id, kind='face', point={0.5, 0.75, 0}, radius=0.3}
if side.returned > 0 then
    local edges = forge.mesh_elements{id=box.id, kind='edge', ofFaces={side.elements[1].id}}
    forge.extrude_edges{id=box.id, ids={edges.ids[1]}, distance=0.2}
end

-- 5. Densify one edge ring, then relax the whole surface and shade smooth.
local anyEdge = forge.mesh_elements{id=box.id, kind='edge', maxCount=1}
forge.subdivide_edges{id=box.id, ids={anyEdge.elements[1].id}}
forge.smooth{id=box.id, strength=0.4, iterations=2}
forge.shade{id=box.id, smooth=true}

local stats = forge.mesh_stats{id=box.id}
return {triangles=stats.triangles, watertight=stats.watertight}
