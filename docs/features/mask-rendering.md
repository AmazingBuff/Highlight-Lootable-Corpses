# Mask rendering

Status: current

Canonical path: `docs/features/mask-rendering.md`

Last verified against: 2026-09-17; Release build and WARP checks described below. Live game validation remains manual.

## Purpose

Provide wall-penetrating highlights with target-specific colors. Silhouettes show the nearest highlighted surface at each pixel; outlines preserve each target's border even inside another target's silhouette.

## Scope and non-goals

The renderer supports the existing validated static meshes and GPU-skinned partitions. This change does not add target categories, change the corpse scanner, change icon mode, add OIT, or compare highlights with scene depth. Transparent textures and internal material boundaries are not reconstructed.

## Architecture

`OutlineMask::set_targets` retains references and validates finite RGB/opacity. Geometry is collected once in target order. Each draw carries a zero-based integer `target_index`; the geometry pixel shader writes `target_index + 1` to a private `R32_UINT` mask. Zero means background. A resizable structured buffer stores unpremultiplied RGB and authoritative opacity for each retained target.

- **Silhouette:** clear the private mask and `D32_FLOAT` depth buffer; draw all targets with depth writes and `GREATER`; composite only the winning ID's style. Final alpha is `target.opacity * 0.5`. Rear highlight colors do not show through a translucent front highlight; the original game image does.
- **Outline:** reuse and clear the mask for each contiguous target draw span. All meshes of that target form one union, with no inter-target depth test. The outline shader searches the existing square neighborhood (radius clamped to 1–6 pixels), outputs only outside the union, and blends directly into the supplied overlay RTV. Interiors output zero and cannot erase an earlier border. Crossing colored borders use premultiplied source-over in retained input-target order: later targets blend over earlier targets. This is deterministic for a fixed input order, not a depth-sorted border policy.

Outline processing currently performs one full-screen neighborhood pass per target with accepted draws. It avoids copying or repeatedly collecting geometry, but GPU work grows with target count, resolution and thickness. Conservative per-target screen bounds are a possible future optimization.

## Code map

| Path | Stable entry point | Responsibility |
| --- | --- | --- |
| [outline_mask.h](../../src/render/mask/outline_mask.h) | `OutlineMaskTarget`, `OutlineMask::set_targets` | Target RGB, opacity and reference boundary |
| [outline_mask.cpp](../../src/render/mask/outline_mask.cpp) | `render_impl` | Mode dispatch, grouping, resource rebuild, state guard |
| [mask_geometry.cpp](../../src/render/mask/mask_geometry.cpp) | `collect_mask_draws` | Validated geometry collection and integral target index |
| [mask_depth.h](../../src/render/mask/mask_depth.h) | `make_private_depth_projection` | Private perspective depth convention |
| [mask_passes.cpp](../../src/render/mask/mask_passes.cpp) | `RenderTarget::init`, `FullscreenPass::update_styles` | D3D resources and style transport |
| [mask_geometry.hlsl](../../src/render/shaders/mask_geometry.hlsl) | `vs_static_main`, `vs_skinned_main`, `ps_main` | Geometry/palette transforms and categorical ID writes |
| [mask_composite.hlsl](../../src/render/shaders/mask_composite.hlsl) | `ps_silhouette_main`, `ps_outline_main` | Integer loads, style lookup and premultiplied output |
| [d3d11_util.cpp](../../src/render/dx11/d3d11_util.cpp) | `D3D11StateCapture` | Capture/restore changed PS SRV slots 0 and 1 and existing pipeline state |
| [smoke.cpp](../../tests/mask_rendering/smoke.cpp) | `mask_test::run` | Actual HLSL WARP rendering and readback assertions |

## Interfaces

`OutlineMaskTarget` supplies `ref`, `opacity`, and `DirectX::XMFLOAT3 color`. RGB is unpremultiplied and clamped to [0,1]; opacity is finite, positive and clamped to 1. Invalid targets and zero-opacity targets are omitted, so invisible targets cannot occlude other highlights. The default RGB is white; callers should pass their intended color explicitly.

The corpse caller in [renderer.cpp](../../src/render/renderer.cpp) passes configured RGB and the existing `pulse * corpse_alpha(...)`, which already includes configured alpha and distance fade. The render passes never multiply configured alpha a second time. Outline alpha is target opacity; silhouette alpha is half that value. Existing configuration keys and icon behavior are unchanged.

## Invariants

