# forge.* script examples

Demo scripts for the `execute_script` MCP tool's Lua surface. Each runs as a
single tool call; every mutating script is one undo entry and rolls back
atomically on error.

| Script | Bindings exercised |
|---|---|
| `camera-tour.lua` | `camera`, `set_camera`, `store_view`, `recall_view`, `look_at` |
| `edit-mode-workflow.lua` | `mesh_elements` (face/edge, radius filter, `ofFaces`), `extrude_faces`, `extrude_edges`, `subdivide_faces`, `subdivide_edges`, `smooth`, `shade`, `mesh_stats` |
| `layout-and-export.lua` | `group`, `ungroup`, `drop_to_ground`, `select`, `toggle_select`, `get_selection`, `clear_selection`, `box_select`, `snap_settings`, `export_stl` |
| `sculpt-pear.lua` | `sculpt` (grab/inflate/smooth, `strokes`, negative-strength dent), `move_verts` (falloff), `subdivide`, `mesh_stats` |
| `lathe-tableware.lua` | `spawn` with `primitive='lathe'` (walled cup profile, vase, turned table legs) and `primitive='sweep'` (cup handle along an arc), `mesh_stats` watertight checks |

Element-id cheatsheet (edit-mode ops):

- **face id** = triangle id = a `raycast` result's `triIndex / 3`.
- `mesh_elements{kind='face', point={x,y,z}, radius=r}` lists face ids with
  world-space centers/normals — pick regions by geometry instead of a cursor.
- `mesh_elements{kind='edge', ofFaces={...}}` maps faces to their edge ids.
- Ids are only valid against the mesh's **current** topology: re-query after
  every `extrude_*` / `subdivide_*` / `boolean` / `remesh`.
