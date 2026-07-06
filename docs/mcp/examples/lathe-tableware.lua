-- Lathe & sweep tableware (#111): a walled cup with a swept handle, a vase,
-- and a table on turned legs — surfaces of revolution instead of
-- sphere-pushing. Every mesh comes back watertight; the script returns the
-- mesh_stats line for each piece.
local stats = {}

-- Cup: profile runs from the bottom center up the outside, over the rim, down
-- the inside to the inner floor center — walls with real thickness, sealed at
-- both ends by the axis poles.
local cup = forge.spawn{primitive='lathe', name='Cup', position={-1.5, 0, 0}, params={
  profile={{0.0,0.0},{0.35,0.0},{0.40,0.02},{0.42,0.30},{0.40,0.60},{0.40,0.62},
           {0.36,0.62},{0.36,0.60},{0.38,0.30},{0.35,0.06},{0.0,0.06}},
  sectors=64, closed=true}}
stats.cup = forge.mesh_stats{id=cup.id}

-- Handle: octagonal section swept along an arc, both ends buried in the wall.
local sec = {}
for i = 0, 7 do
  local a = 2 * math.pi * i / 8
  sec[#sec+1] = {0.035 * math.cos(a), 0.035 * math.sin(a)}
end
local handle = forge.spawn{primitive='sweep', name='CupHandle', position={-1.5, 0, 0}, params={
  profile=sec,
  path={{0.38,0.52,0},{0.52,0.50,0},{0.60,0.34,0},{0.52,0.18,0},{0.38,0.14,0}}}}
stats.handle = forge.mesh_stats{id=handle.id}

-- Vase: classic belly + neck flare, closed to the axis at both ends.
local vase = forge.spawn{primitive='lathe', name='Vase', position={0, 0, 0}, params={
  profile={{0.0,0.0},{0.22,0.0},{0.30,0.08},{0.34,0.30},{0.26,0.55},{0.13,0.72},
           {0.12,0.88},{0.17,0.98},{0.19,1.02},{0.0,1.02}},
  sectors=64, closed=true}}
stats.vase = forge.mesh_stats{id=vase.id}

-- Table: four turned legs + a box top.
local legProfile = {{0.0,0.0},{0.045,0.0},{0.045,0.06},{0.03,0.10},{0.05,0.16},
                    {0.032,0.40},{0.045,0.62},{0.045,0.70},{0.0,0.70}}
local legPos = {{1.45,0,-0.35},{1.45,0,0.35},{2.55,0,-0.35},{2.55,0,0.35}}
for i, p in ipairs(legPos) do
  local leg = forge.spawn{primitive='lathe', name='TableLeg'..i, position=p,
                          params={profile=legProfile, sectors=32, closed=true}}
  if i == 1 then stats.leg = forge.mesh_stats{id=leg.id} end
end
local top = forge.spawn{primitive='cube', name='TableTop', position={2.0, 0.73, 0},
                        scale={1.5, 0.06, 1.0}}
stats.top = forge.mesh_stats{id=top.id}

local out = {}
for k, v in pairs(stats) do
  out[#out+1] = string.format('%s: verts=%d tris=%d watertight=%s boundary=%d nonmanifold=%d degen=%d',
    k, v.vertices, v.triangles, tostring(v.watertight), v.boundaryEdges,
    v.nonManifoldEdges, v.degenerateTriangles)
end
table.sort(out)
return table.concat(out, '\n')
