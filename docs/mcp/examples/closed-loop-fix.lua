-- Closed-loop shape fix from structured diff data (#136): build a deliberately
-- mis-built axe head (top plates 0.3 too short, right blade 0.15 too long),
-- then repair it by looping compare_silhouette -> regions[] -> parameter fix,
-- WITHOUT reading a single image. Each region arrives as numbers (type,
-- centroidWorld, bboxWorld), so the fix is arithmetic: 'missing' grows the
-- nearest part along the region's dominant offset axis by the region's extent,
-- 'excess' shrinks it.
--
-- Frame caveat (learned the hard way): compare_silhouette normalizes both
-- masks (tight-crop + rescale + center), so the diff lives in a translation/
-- scale-invariant frame. A defect that changes the silhouette's OUTER BBOX
-- (e.g. one blade too wide) re-centers that frame and smears the one-sided
-- error into a symmetric half-magnitude pair — still convergent under this
-- loop, but no longer attributable to one side in a single measurement. The
-- perturbations here keep the outer bbox intact (short plates sit below the
-- column top; the long blade stays above the column bottom), so every region
-- localizes exactly and each fix lands in one shot.
--
-- Reference fixture: axe-head-ref.png next to this script — the correct-
-- parameter build's own normalized silhouette (captured once from
-- compare_silhouette's silhouette output). Run with the editor launched from
-- the repo root so the relative path resolves.

local REF = "docs/mcp/examples/axe-head-ref.png"

-- Correct proportions; the two perturbations below are what the loop must
-- recover. Parts anchor to fixed datums (plate bottoms, blade tops), so a
-- height fix moves the damaged edge only — like real modeling would.
local P = {
  colW = 0.35, colH = 2.20,           -- pommel column (sets the frame: tallest part)
  bladeWL = 0.60, bladeWR = 0.60,     -- blade widths, per side
  bladeHL = 1.10, bladeHR = 1.10,     -- blade heights, per side, tops fixed
  plateW = 0.50, plateH = 0.80,       -- top plates flanking the column, tops flush
  depth = 0.25,
}
P.plateH = P.plateH - 0.3   -- perturbation 1: plates too short (bottoms stay put)
P.bladeHR = P.bladeHR + 0.15 -- perturbation 2: right blade too long (top stays put)

local BLADE_TOP = 0.20    -- world y of both blades' top edges
local PLATE_BOTTOM = 0.30 -- world y of both plates' bottom edges

-- Part centers derive from the parameters — the same arithmetic the fix loop
-- uses to match a region to its nearest part and pick which parameter to move.
local function layout(p)
  return {
    { name = "axe_part_col",     pos = { 0, 0, 0 },                                       size = { p.colW, p.colH, p.depth },       wParam = nil,       hParam = "colH" },
    { name = "axe_part_blade_l", pos = { -(p.colW + p.bladeWL) / 2, BLADE_TOP - p.bladeHL / 2, 0 },    size = { p.bladeWL, p.bladeHL, p.depth }, wParam = "bladeWL", hParam = "bladeHL" },
    { name = "axe_part_blade_r", pos = { (p.colW + p.bladeWR) / 2, BLADE_TOP - p.bladeHR / 2, 0 },     size = { p.bladeWR, p.bladeHR, p.depth }, wParam = "bladeWR", hParam = "bladeHR" },
    { name = "axe_part_plate_l", pos = { -(p.colW + p.plateW) / 2, PLATE_BOTTOM + p.plateH / 2, 0 },   size = { p.plateW, p.plateH, p.depth },   wParam = "plateW",  hParam = "plateH" },
    { name = "axe_part_plate_r", pos = { (p.colW + p.plateW) / 2, PLATE_BOTTOM + p.plateH / 2, 0 },    size = { p.plateW, p.plateH, p.depth },   wParam = "plateW",  hParam = "plateH" },
  }
end

local function rebuild(p)
  -- Re-query after every delete: correct whether or not deleting the group
  -- cascades to its children, and n is tiny.
  while true do
    local victim
    for _, e in ipairs(forge.scene().entities) do
      if e.name == "axe_head_demo" or (e.name and e.name:find("^axe_part_")) then
        victim = e.id
        break
      end
    end
    if not victim then break end
    forge.delete{ id = victim }
  end
  local names = {}
  for _, part in ipairs(layout(p)) do
    forge.spawn{ primitive = "cube", name = part.name, position = part.pos, scale = part.size }
    names[#names + 1] = part.name
  end
  local g = forge.group{ names = names } -- one root so the compare sees the whole head
  forge.rename{ id = g.id, newName = "axe_head_demo" }
end

rebuild(P)
local ious, fixes = {}, {}

for iter = 1, 6 do
  local c = forge.compare_silhouette{ name = "axe_head_demo", reference = REF,
                                      view = "front", size = 256, threshold = 0.99 }
  ious[#ious + 1] = c.iou
  if c.pass or #c.regions == 0 then break end

  local r = c.regions[1] -- largest mismatch drives this iteration
  local rw = r.bboxWorld.max[1] - r.bboxWorld.min[1]
  local rh = r.bboxWorld.max[2] - r.bboxWorld.min[2]
  local cx, cy = r.centroidWorld[1], r.centroidWorld[2]

  -- Nearest part by world-frame centroid distance.
  local best, bestD
  for _, part in ipairs(layout(P)) do
    local d = math.abs(cx - part.pos[1]) + math.abs(cy - part.pos[2])
    if not bestD or d < bestD then best, bestD = part, d end
  end

  -- Dominant offset axis picks the dimension; the region's extent along it is
  -- the measured correction. 'missing' grows, 'excess' shrinks.
  local dx, dy = cx - best.pos[1], cy - best.pos[2]
  local sign = (r.type == "missing") and 1 or -1
  local param, delta
  if math.abs(dy) >= math.abs(dx) then
    param, delta = best.hParam, sign * rh
  else
    param, delta = best.wParam, sign * rw
  end
  if not param then break end -- unfixable match (column width is not a knob here)
  P[param] = P[param] + delta
  fixes[#fixes + 1] = { iter = iter, region = r.type, part = best.name,
                       param = param, delta = delta }
  rebuild(P)
end

return { ious = ious, final = ious[#ious], iterations = #ious, fixes = fixes }
