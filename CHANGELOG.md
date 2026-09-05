# Changelog

## [Unreleased]

### Changed

- **BREAKING:** Rename the plugin to `HighlightLootableCorpses`. The DLL, INI, and log files are renamed. Migration: rename your `CorpseESP.ini` to `HighlightLootableCorpses.ini` to keep your settings, and replace the old DLL.

### Removed

- Remove the model-silhouette outline mode (`OutlineMode`) to eliminate its heavy performance cost.

### Fixed

- Fix box/loot state mismatches on leveled-list corpses (e.g. draugr weapons inherited via NPC templates): lootability is now judged from the same inventory view the loot menu uses — engine-initialized inventory changes merged with base-container entries and dropped-item lists, skipping unresolved leveled-list placeholder entries (QuickLoot IE approach).
- Fix boxes persisting on looted-empty corpses by ignoring resolved leveled-list placeholder entries in the searchable check.
- Stop drawing boxes on corpses whose loot has been fully looted by ignoring non-playable items in the searchable check.
- Prevent per-scan corpse diagnostics from flooding the log with unchanged entries.
