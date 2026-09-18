# Mask rendering

Status: current

Canonical path: `docs/features/mask-rendering.md`

Last verified against: 2026-09-18; Release build and WARP checks described below. Live game validation remains manual.

## Purpose

Provide wall-penetrating highlights with target-specific colors. Silhouettes show the nearest highlighted surface at each pixel; outlines preserve each target's border even inside another target's silhouette, with a bright narrow rim and a smoothly fading exterior halo.

## Scope and non-goals

The renderer supports the existing validated static meshes and GPU-skinned partitions. This change does not add target categories, change the corpse scanner, change icon mode, add OIT, or compare highlights with scene depth. Transparent textures and internal material boundaries are not reconstructed.

## Architecture

`OutlineMask::set_targets` retains non-null target references and callers provide the RGBA style. Geometry is collected once in target order. Each draw carries a zero-based integer `target_index`; the geometry pixel shader writes `target_index + 1` to a private `R32_UINT` mask. Zero means background. A resizable structured buffer stores unpremultiplied RGB and the caller-provided alpha for each retained target.

- **Silhouette:** clear the private mask and `D32_FLOAT` depth buffer; draw all targets with depth writes and `GREATER`; composite only the winning ID's style. Final alpha is `color.w * 0.5`. Rear highlight colors do not show through a translucent front highlight; the original game image does.
- **Outline:** clear the full integer mask once per active outline frame, then preserve earlier target IDs while drawing each contiguous target span. All meshes of that target form one union, with no inter-target depth test. The horizontal separable Gaussian pass reads the categorical `R32_UINT` mask and writes narrow and wide coverage to one reusable `R16G16_FLOAT` scratch texture. The vertical pass reads that scratch plus the raw mask, rejects only the current object's raw interior, looks up the explicit current object ID, and writes a bright narrow core over a wider colored halo into the supplied overlay RTV. Crossing colored borders use premultiplied source-over in retained input-target order: later targets blend over earlier targets. This is deterministic for a fixed input order, not a depth-sorted border policy.

Outline processing performs two separable Gaussian passes per accepted target inside conservative screen regions. The union of accepted `node->worldBound` spheres is projected to a base rectangle `B` with a 2-pixel margin; geometry uses `B`, horizontal coverage uses `B` expanded by the support radius on X, and the final vertical/composite pass uses `B` expanded on both axes. Invalid, near-crossing, unavailable or transposed bounds use the full viewport; valid fully offscreen bounds are empty. The bounded support radius is `3 * OutlineThickness + 3` pixels (6–18 for the existing setting range 1–5), with CPU-normalized narrow and wide weights packed into the glow constant buffer. The viewport and full-size scratch allocation remain unchanged. For a 40×40 WARP framebuffer with a representative 16×16 target plus margin, the calculated H+V scissor area fell from 3200 to 2204 pixels (31.125%); this is synthetic area evidence, not measured GPU time or a game FPS claim.

## Code map

| Path | Stable entry point | Responsibility |
| --- | --- | --- |
| [outline_mask.h](../../src/render/mask/outline_mask.h) | `OutlineMaskTarget`, `OutlineMask::set_targets` | Target RGB, opacity and reference boundary |
| [outline_mask.cpp](../../src/render/mask/outline_mask.cpp) | `render_impl` | Mode dispatch, grouping, resource rebuild, state guard |
| [mask_geometry.cpp](../../src/render/mask/mask_geometry.cpp) | `collect_mask_draws` | Validated geometry collection and integral target index |
| [mask_depth.h](../../src/render/mask/mask_depth.h) | `make_private_depth_projection` | Private perspective depth convention |
| [mask_passes.cpp](../../src/render/mask/mask_passes.cpp) | `GlowScratch::init`, `OutlinePass::draw` | D3D resources, packed glow constants and scissored H/V execution |
| [outline_glow.h](../../src/render/mask/outline_glow.h) | `make_kernel_profile` | Clamped thickness mapping and normalized CPU Gaussian weights |
| [outline_roi.h](../../src/render/mask/outline_roi.h) | `make_region`, `expand` | Projected sphere rectangles, fallback classification and H/V domains |
| [mask_geometry.hlsl](../../src/render/shaders/mask_geometry.hlsl) | `vs_static_main`, `vs_skinned_main`, `ps_main` | Geometry/palette transforms and categorical ID writes |
| [mask_composite.hlsl](../../src/render/shaders/mask_composite.hlsl) | `ps_silhouette_main`, `ps_outline_main` | Silhouette output and retained legacy hard-band comparison entry |
| [mask_glow.hlsl](../../src/render/shaders/mask_glow.hlsl) | `ps_glow_horizontal`, `ps_glow_vertical` | Separable coverage and premultiplied core/halo composition |
| [d3d11_util.cpp](../../src/render/dx11/d3d11_util.cpp) | `D3D11StateCapture` | Capture/restore PS SRV slots 0–2, scissor rectangles and existing pipeline state |
| [smoke.cpp](../../tests/mask_rendering/smoke.cpp) | `mask_test::run` | Actual HLSL WARP rendering and readback assertions |
| [smoke.cpp](../../tests/outline_glow/smoke.cpp) | `run`, `main` | Production glow shader WARP checks, kernel boundaries and PNG comparison |

