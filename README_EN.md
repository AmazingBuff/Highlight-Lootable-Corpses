# Highlight Lootable Corpses

A corpse-detection overlay for **Skyrim Special Edition / Anniversary Edition**. It draws a glowing, see-through outline around every **lootable corpse** near you — so you never lose sight of a kill behind tall grass, bushes, rocks, or terrain again.

Works on dead NPCs and creatures, ash piles left behind by reanimated enemies, and static corpse containers (draugr corpses, burnt corpses, wrapped corpses, and more). Corpses disappear from the overlay as soon as they are fully looted — no more chasing boxes that have nothing left to take.

## Features

- **Lootable-only marking** — only corpses that still contain items are outlined; emptied corpses disappear within one scan cycle (default 500 ms)
- **See through everything** — outlines are drawn after the scene renders and ignore depth, so grass, bushes, walls, and hills never hide a corpse
- **Ash pile support** — ash piles from reanimated / disintegrated enemies (including DLC variants such as Soul Embers and Ash Spawn) are checked through the original actor they point to
- **Static corpse support** — container-type corpses such as `TreasDraugrAmbushCorpse*`, `TreasBurntCorpse*`, `defaultGhostCorpse`, including DLC variants
- **Optional loot filter** — show only corpses whose inventory contains quest items, keys, enchanted gear, high-value items, books, consumables (arrows, potions, scrolls, ingredients, soul gems — filled-only option available)
- **Distance fade** — the `icon` marker is fully opaque up close and fades smoothly with distance
- **Three display modes** — pick one via `DisplayMode`: `silhouette` fills the wall-penetrating mesh silhouette, `outline` draws a band just outside it, and `icon` shows a small circular marker at the corpse's screen position
- **Accurate placement** — marker positions come from Havok collision shapes and ragdoll bodies, matching the corpse's real footprint
- **Hotkey toggle** — optionally turn the overlay on/off with a single key (unbound by default; bind one via the "Hotkey" button in the MCP menu or the `Hotkey` INI key; toggles print `HighlightLootableCorpses: ON/OFF` to the console)
- **In-game settings menu** — every option can be adjusted live in the Mod Control Panel ("Highlight Lootable Corpses > Settings") and saved to the INI
- **Lightweight** — simple marker rendering only, with a throttled scan loop; negligible frame-time impact

## Requirements

- [Skyrim Special Edition / AE](https://store.steampowered.com/app/489830/) **1.6.629 or newer** (1.6.1170 and 1.7.99+ are supported). SE 1.5.97 and Skyrim VR are supported theoretically.
- [SKSE64](https://skse.silverlock.org/) matching your game version
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) (All in one)
- Optional (recommended): [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352) — enables the in-game settings panel. Without it, the mod works normally and is configured through the INI file.

## Installation

1. Install SKSE64 and Address Library (links above).
2. Install this mod with your mod manager (MO2 / Vortex), or manually copy `HighlightLootableCorpses.dll` into `Data\SKSE\Plugins\`.
3. Launch the game. A default configuration file is created automatically at `Data\SKSE\Plugins\HighlightLootableCorpses.ini` on first run.

> Log file (for troubleshooting): `Documents\My Games\Skyrim Special Edition\SKSE\HighlightLootableCorpses.log`

## Usage

- Press **F7** (default) in game to toggle the overlay.
- Walk around: every lootable corpse within **Max Distance** (default 2000 units, ≈ 17 m) gets an outline.
- Take everything from a corpse and its outline vanishes by the next scan — including ash piles and static corpse containers.
- Optionally hide corpses you have searched at least once — even if you took nothing, they stay hidden across sessions: the marks persist per save game in the SKSE co-save and are cleaned up automatically (toggled via `HideSearchedEnabled` or the "Hide Searched Corpses" menu checkbox). Works with vanilla activation and [QuickLoot IE](https://www.nexusmods.com/skyrimspecialedition/mods/120075) (optional — opening its loot menu counts as searching that corpse).
- Open **Mod Control Panel → Highlight Lootable Corpses → Settings** to tune everything live (color, distance, fade, loot filter, scan interval). Use *Save to INI* to persist changes.

## Configuration reference

All options live in `Data\SKSE\Plugins\HighlightLootableCorpses.ini` (auto-generated, values below are the defaults). Every option is also editable in the in-game menu.

```ini
[General]
; mod enabled on startup
Enabled=true
; toggle key virtual-key code (0 = disabled, rebindable in the MCP menu)
Hotkey=0
; search radius in game units (~17 m default)
MaxDistance=2000.0
; corpse scan interval in milliseconds
ScanIntervalMs=500

[Display]
; corpse display style: silhouette (filled mask) | outline (band around the mask) | icon (small circle at the corpse position)
DisplayMode=outline
; outline color (RGB hex)
OutlineColor=00FF66
; minimum opacity at max distance
MinOpacity=0.15
; outline thickness in pixels
OutlineThickness=2.0
; distance where fading begins (fully opaque below)
FadeStartDistance=500.0
; fade curve exponent (higher = faster fade)
FadePower=2.0

[LootFilter]
; stop outlining corpses the player has searched (activated) at least once, even if nothing was taken
HideSearchedEnabled=false
; only outline corpses matching the categories below
ValueFilterEnabled=false
; quest items
ValueQuestItems=false
; keys
ValueKeys=false
; enchanted equipment
ValueEnchanted=false
; single item worth >= HighValueThreshold gold
ValueHighValue=false
; high-value threshold (gold piles count by amount)
HighValueThreshold=100.0
; books
ValueBooks=false
; 0 = all books, 1 = spell & skill books, 2 = spell books only
BookFilterMode=0
; arrows, ingredients, potions, scrolls, soul gems
ValueConsumables=false
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
- [SKSE Menu Framework](https://github.com/QTR-Modding/SKSE-Menu-Framework-3) & [SKSE-MCP](https://github.com/QTR-Modding/SKSE-MCP)