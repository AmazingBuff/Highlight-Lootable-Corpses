# Icon rendering

## Purpose and scope

Icon mode locates lootable corpses with a downward arrow above their bounds. Nearby crowded targets share a larger double arrow. Distance controls bounded size in addition to the existing opacity fade. Silhouette/outline rendering, scanner behavior, loot rules and configuration storage remain unchanged.

## Architecture and code map

1. `OverlayDirector::draw` consumes `CorpseScan::snapshot`, computes an icon-only top anchor, projects it and forms visible candidates with the existing `pulse * corpse_alpha(...)` opacity.
2. `IconLayout::update` validates candidates, rebuilds bounded previous groups, then greedily combines compatible groups. FormID history preserves group membership and representatives across snapshot reorder and threshold jitter.
3. All candidates participate before the nearest 16 groups are selected. `IconOverlay::add_marker` consumes `icon_geometry`; selected markers draw from far to near so closer markers take visual priority when unrelated icons still overlap.
4. The existing icon shader premultiplies RGB once and the overlay uses source-over blending with no scene depth. A captured full-backbuffer viewport is set for drawing and then restored.

| Path | Entry point / responsibility |
| --- | --- |
| [renderer.cpp](../../src/render/renderer.cpp) | `OverlayDirector::draw`: projection, opacity, resets and mode integration |
| [icon_layout.h](../../src/render/icon/icon_layout.h), [icon_layout.cpp](../../src/render/icon/icon_layout.cpp) | `icon_anchor`, `icon_screen_tip`, `icon_clip_tip`, `icon_radius`, `IconLayout::update` |
| [icon_geometry.h](../../src/render/icon/icon_geometry.h), [icon_geometry.cpp](../../src/render/icon/icon_geometry.cpp) | `IconVertex`, `IconGeometry`, `icon_geometry`: shared GPU-ready triangle generation |
| [icon_overlay.cpp](../../src/render/icon/icon_overlay.cpp) | `begin_frame`, `add_marker`, `draw`: staging and D3D11 submission |
| [icon_overlay.hlsl](../../src/render/shaders/icon_overlay.hlsl) | `vs_main`, `ps_main`: unchanged vertex color pipeline |
| [tests/icon_rendering](../../tests/icon_rendering/run.ps1) | standalone layout/geometry tests, WARP readback and synthetic preview |

## Interfaces and defaults

`IconCandidate` carries FormID, original world anchor, screen tip, player distance and effective opacity. `IconMarker` carries an actual representative FormID, member count, tip, half-width and opacity. These helpers depend only on STL and DirectXMath; they do not query Skyrim.

Internal tuning defaults:

| Rule | Value |
| --- | --- |
| Pair world distance | At most 180 game units |
| Pair anchor height difference | At most 64 game units |
| Pair screen distance | At most `max(24, 2 * max(single_radius_A, single_radius_B) + 8)` pixels |
| Existing-group exit thresholds | 1.25 times the join thresholds |
| Tip offset after projection | 6 pixels upward |
| Single distance size | `base * (1.25 - 0.5 * smoothstep(clamp(distance/max_distance, 0, 1)))` |
| Group size | Single size using nearest member distance, times 1.2; at most 1.5 base |
| Group opacity | Maximum current member opacity |
| Output budget | Nearest 16 groups, distance then representative FormID |

`IconRadius` remains the persisted key and retains its 5–20 pixel range. It now means base half-width; the menu label is **Icon Size**. No new settings or default-loading changes are introduced. The README's value 10 is an example, not a new configured default.

## Invariants

- Every member pair must satisfy world, height and screen bounds. Connected chains cannot expand a group beyond the bounds. Only pairs from the same previous group use exit thresholds.
- Existing groups rebuild before accepting new members; arriving low IDs cannot steal part of an otherwise retained group. Greedy grouping uses quadratic pair checks, not globally optimal clustering. Input order is normalized; membership lookups are hashed.
- Representatives remain actual members. A retained representative is preferred; when groups merge, the lowest surviving representative FormID wins. If none survives, choose the member nearest the world centroid, breaking ties by FormID.
- All candidate memberships are retained even when the output budget hides their groups. Removed or invalid members disappear immediately. Empty inputs, disabled display, inactive pulse, non-icon modes and viewport changes clear history.
- The source corpse anchor and bounds are never modified. Valid finite AABBs with positive extent on all axes yield the top center; otherwise use the existing anchor. Icons bypass mask sphere culling and use their own projected tip visibility.
- Invalid distances, opacity, positions or viewport dimensions are rejected. The icon-only matrix fallback requires positive clip w; behind-camera or offscreen projections have no edge indicator. Geometry may clip at screen edges when its tip is visible.
- All geometry extends upward from an immutable bottom tip. Groups use two separated triangles. Each triangle is one fill triangle plus a six-triangle border ring: 21 vertices single, 42 double, at most 672 for 16 double markers. The border and fill do not cover the same interior pixels.
- Opacity is authoritative; the geometry does not multiply configured alpha again. Zero opacity produces no geometry. Staging clears at frame entry and after draw, including skipped frames.