## Interfaces

`OutlineMaskTarget` supplies `ref` and one unpremultiplied `DirectX::XMFLOAT4 color`. The RGB channels carry the configured target color and `color.w` carries the caller-provided pulse, alpha and distance fade. `OutlineMask::set_targets` retains non-null references and does not add another opacity clamp or filter; the existing geometry collector remains responsible for unsupported or unloaded geometry.

The corpse caller in [renderer.cpp](../../src/render/renderer.cpp) passes configured RGB and the existing `pulse * corpse_alpha(...)`, which already includes configured alpha and distance fade. The render passes apply that target alpha once: silhouette coverage is multiplied by `color.w * 0.5`, while the glow combines core and halo coverage first and then multiplies premultiplied RGB and alpha by `color.w`. A zero style alpha is visually transparent but remains an ordinary retained target in the mask. Existing configuration keys and icon behavior are unchanged.

## Invariants

- IDs are integer and never blended or filtered; `Texture2D<uint>.Load` reads categorical values. Background never indexes the style table. Glow horizontal coverage writes only floating-point coverage to `R16G16_FLOAT`; categorical IDs never enter a filtered resource.
- The style table has one entry per retained target. IDs above 255 no longer alias. The pre-existing 256 **geometry draw** budget remains; this is not unlimited rendered-target support.
- The camera uses the existing column-vector matrix contract. Replace only its z row with `(0,0,0,near * length(w-row.xyz))` before multiplying world or skin transforms. Thus z/w is reversed perspective depth, independent of the engine's raw z convention. x/y/w and matrix upload conventions are unchanged.
- Clear private depth to 0, compare `GREATER`, write depth, and enable depth clipping. Positive near depth maps to 1; farther surfaces approach 0. Coplanar ties retain the first submitted surface. The map has an infinite far plane and does not use scene depth.
- Orthographic cameras, nonfinite matrices, nonpositive near distances, and zero w gradients are rejected. Perspective cameras remain the supported contract; stereo/VR eye integration is not newly implemented or verified.
- All meshes of one target must remain contiguous in the collected draw list. Outline masks must contain only that target's ID.
- Outline mode clears the full integer mask once before its target loop. Foreign IDs remain elsewhere while each target overwrites only its own geometry rectangle; vertical interior rejection compares the current object ID, so a retained foreign ID cannot suppress a later target's halo.
- Missing nodes, nonfinite or nonpositive world spheres, unsafe projections, near-plane crossings and transposed upload orientation select the full viewport. A valid fully offscreen sphere union is empty and skips that target. Skinned draws with missing world bounds therefore remain renderable through the full fallback.
- The style buffer is dynamically **sized** but uses `D3D11_USAGE_DEFAULT`; structured resources cannot combine CPU access flags. Bounded `UpdateSubresource` uploads only populated bytes.
- Mask and scratch SRVs are explicitly unbound before RTV reuse. Fullscreen PS b0 is restored locally; the outer state capture restores t0–t2, rasterizer, viewport and captured scissor rectangles/count even if rendering throws. Silhouette remains on its 16-byte CB; glow uses a separate 192-byte packed CB containing the horizontal dependency rectangle.

## Failure modes

| Failure | Expected handling | Verification |
| --- | --- | --- |
| Null target reference, unsupported/unloaded 3D, or zero style alpha | Null references are omitted; existing geometry collection skips unsupported/unloaded draws; zero style alpha composites transparently while retaining the target's normal mask path | Boundary inspection; game manual checks |
| Invalid/orthographic camera | Skip frame; bounded warning | Shared depth helper assertions |
| Texture/view/pipeline creation failure | Skip rendering; release partial mask, scratch and view resources; rebuild resources on subsequent resize/device transition | Resource ownership review and WARP creation checks |
| Invalid or unavailable ROI bound | Use the full viewport and full-size scratch allocation; never clip a draw from an untrusted bound | Pure ROI fallback tests and source review |
| Style allocation/update or device failure | Mark styles invalid and skip frame; never draw stale styles | Creation guards and device-removal check; failure injection not automated |
| Exception after state capture | Scope guard restores game state; Present boundary logs and skips | Guard lifetime review; live injection not automated |
| Resize or device change | Release mask/depth/views, reusable glow scratch, geometry pipeline/layout cache, and both consumer pipelines/style tables before recreation | Release build; WARP equivalent-resource recreation |
| Unsupported mesh/palette or draw-budget exhaustion | Existing skip/log/budget behavior remains | Existing geometry validation; game manual checks |

