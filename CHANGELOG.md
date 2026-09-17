# Changelog

## [Unreleased]

### Changed

- Change silhouettes to show the nearest highlighted surface per pixel, including intersecting targets, while preserving wall penetration.
- Add per-target highlight colors while preserving corpse color, pulse and distance fade settings.

- **BREAKING:** Rename the plugin to `HighlightLootableCorpses`. The DLL, INI, and log files are renamed. Migration: rename your `CorpseESP.ini` to `HighlightLootableCorpses.ini` to keep your settings, and replace the old DLL.
- The toggle hotkey is now unbound by default. Bind one via the new "Hotkey" button in the MCP menu — all keys are bindable (keyboard and mouse), including ESC/F1 (pressing them may close the panel, but the bind still applies; click the button again or wait 5s to cancel). Bind it via the `Hotkey` INI key alternatively. Hotkey toggles stay in sync with the menu's Enabled checkbox and are suspended while the menu is open.
- The INI is now written with a comment above each option (same style as the configuration reference in the README) and is saved automatically whenever the player makes a save game after changing settings in the MCP menu — pressing "Save to INI" is no longer required (that button still saves immediately).
- Loot filter category switches now default to **false** (opt-in per category), and their controls are greyed out in the MCP menu while the loot filter is off.
- **BREAKING:** Replace the corpse box wireframe with a `DisplayMode` setting (`silhouette`, `outline`, or `icon`) in the Display section: `silhouette` fills the wall-penetrating mesh silhouette in `OutlineColor`, `outline` draws a band just outside it, and `icon` shows a small circular marker at the corpse's screen position. The box wireframe is removed. All three modes fade with distance according to `MinOpacity`, `FadeStartDistance`, and `FadePower`.

### Added

- Add an optional "hide searched corpses" mode (`HideSearchedEnabled`, off by default): once the player searches a corpse, it stops being outlined — even if nothing was taken. Searching is always recorded (so corpses searched before enabling the option are hidden too once it is turned on). QuickLoot IE users are covered via its public API (opening the loot menu marks the corpse as searched); QuickLoot IE is an optional dependency and its absence falls back to vanilla activation events only.
- Add an experimental mesh outline mask renderer: corpse meshes — fully GPU-skinned via per-partition bone matrices — are rendered into an offscreen integer-ID mask (private depth for silhouettes, independent masks for outlines; both ignore scene depth) whenever corpses are in range, and a translucent on-screen overlay shows the silhouette for verification.
- Add the edge-detection outline pass consuming the mask: a full-screen pass dilates the mask by the configured `OutlineThickness` and draws an `OutlineColor` band just outside each corpse silhouette, penetrating walls; the silhouette interior is left to the verification overlay.
- Searched-corpses marks persist per save game via the SKSE co-save (record `HLCS`): form IDs are re-resolved on load (stale marks are dropped), marks are removed when the engine deletes a form, and loading a save without marks (or removing the plugin) leaves saves fully intact.

### Removed

- Remove the model-silhouette outline mode (`OutlineMode`) to eliminate its heavy performance cost.
- Remove the `SoulGemFilledOnly` loot filter option; soul gems always count as consumables.
- Remove the redundant `ShowOutline` option; the plugin toggle (`Enabled`) already covers it.

### Fixed

- Preserve each target's outline when another highlighted target covers its silhouette, and merge crossing outlines with alpha blending.
- Prevent target style IDs above 255 from aliasing earlier entries; the existing geometry draw budget remains unchanged.

- Fix the QuickLoot IE compatibility layer never activating: SKSE loads plugins one by one in (alphabetical) scan order and `HighlightLootableCorpses` loads before `QuickLootIE`, so probing for its DLL inside `SKSEPlugin_Load` always failed. Detection now happens on the `kPostLoad` message (after every plugin's `SKSEPlugin_Load` has returned) and requires only API v20, matching the one call actually used.
- Support both QuickLoot IE API generations: 4.x (`GetQuickLootInterfaceV20` C export, vendored official header) and 3.x (PluginRequests over the SKSE messaging interface, vendored official header with the namespace renamed to avoid clashing with 4.x). Detection tries the 4.x export first and falls back to the 3.x handshake, so the searched-corpses marking works with either QuickLoot IE version.
- Searched-corpses marks now record only corpse-like references (dead actors, ash piles, static corpse containers — the same candidates the scan outlines). Previously every player-activated door, lever, or chest entered the set and grew the co-save record indefinitely.
- Fix box/loot state mismatches on leveled-list corpses (e.g. draugr weapons inherited via NPC templates): lootability is now judged from the same inventory view the loot menu uses — engine-initialized inventory changes merged with base-container entries and dropped-item lists, skipping unresolved leveled-list placeholder entries (QuickLoot IE approach).
- Fix boxes persisting on looted-empty corpses by ignoring resolved leveled-list placeholder entries in the searchable check.
- Stop drawing boxes on corpses whose loot has been fully looted by ignoring non-playable items in the searchable check.
- Prevent per-scan corpse diagnostics from flooding the log with unchanged entries.
- Fix the experimental outline mask renderer treating effect-shader attachment meshes (glow sprites, frost/trail art) on corpse 3D as regular static geometry — a mesh whose vertices are not ordinary model-space surface data could cover the whole screen in the debug overlay. Effect-shader geometry is now excluded, and every static draw must pass world-sphere and target-proximity checks before it is rendered (skipped draws are logged once with details). Static draws now derive the position vertex format by validating decoded positions against the engine's own model bound for each mesh (full-precision float32 positions are no longer misread as half precision), and meshes whose layout cannot be validated are skipped instead of being drawn as screen-covering garbage.
- Fix the experimental outline mask renderer's skinned path. Skinned shapes were rejected before their per-partition path was reached; the path then required an engine model bound and a bone-matrix count that engine-managed skinned meshes do not provide; layout selection was cached per vertex descriptor so meshes sharing one were drawn on another mesh's evidence; and the bone palette was built in partition-local index space while the vertex bone indices are indices into the skin's bone array, so most vertices sampled the wrong matrix and collapsed. Skinned shapes now reach their own path, are no longer gated on those values, calibrate their position format from the descriptor's attribute offsets and their bone-weight/index layout and stride from each mesh's own vertex data, validate every mesh individually, and build the bone palette in the skin's global bone index space (padded to the full upload size with a valid matrix). Non-skinned geometry attached to a corpse that has skinned meshes (such as icicles) is no longer included in the silhouette. Layouts that cannot be validated are skipped and every skip reason is logged.
