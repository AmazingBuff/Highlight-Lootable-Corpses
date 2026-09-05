# Highlight Lootable Corpses

A corpse-detection ESP overlay for **Skyrim Special Edition / Anniversary Edition**. It draws a glowing, see-through outline around every **lootable corpse** near you — so you never lose sight of a kill behind tall grass, bushes, rocks, or terrain again.

Works on dead NPCs and creatures, ash piles left behind by reanimated enemies, and static corpse containers (draugr corpses, burnt corpses, wrapped corpses, and more). Corpses disappear from the overlay as soon as they are fully looted — no more chasing boxes that have nothing left to take.

## Features

- **Lootable-only marking** — only corpses that still contain items are outlined; emptied corpses disappear within one scan cycle (default 500 ms)
- **See through everything** — outlines are drawn after the scene renders and ignore depth, so grass, bushes, walls, and hills never hide a corpse
- **Ash pile support** — ash piles from reanimated / disintegrated enemies (including DLC variants such as Soul Embers and Ash Spawn) are checked through the original actor they point to
- **Static corpse support** — container-type corpses such as `TreasDraugrAmbushCorpse*`, `TreasBurntCorpse*`, `defaultGhostCorpse`, including DLC variants
- **Optional loot filter** — show only corpses whose inventory contains quest items, keys, enchanted gear, high-value items, books, consumables (arrows, potions, scrolls, ingredients, soul gems — filled-only option available)
- **Distance fade** — outlines are fully opaque up close and fade smoothly with distance
- **Accurate boxes** — outlines come from Havok collision shapes and ragdoll bodies, matching the corpse's real footprint (12-edge 3D wireframe when an oriented collision box is available)
- **Hotkey toggle** — turn the overlay on/off with a single key (F7 by default; toggles print `HighlightLootableCorpses: ON/OFF` to the console)
- **In-game settings menu** — every option can be adjusted live in the Mod Control Panel ("Highlight Lootable Corpses > Settings") and saved to the INI
- **Lightweight** — bounding-box wireframe rendering only, with a throttled scan loop; negligible frame-time impact

## Requirements

- [Skyrim Special Edition / AE](https://store.steampowered.com/app/489830/) **1.6.629 or newer** (1.6.1170 and 1.7.99+ are supported). Not compatible with SE 1.5.97 or Skyrim VR.
- [SKSE64](https://skse.silverlock.org/) matching your game version
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) (All in one)
- Optional (recommended): [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352) — enables the in-game settings panel. Without it, the mod works normally and is configured through the INI file.

## Installation

1. Install SKSE64 and Address Library (links above).
2. Install this mod with your mod manager (MO2 / Vortex), or manually copy `HighlightLootableCorpses.dll` into `Data\SKSE\Plugins\`.
3. Launch the game. A default configuration file is created automatically at `Data\SKSE\Plugins\HighlightLootableCorpses.ini` on first run.

> Log file (for troubleshooting): `Documents\My Games\Skyrim Special Edition\SKSE\HighlightLootableCorpses.log`

**Upgrading from the old `CorpseESP` builds:** the DLL and INI were renamed. Delete the old `CorpseESP.dll` / `CorpseESP.ini` and rename your INI to `HighlightLootableCorpses.ini` to keep your settings.

## Usage

- Press **F7** (default) in game to toggle the overlay.
- Walk around: every lootable corpse within **Max Distance** (default 2000 units, ≈ 17 m) gets an outline.
- Take everything from a corpse and its outline vanishes by the next scan — including ash piles and static corpse containers.
- Open **Mod Control Panel → Highlight Lootable Corpses → Settings** to tune everything live (color, distance, fade, loot filter, scan interval). Use *Save to INI* to persist changes.

## Configuration reference

All options live in `Data\SKSE\Plugins\HighlightLootableCorpses.ini` (auto-generated, values below are the defaults). Every option is also editable in the in-game menu.

```ini
[General]
Enabled=true                ; mod enabled on startup
Hotkey=118                  ; toggle key virtual-key code (118 = F7, 0 = disabled)
MaxDistance=2000.0          ; search radius in game units (~17 m default)
ScanIntervalMs=500          ; corpse scan interval in milliseconds

[Display]
OutlineColor=00FF66         ; outline color (RGB hex)
MinOpacity=0.15             ; minimum opacity at max distance
OutlineThickness=2.0        ; outline thickness in pixels
FadeStartDistance=1000.0    ; distance where fading begins (fully opaque below)
FadePower=2.0               ; fade curve exponent (higher = faster fade)
ShowOutline=true            ; draw outlines at all

[LootFilter]
LootFilterEnabled=false     ; only outline corpses matching the categories below
ValueQuestItems=true        ; quest items
ValueKeys=true              ; keys
ValueEnchanted=true         ; enchanted equipment
ValueHighValue=true         ; items worth >= HighValueThreshold gold
HighValueThreshold=100.0    ; high-value threshold (gold piles count by amount)
ValueBooks=true             ; books
BookFilterMode=1            ; 0 = all books, 1 = spell & skill books, 2 = spell books only
ValueConsumables=true       ; arrows, ingredients, potions, scrolls, soul gems
SoulGemFilledOnly=true      ; soul gems count only when filled
```

## Compatibility

- The loot check reads the same merged inventory the container UI uses (base container + runtime changes), so a corpse is outlined exactly when something is still takeable. Script-added items and player-dropped items in a corpse count as loot, same as vanilla.
- No gameplay records are modified; the overlay is pure rendering. Safe to add/remove on an existing save.
- Should be compatible with any loot/ESP mod; load order does not matter.

## Troubleshooting

- **Nothing shows up** — check that corpses are actually within *Max Distance* and still contain loot; press the hotkey to confirm the overlay is ON (console prints `HighlightLootableCorpses: ON`); verify Address Library is installed.
- **No in-game menu** — SKSE Menu Framework is not installed; the rest of the mod still works, configure via the INI.
- **Weird colors on HDR displays** — the game's HDR back buffer has only 2-bit alpha, so half-transparent outlines are quantized; lower *Min Opacity* for a subtler look.
- Check the log at `Documents\My Games\Skyrim Special Edition\SKSE\HighlightLootableCorpses.log` — it lists every detection decision and any setup problems.

## Credits

- [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG) by alandtse & contributors
- [SKSE](https://skse.silverlock.org/) by the SKSE team
- [SKSE Menu Framework](https://github.com/Adhy-S/SKSE-Menu-Framework) & [SKSE-MCP](https://github.com/QTR-Modding/SKSE-MCP)
- [DirectXTK](https://github.com/microsoft/DirectXTK), [SimpleIni](https://github.com/brofield/simpleini), [spdlog](https://github.com/gabime/spdlog), [fmt](https://github.com/fmtlib/fmt)
