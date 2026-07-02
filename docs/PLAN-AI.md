# PLAN-AI — Making Forge AI-native

Research report + roadmap for driving Forge with an LLM agent (Claude via MCP) up to the
milestone of **an agent creating a realistic textured human end-to-end**. Tracked by
epic [#74](https://github.com/Maxim-Mushizky/forge-3d-engine/issues/74); sub-issues #75–#86
plus existing #16/#17.

Method: multi-agent deep research (2026-07-02) — 5 search angles, 22 primary sources fetched,
110 claims extracted, top 25 adversarially verified by 3-vote panels (23 confirmed, 2 refuted).
Claims marked *(unverified)* below came from primary-source fetches that didn't make the
verification budget; the license pages and READMEs were read directly, so treat them as solid
but re-check at implementation time.

---

## 1. MCP vs RAG — verdict: hybrid

**MCP tools for actuation, MCP resources (lightweight RAG) for knowledge.** Not either/or.

- LL3M (arXiv:2508.08228) — the strongest agentic-3D precedent — pairs script-writing actuation
  with *BlenderRAG*, a retrieval base built from 1,729 Blender API doc pages. Its ablation:
  **RAG cut total agent error rate by 26%**. [verified 3-0]
- blender-mcp (~23k stars) is pure actuation + an execute-code escape hatch; third-party review
  finds that path limited to simple/hard-surface geometry — "complex organic shapes, characters,
  or production-ready assets" fail without its gen-3D integrations. [verified 3-0]
- Forge's op surface is tiny compared to Blender's — MCP `resources/read` over a curated
  `docs/mcp/` corpus (#79) covers the knowledge side without an embedding store.

## 2. Architecture — embedded server, Epic's threading rule

- **Embedded, not sidecar.** Epic's official UE 5.8 plugin embeds the MCP server in the editor
  process over local HTTP (Claude Code named as a supported client); ChiR24/Unreal_mcp proves a
  fully native C++ Streamable-HTTP implementation (raw sockets, JSON-RPC 2.0, SSE). Sidecars
  (unity-mcp's Python bridge, blender-mcp's TCP addon) work but add a process and an IPC layer
  Forge doesn't need. [verified 3-0 ×4]
- **Threading:** HTTP thread parses + enqueues only; tool calls execute **serially on the GL
  main thread between frames** (Epic: "executing Tool invocations on the game thread serially…
  clients should not issue overlapping Tool calls"). No C++ SDK does this for you. [verified 3-0]
- **No C++ MCP SDK is turnkey** — hand-roll the protocol layer on cpp-httplib + nlohmann/json
  (ChiR24 pattern):
  | SDK | License | Problem |
  |---|---|---|
  | hkr04/cpp-mcp | MIT | transport/spec-revision claims **refuted 0-3** on inspection — capabilities unclear |
  | Qihoo360/TinyMCP | MIT | stdio-only (GUI app can't cede stdin/stdout), no root CMakeLists |
  | GopherSecurity/gopher-mcp | Apache-2.0 | pre-1.0, libevent dep, Windows builds via Cygwin scripts |
- **Visual feedback transport:** render → PNG → base64 MCP image content, capped ~1024px.

## 3. Tool surface — mid-coarse granularity + batch

Shipped precedents converge: Unity MCP = 47 focused tools in 10 groups; ChiR24/Unreal = 23
domain-multiplexed tools ("related actions live on their parent tools so clients load less
context"); blender-mcp = ~22 tools + code escape hatch. [verified 3-0, 3-0, 2-1]

Forge: **~15–40 domain-grouped tools** (#76 perception, #77 actuation) plus a **batch op-list
tool** (#78) — code-interpreter-style batching is validated by LL3M (assets as code) and
SceneCraft (ICML 2024: scripts placing ~100 assets). [verified 3-0 ×2]

**The render-feedback loop is load-bearing.** BlenderAlchemy (ECCV 2024): without a visual
target "the verisimilitude of the material plateaus very quickly"; its evaluator + edit-reversion
is the error-recovery model. LL3M's Critic renders 5 adaptive views. Both papers also admit VLMs
miss fine spatial artifacts — keep a human in the loop for final judgment. [verified 3-0 ×4]

## 4. Realistic humans — delegate, don't box-model

No precedent shows an LLM modeling a human from ops; the working pattern is parametric bases +
generative models, with agent ops for assembly/adjustment.

**Parametric base — licensing decides it** *(unverified tier, primary sources read directly)*:
- **SMPL-X: ruled out for bundling.** Research-only license, redistribution prohibited, the
  method itself is patented (US10395411B2), commercial rights exclusive to Meshcapade.
- **MPFB2/MakeHuman:** code GPLv3 (keep out of the engine), **assets CC0**, outputs unowned —
  fine as an external generator; documented export path is FBX with the "GameEngine" rig.
- **Anny (NAVER, arXiv:2511.03589): the pick.** Apache-2.0 code *and* model, built on MakeHuman
  CC0 assets. Interpretable phenotype parameters (gender/age/height/weight — LLM-native, unlike
  PCA latents), 13,380-vert quad topology, 163-bone rig + Mixamo-style rig (SOMA-X in v0.5),
  SMPL-X-competitive accuracy (2.4mm fitting error, 3DBodyTex). No clothing/textures.

**Generation shortlist** *(unverified tier)*:
| Model/API | VRAM / cost | License | Notes |
|---|---|---|---|
| Hunyuan3D-2GP / WinPortable | ~3GB shape, ~6GB texture (mmgp offload, needs ~24GB RAM); 24GB native | open | ships local **API-server mode** → Forge talks HTTP |
| TripoSR | 8GB-class, sub-second | MIT | most viable pure-local; weakest textures |
| TRELLIS | 16GB+ | MIT | out of local budget on RTX 1000 Ada |
| Meshy API | free 200 cr/mo; ~$0.10–0.30/asset | SaaS | texturing endpoint too |
| Tripo API | $0.01/credit PAYG | SaaS | **auto-rigging** — pairs with skinning (#82) |
| Rodin (Hyper3D) | $120/mo floor | SaaS | skip v1 |

GLB is every provider's common output → the existing tinygltf path is the ingestion route.

## 5. Engine gaps — prerequisites vs deferrable

Prerequisites for the human milestone (in order): **multi-material meshes** (#80 — characters
are skin+eyes+teeth in one mesh; glTF per-primitive materials currently flatten), **full PBR
texture maps** (#16), **UV unwrap** (#81 — xatlas; every texturing path needs UVs),
**skeletal skinning + rigged glTF import** (#82 — Anny/Tripo/MPFB all arrive rigged),
**RT texture sampling** (#17 — photoreal renders of textured assets).

Deferrable: animation playback (posing suffices), SSS skin shading (wrap-diffuse first),
hair strands (mesh cards v1), UV *editing* tools (unwrap only).

## 6. Roadmap

Epic #74. Order: #75 → #76 → #77 → #78 → #79 (agent builds prop scenes) → #80 → #16 → #81 →
#82 → #17 (complex-asset engine floor) → #83 → #84 (generation) → #85 → #86 (human milestone).

Milestone acceptance (#86): a fresh Claude session, told only *"create a realistic middle-aged
man, casually posed, studio-lit, and render a portrait"*, completes unassisted via MCP.

## Open questions (benchmark early)

- Iteration count per asset class for the render-feedback loop — LL3M/BlenderAlchemy report
  qualitative results only; measure once #76–#78 land.
- cpp-httplib + MinGW-w64 GCC 13.2 behavior under streaming responses (spike in #75).
- Anny → glTF export fidelity (rig + weights) — validate in a #85 spike before committing to
  the sidecar API shape.

## Key sources

Epic UE 5.8 MCP docs · ChiR24/Unreal_mcp · CoplayDev/unity-mcp · ahujasid/blender-mcp ·
LL3M (arXiv:2508.08228) · BlenderAlchemy (arXiv:2404.17672) · SceneCraft (arXiv:2403.01248) ·
3D-GPT (arXiv:2310.12945) · Anny (arXiv:2511.03589, github.com/naver/anny) ·
SMPL-X model license (smpl-x.is.tue.mpg.de) · MPFB2 LICENSE.md · Hunyuan3D-2GP ·
Hunyuan3D-2-WinPortable · meshcapade.com/smpl
