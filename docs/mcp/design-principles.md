# Design principles for reference-driven modeling

How an agent should turn a reference image into geometry through the MCP tools.
The companion to [`critic-checklist.md`](critic-checklist.md): that file is how to
*judge* a result, this one is how to *design* it. Born from the Templar-shield
build (2026-07-07), where a faithful trace of a tilted photo produced a subtly
asymmetric shield that scored 0.99 IoU — the metric rewarded copying the photo's
flaws.

## 1. Identify structural invariants BEFORE cutting geometry

Look at the subject and name what is true of the *object class*, not the pixels:

- **Bilateral symmetry** — shields, bottles, tools, furniture fronts.
- **Radial symmetry / lathe-ness** — vases, cups, lamp bases, chess pieces:
  a surface of revolution wants a lathe profile, not a traced sweep.
- **Repeated elements** — rivets, bosses, spokes, slats: one parametric loop,
  never N hand-placed copies.
- **Straight/level edges, right angles** — architecture, frames, plates.

Then pick the tooling that **enforces** the invariant by construction: the
`mirror="x"` fold, `spawn{lathe}`, Lua loops with shared parameters, snapped
coordinates. Tracing pixels and hoping the invariant survives is not a method.

## 2. Symmetric subject → symmetric tooling FIRST

Not as repair after someone spots the skew — as the first tool reached for.
Fold the outline (`analyze_reference{mirror="x"}`, mirror the half-profile back
to a closed polygon) for anything symmetric by nature. Mirrored features
(blades, arms, handles) build from ONE traced side so both share a single
source of truth. This also repairs one-sided reference damage for free (the
shield's JPEG-chewed cross arm; the axe's blown blade rim).

## 3. The reference is a measurement, not ground truth

Photo tilt, JPEG noise, mask erosion are noise terms on top of the real object.
Object-class priors outrank per-pixel fidelity: enforcing a true invariant is
EXPECTED to lower IoU-vs-photo (shield: 0.9918 raw → 0.9824 folded). Read the
raw-vs-folded delta as *measured photo noise*, record it, and don't chase it.

## 4. Metrics don't see everything — look at the thing

A silhouette score self-consistent with a defective mask stays high (0.9925
with bitten edges). Before calling a build done: render it from at least two
angles and actually look, per critic-checklist. A number passing is necessary,
never sufficient.

## 5. Place by measurement, never by eyeball

Positions and sizes come from landmark arithmetic (bbox centers, spans,
`get_entity` worldBounds) — including self-correction loops for things with
unknowable intrinsic size (3D text: spawn, measure bounds, rescale to target).
The shield's cross was centered by measuring its 2.1 mm offset, not by nudging
until it looked right.

## 6. Know the assembly's real mounting planes

Attachment geometry seats on measured surfaces, not remembered ones — the
shield's back plane was the rim slab's face (z −0.075), not the body's (−0.06),
and a motto placed on the remembered plane rendered entombed inside solid
geometry. When a detail vanishes, `get_entity` bounds vs a close-up render
localizes the disagreement immediately.
