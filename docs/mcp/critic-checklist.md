# Visual critique checklist (agent self-review)

Run this after every build step, before declaring a step done. Single-view
checks miss depth errors along the view axis — a part can hover 7mm off a
surface and look perfectly attached head-on. Use `render_views`, not one
`render_image`, and answer the questions *explicitly* (CADCodeVerify-style
question/answer critique beats "look and fix").

## Pass 1 — geometry (`render_views` preset `turntable`, mode `clay`)

- [ ] Any **floating parts**? Check every contact point from all four angles.
- [ ] Any **interpenetration** that should be a clean joint?
- [ ] Are proportions right from the side and back, not just the front?
- [ ] Silhouette check: does the outline read as the intended object?

## Pass 2 — layout (`render_views` preset `top_ortho`)

- [ ] Plan view: is the spacing/alignment between objects what was asked?
- [ ] Anything overlapping in plan that should be separate?

## Pass 3 — topology (`render_views` mode `normals` + `get_mesh_stats`)

- [ ] Normals view: sudden color flips inside a smooth surface = inverted or
      broken faces.
- [ ] `get_mesh_stats` on every mesh you edited: `watertight`? `nonManifoldEdges`
      = 0? `degenerateTriangles` = 0? Booleans and remesh can silently tear —
      a dark slash in a render was 6 non-manifold edges once.

## Pass 4 — identification (`render_views` mode `object_id`)

- [ ] Name every colored blob using the legend. Anything you can't name is
      either debris to delete or a part in the wrong place.

## Pass 5 — final look (`render_image`, path traced)

- [ ] Lighting: harsh shadows? blown highlights? subject too dark?
- [ ] Glass/transmissive parts: interior dark? Raise `bounces` to 8+.
- [ ] Wrong focus for the shot? Use `aperture` + `focusDist`.

Numeric checks beat pixel checks when both exist: prefer `get_mesh_stats` and
world-space positions from `get_scene` over squinting at renders.