- IDs are integer and never blended or filtered; `Texture2D<uint>.Load` reads categorical values. Background never indexes the style table.
- The style table has one entry per retained target. IDs above 255 no longer alias. The pre-existing 256 **geometry draw** budget remains; this is not unlimited rendered-target support.
- The camera uses the existing column-vector matrix contract. Replace only its z row with `(0,0,0,near * length(w-row.xyz))` before multiplying world or skin transforms. Thus z/w is reversed perspective depth, independent of the engine's raw z convention. x/y/w and matrix upload conventions are unchanged.
- Clear private depth to 0, compare `GREATER`, write depth, and enable depth clipping. Positive near depth maps to 1; farther surfaces approach 0. Coplanar ties retain the first submitted surface. The map has an infinite far plane and does not use scene depth.
- Orthographic cameras, nonfinite matrices, nonpositive near distances, and zero w gradients are rejected. Perspective cameras remain the supported contract; stereo/VR eye integration is not newly implemented or verified.
- All meshes of one target must remain contiguous in the collected draw list. Outline masks must contain only that target's ID.
- The style buffer is dynamically **sized** but uses `D3D11_USAGE_DEFAULT`; structured resources cannot combine CPU access flags. Bounded `UpdateSubresource` uploads only populated bytes.
- Mask SRVs are explicitly unbound before RTV reuse. Fullscreen PS b0 is restored locally; the outer state capture restores t0/t1 and existing state even if rendering throws.

## Failure modes

| Failure | Expected handling | Verification |
| --- | --- | --- |
| Missing/invalid target, zero opacity, unloaded 3D | Omit target/draw; no invisible occluder | Boundary inspection; game manual checks |
| Invalid/orthographic camera | Skip frame; bounded warning | Shared depth helper assertions |
| Texture/view/pipeline creation failure | Skip rendering; release partial resources; rebuild resources on subsequent resize/device transition | Resource ownership review and WARP creation checks |
| Style allocation/update or device failure | Mark styles invalid and skip frame; never draw stale styles | Creation guards and device-removal check; failure injection not automated |
| Exception after state capture | Scope guard restores game state; Present boundary logs and skips | Guard lifetime review; live injection not automated |
| Resize or device change | Release mask/depth/views, geometry pipeline/layout cache, and both consumer pipelines/style tables before recreation | Release build; WARP equivalent-resource recreation |
| Unsupported mesh/palette or draw-budget exhaustion | Existing skip/log/budget behavior remains | Existing geometry validation; game manual checks |

Pipeline creation failure stays disabled until resource rebuild; target allocation and style failures can retry. The shader harness exercises production shaders and equivalent D3D states, not the live engine facade, scanner, COM allocation-failure paths or camera ABI.

## Dependencies

Existing Windows SDK D3D11/D3DCompiler/DirectXMath, CommonLibSSE and the installed MSVC toolchain. No new package, library, runtime service or shader compiler dependency is introduced.

## Tests and verification

From the repository root:

```powershell
cmake --build build --config Release --target HighlightLootableCorpses
./tests/mask_rendering/run.ps1
```

The runner uses an x64 developer shell or discovers Visual Studio through `vswhere`. If needed, pass `-VcVars 'absolute/path/to/vcvars64.bat'`. Build artifacts remain under `build/mask_rendering`.

The harness compiles all six production shader entries and uses WARP readback to check nearest color/opacity in both draw orders, intersecting depths, non-unit skin weights, empty background, near clipping, rear borders inside front silhouettes, whole-target mesh unions, crossing-border source-over, IDs above 255, and new-size/new-device resources. It checks D3D11 debug warnings/errors when the SDK debug layer is available and explicitly reports its absence otherwise. It also validates scaled near depth, unchanged x/y/w, and invalid camera rejection through the shared helper.

Manual game checks still required: overlapping and intersecting corpses while moving the camera; front/rear targets behind a wall; multi-part skinned bodies and static ash piles; pulse/distance fades; mode switching; window resize/device recovery; and performance with many targets at maximum thickness. No in-game result is claimed by the WARP test.

## Safe modification guidance

1. Extend target style data at the target boundary, keeping opacity authoritative and C++/HLSL layouts synchronized.
2. Preserve ID zero, no blending/filtering, private depth convention, contiguous whole-target grouping and exception-safe state restoration.
3. Run the shader readback harness and Release build. Recheck overlap colors and near clipping before game validation; measure full-screen outline cost before increasing budgets.

## Synchronized files

| Paths | Reason |
| --- | --- |
| `src/render/mask/mask_types.h`, `src/render/mask/outline_mask.h`, `src/render/renderer.cpp` | Target color, opacity and ID contract |
| `src/render/mask/mask_passes.h`, `src/render/mask/mask_passes.cpp`, both mask HLSL files | Resource formats, slots, constant layouts and entry points |
| `src/render/mask/mask_depth.h`, `src/render/mask/outline_mask.cpp`, `tests/mask_rendering/smoke.cpp` | Perspective depth and independent-object semantics |
| `src/render/dx11/d3d11_util.h`, `src/render/dx11/d3d11_util.cpp` | Captured resource slots |
| [feature index](README.md), [Chinese README](../../README.md), [English README](../../README_EN.md), [Changelog](../../CHANGELOG.md) | Mode behavior and navigation |

## Related history

The former shared RGBA8/MAX mask selected larger encoded IDs and erased boundaries inside the union of overlapping targets. The current design separates nearest-surface silhouettes from independent outlines, retains matrix/palette migration, and removes the 8-bit style-ID aliasing limit. See [Unreleased changes](../../CHANGELOG.md).
