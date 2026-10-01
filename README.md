# wxl-loot-beam

A pillar of light over every corpse that can still be looted.

Hunting for the body you just killed means squinting at a pile of grey models and reading nameplates.
**Loot Beam** puts a warm glow on the ground and a beam of light rising straight up from every NPC body
that can still be looted, so the one worth walking to announces itself. The beam is capped at 15 yards
by default -- tall enough to spot across a camp or clear a low rise, short enough to stay a marker
rather than a light show. It is occluded by the world like any other geometry, unless **Through walls**
is turned on.

- a soft pool of light on the terrain, falling off smoothly from a hot centre rather than a flat circle;
- a camera-facing shaft rises a little above the body and fades in from transparent there, peaking a
  short way up and easing to nothing at the top, with a horizontal falloff that keeps its edges soft
  and its core whiter;
- the whole thing breathes slowly and eases in and out, so a live beacon never reads as scenery;
- colour, size, height, opacity, range, pulse and fade are all tunable from the in-game overlay panel
  under **Loot Beam**;
- with **Colour by loot rarity** on (the default), a corpse whose loot is known glows in the quality
  colour of its best item -- green for a green, purple for an epic -- instead of the default gold.

## Loot rarity colour

The beacon is tinted with the best item quality in the corpse's loot, using the game's own quality
colours (poor grey, common white, uncommon green, rare blue, epic purple, legendary orange, artifact
gold). A body whose loot is not known keeps the configured `Color`.

The 3.3.5a client only receives a corpse's loot when loot is requested for it -- normally the loot
window opening -- so on a stock server the quality colour appears once you have opened that body. The
module reads the loot the client already holds (via the client's own `GetNumLootItems` /
`GetLootSlotInfo` script functions) and caches the best quality against the corpse's GUID; a server
that sends loot ahead of the window will colour the beam before you open it. Turn **Colour by loot
rarity** off (or set `LootColor=0`) to use a single fixed tint.

## How it works

Every frame the module walks the resident unit objects through the SDK's object enumerator, drops any
that are the player or still alive, and keeps the rest as beacon positions (optionally filtered to the
ones the server still flags `UNIT_DYNFLAG_LOOTABLE`). Positions are turned into world-space triangles
and drawn on `OnWorldSceneEnd`, which is the one point in the frame where geometry placed by world
coordinate lands where its coordinates say -- the scene's own view and projection are still on the
device and its depth buffer is complete. Nothing is retained: a body that stops being lootable loses
its beacon with no cleanup.

The draw is additive (source alpha added to the frame, so the beacon glows and never dims what is
behind it) with a colour on every vertex, so the GPU interpolates the falloffs and a handful of quads
read as a soft volume. The ground pool follows the terrain vertex by vertex and falls off smoothly
from its hot middle outward; the shaft is one camera-facing billboard, gridded across its width and up
its height so its horizontal and vertical falloffs read as light rather than a slab.

## Tuning

The look lives in `wxl-loot-beam.ini` next to the DLL. The file is read live: save a change and the
module picks it up within about a second, no restart. The panel's **Save** button writes the current
slider values back to the file; **Revert** discards unsaved edits. If the file is missing it is written
with the defaults on first load.

| Key | Meaning |
|---|---|
| `Height` | how far the beam rises, yards (default 15) |
| `BaseOffset` | how far above the body the shaft begins, yards (default 1) |
| `GroundRadius` | radius of the glow on the ground |
| `BeamWidth` | half-width of the beam at its base |
| `GroundAlpha`, `BeamAlpha` | opacity of the ground glow and the beam |
| `Color` | tint, as `#RRGGBB` |
| `Pulse`, `PulseSpeed` | breathing depth and rate |
| `FadeIn`, `FadeOut` | seconds to ease a beacon in on appear / out on loot (0 = instant) |
| `MaxDistance` | ignore corpses beyond this range (0 = unlimited) |
| `ShowGround`, `ShowBeam` | keep just one half of the marker |
| `ThroughWalls` | draw through terrain and walls (off by default, so the world occludes the beam) |
| `WidthPerYard` | minimum beam half-width per yard of camera distance |
| `RequireLootable` | only beam corpses the server still flags lootable (default on) |
| `LootColor` | tint a known corpse by its best loot quality (default on) instead of `Color` |

## Notes

- A looted body loses its beam. The default (`RequireLootable=1`) marks only corpses the server still
  flags `UNIT_DYNFLAG_LOOTABLE`, so once you loot one the flag clears and the beacon goes away. Set it
  to 0 to mark every dead NPC instead. If the flags field cannot be trusted on a given client build it
  falls back to the health-only verdict rather than turning into noise.
- Loot quality is only known for a body the client has been sent loot for (see above). The module does
  not request loot itself: it never talks to the server and never opens the loot window.
- Player corpses are left alone -- this marks NPC bodies.
- Purely visual and client-side: the server never learns the beams exist.