Pipeline creation failure stays disabled until resource rebuild; target allocation and style failures can retry. The shader harness exercises production shaders and equivalent D3D states, not the live engine facade, scanner, COM allocation-failure paths or camera ABI.

## Dependencies

Existing Windows SDK D3D11/D3DCompiler/DirectXMath, CommonLibSSE and the installed MSVC toolchain. No new package, library, runtime service or shader compiler dependency is introduced.

## Tests and verification

From the repository root:

```powershell
cmake --build build --config Release --target HighlightLootableCorpses
./tests/mask_rendering/run.ps1
./tests/outline_glow/run.ps1
```

The runners use an x64 developer shell or discover Visual Studio through `vswhere`. If needed, pass `-VcVars 'absolute/path/to/vcvars64.bat'`. Build artifacts remain under `build/mask_rendering` and `build/outline_glow`.

The mask harness compiles the existing geometry and silhouette entries. The outline-glow harness compiles the production `MaskGlow` entries and uses WARP readback to check normalized thickness profiles, projected ROI unions and fallback cases, H/V dependency expansion, full-vs-local pixel equivalence for thickness 1/2/5 and moving regions, poisoned scratch and reused IDs, tight-core/wide-halo monotonic fade, finite support, transparent interiors and empty frames, zero and proportional target alpha, target colors, screen-edge sampling, retained overlapping halos, scratch recreation at another size, and D3D11 debug warnings/errors when the SDK debug layer is available. It reports exact synthetic H/V work-area reduction and emits `build/outline_glow/outline_glow_comparison.png`, a dark/light four-panel comparison of the retained legacy hard band and the new glow on a larger rounded human-like silhouette. Synthetic WARP results do not claim live-game appearance or performance.

Manual game checks still required: overlapping and intersecting corpses while moving the camera; front/rear targets behind a wall; multi-part skinned bodies and static ash piles; pulse/distance fades; mode switching; window resize/device recovery; and performance with many targets at maximum thickness. No in-game result is claimed by the WARP test.

## Safe modification guidance

1. Extend target style data at the target boundary, keeping opacity authoritative and C++/HLSL layouts synchronized.
2. Preserve ID zero, no blending/filtering, private depth convention, contiguous whole-target grouping and exception-safe state restoration.
3. Run both shader readback harnesses and the Release build. Recheck overlap colors, core/halo falloff, ROI fallback and near clipping before game validation; treat the measured WARP area reduction as synthetic evidence rather than a gameplay performance claim.

## Synchronized files

| Paths | Reason |
| --- | --- |
| `src/render/mask/mask_types.h`, `src/render/mask/outline_mask.h`, `src/render/renderer.cpp` | Target color, opacity and ID contract |
| `src/render/mask/mask_passes.h`, `src/render/mask/mask_passes.cpp`, `src/render/mask/outline_glow.h`, `src/render/mask/outline_roi.h` | Scratch ownership, CB packing, thickness mapping, ROI domains and pass lifecycle |
| `src/render/shaders/mask_composite.hlsl`, `src/render/shaders/mask_glow.hlsl` | Silhouette, legacy comparison, separable coverage and glow composition |
| `src/render/mask/mask_depth.h`, `src/render/mask/outline_mask.cpp`, `tests/mask_rendering/smoke.cpp`, `tests/outline_glow/smoke.cpp` | Perspective depth, independent-object semantics and WARP validation |
| `src/render/dx11/d3d11_util.h`, `src/render/dx11/d3d11_util.cpp` | Captured resource slots 0–2 |
| [feature index](README.md), [Chinese README](../../README.md), [English README](../../README_EN.md), [Changelog](../../CHANGELOG.md) | Mode behavior and navigation |

## Related history

The former shared RGBA8/MAX mask selected larger encoded IDs and erased boundaries inside the union of overlapping targets. The current design separates nearest-surface silhouettes from independent outlines, retains matrix/palette migration, and removes the 8-bit style-ID aliasing limit. See [Unreleased changes](../../CHANGELOG.md).
