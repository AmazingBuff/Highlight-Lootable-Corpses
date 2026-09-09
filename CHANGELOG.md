# Changelog

## [Unreleased]

### Changed

- **BREAKING:** Rename the plugin to `HighlightLootableCorpses`. The DLL, INI, and log files are renamed. Migration: rename your `CorpseESP.ini` to `HighlightLootableCorpses.ini` to keep your settings, and replace the old DLL.
- The toggle hotkey is now unbound by default. Bind one via the new "Hotkey" button in the MCP menu — all keys are bindable (keyboard and mouse), including ESC/F1 (pressing them may close the panel, but the bind still applies; click the button again or wait 5s to cancel). Bind it via the `Hotkey` INI key alternatively. Hotkey toggles stay in sync with the menu's Enabled checkbox and are suspended while the menu is open.
- The INI is now written with a comment above each option (same style as the configuration reference in the README) and is saved automatically whenever the player makes a save game after changing settings in the MCP menu — pressing "Save to INI" is no longer required (that button still saves immediately).
- Loot filter category switches now default to **false** (opt-in per category), and their controls are greyed out in the MCP menu while the loot filter is off.

### Added

- Add an optional "hide searched corpses" mode (`HideSearchedEnabled`, off by default): once the player searches a corpse, it stops being outlined — even if nothing was taken. Searching is always recorded (so corpses searched before enabling the option are hidden too once it is turned on). QuickLoot IE users are covered via its public API (opening the loot menu marks the corpse as searched); QuickLoot IE is an optional dependency and its absence falls back to vanilla activation events only.
- Searched-corpses marks persist per save game via the SKSE co-save (record `HLCS`): form IDs are re-resolved on load (stale marks are dropped), marks are removed when the engine deletes a form, and loading a save without marks (or removing the plugin) leaves saves fully intact.

### Removed

- Remove the model-silhouette outline mode (`OutlineMode`) to eliminate its heavy performance cost.
- Remove the `SoulGemFilledOnly` loot filter option; soul gems always count as consumables.
- Remove the redundant `ShowOutline` option; the plugin toggle (`Enabled`) already covers it.

### Fixed

- Fix box/loot state mismatches on leveled-list corpses (e.g. draugr weapons inherited via NPC templates): lootability is now judged from the same inventory view the loot menu uses — engine-initialized inventory changes merged with base-container entries and dropped-item lists, skipping unresolved leveled-list placeholder entries (QuickLoot IE approach).
- Fix boxes persisting on looted-empty corpses by ignoring resolved leveled-list placeholder entries in the searchable check.
- Stop drawing boxes on corpses whose loot has been fully looted by ignoring non-playable items in the searchable check.
- Prevent per-scan corpse diagnostics from flooding the log with unchanged entries.
