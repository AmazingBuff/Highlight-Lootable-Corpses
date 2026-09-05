# Changelog

## [Unreleased]

### Removed

- Remove the model-silhouette outline mode (`OutlineMode`) to eliminate its heavy performance cost.

### Fixed

- Fix boxes persisting on looted-empty corpses by ignoring resolved leveled-list placeholder entries in the searchable check.
- Stop drawing boxes on corpses whose loot has been fully looted by ignoring non-playable items in the searchable check.
- Prevent per-scan corpse diagnostics from flooding the log with unchanged entries.