## Failure modes and limitations

| Condition | Handling |
| --- | --- |
| Invalid/degenerate AABB | Fall back to original anchor |
| Missing projection or invalid/offscreen tip | Omit target; no fake edge direction |
| No valid targets, inactive mode/pulse | Clear icon history and staging |
| More than 16 resulting groups | Display nearest 16; retain history for hidden groups |
| Different floors / distant targets at same screen position | Keep separate; residual overlap is possible |
| Vertex capacity reached | Omit whole marker, never a partial arrow |
| D3D map/pipeline failure | Skip drawing; no stale staged geometry |

Grouping is a deterministic greedy approximation. Legitimate floor changes within 64 units can still qualify, and threshold values need gameplay tuning. The snapshot's bounds may lag animation between scans. Stable representatives may sit away from a moving group's center by design. Stereo/VR, live device recovery and game performance are not newly validated.

## Dependencies

Existing STL, DirectXMath, D3D11/D3DCompiler, DirectXTK and CommonLibSSE. Tests use installed MSVC/Windows SDK and PowerShell System.Drawing to label a preview. No package or asset dependency is added.

## Tests and verification

From the repository root:

```powershell
cmake -S . -B build -DVCPKG_MANIFEST_INSTALL=OFF
cmake --build build --config Release --target HighlightLootableCorpses -- /m:2 /p:CL_MPCount=2
./tests/icon_rendering/run.ps1
```

The offline configure command assumes dependencies are already installed in the existing build environment. The test runner discovers an installed x64 MSVC environment; pass `-VcVars 'absolute/path/to/vcvars64.bat'` to select one explicitly. Outputs remain in `build/icon_rendering`.

`smoke.cpp` calls the production layout/geometry helpers to verify joins and exits, complete-link rejection, height/world/screen separation, reorder stability, retained/removed representatives, empty/viewport resets, more-than-16 candidate grouping, hidden membership history and output priority, scaling, invalid AABBs and projections, tip anchoring, single/double shape area, zero alpha and capacity.

`gpu.cpp` compiles the actual icon HLSL entries, renders production vertices through WARP with equivalent blend/raster states and reads pixels back. It checks nonoverlapping border/fill coverage at alpha 0.25, untinted premultiplied fill, zero-alpha omission and visible border/fill at the minimum base size 5. The debug layer is checked when available. `preview.png` shows base sizes 5/10/20, near/far, single/group. It is a synthetic shader rendering, not a game screenshot; preview opacity 0.9/0.4 is illustrative, not a change to the fade settings.

Manual checks still required: camera movement around crowded corpses, uneven ground and floors, changing group members, looting/removal, pulse expiry, mode switching, resizing and readability over bright/dark gameplay. The harness exercises shared algorithms and actual shaders, not live engine projection, scanner timing or the complete Present facade.

## Safe modification guidance

Keep clustering bounds, persistent identity and the output cap separate. Change tuning constants in `icon_layout.cpp` with tests for joins and exits. Preserve the tip anchor and single-coverage ring when altering geometry; run the WARP alpha check at the smallest allowed size. Do not alter shared corpse anchors or mask culling to reposition icons.

## Synchronized files

Layout/geometry headers and implementations, `renderer.cpp`, `icon_overlay.cpp`, [config.cpp](../../src/config/config.cpp), [ui_menu.cpp](../../src/ui/ui_menu.cpp), the test runner and test sources, [feature index](README.md), [Chinese README](../../README.md), [English README](../../README_EN.md), and [Changelog](../../CHANGELOG.md).

## Related history

Previously icon mode drew one fixed-radius circle per corpse at the shared center anchor and used opacity alone for distance. This change adds above-object pointers, bounded size and stable crowd grouping while preserving the existing color/pulse/fade inputs. See the Unreleased icon entries in the Changelog.
