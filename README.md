# wxl-loot-beam

## EXPERIMENTAL AND TOTALLY UNSTABLE


A pillar of light over every corpse that can still be looted.

Hunting for the body you just killed means squinting at a pile of grey models and reading nameplates.
**Loot Beam** puts a warm glow on the ground and a beam of light rising straight up from every NPC body
that can still be looted, so the one worth walking to announces itself. The beam is capped at 15 yards
by default -- tall enough to spot across a camp or clear a low rise, short enough to stay a marker
rather than a light show. It draws through terrain and walls by default, so a body tucked behind a rise
is never missed; turn **Through walls** off to let the world occlude it like any other geometry.

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
`GetLootSlotInfo` script functions) and caches the best quality against the corpse's GUID.

### Server-driven colour

To colour a beam the moment the body dies -- before anyone opens it -- run the companion
**mod-loot-beam** module on the AzerothCore server. At creature death the server has just rolled the
corpse's loot, so it computes the best item quality in it and writes that back onto the corpse's own
`UNIT_FIELD_PADDING` update field as `quality + 1` (0 means "no hint"; 9 means the loot held money but
no gear, the `Currency` tier). The client reads that field straight out of the object it is already
walking, so the beacon is the right colour immediately and no custom opcode, addon message or client
patch is involved.

This is opt-in on the client too: with **Prefer server loot colour** on (the default, `ServerColor=1`)
the server's quality wins over the loot the client discovers for itself; with it off the module
ignores the field and behaves exactly as it did before. On a server that does not run the module the
field stays 0 and the client falls back to its own loot, so nothing needs changing.

Turn **Colour by loot rarity** off (or set `LootColor=0`) to use a single fixed tint; that disables
the server colour as well.

### Gear tiers

Every beam belongs to a **tier**, and each tier has its own colour and an on/off switch, both live in
the panel's **Gear tiers** section (and the `Tier.*` keys in the INI):

| Tier | Meaning |
|---|---|
| `Currency` | the corpse's loot held money but no gear (server-flagged; the client cannot tell on its own) |
| `Poor` .. `Heirloom` | the best item quality in the loot: 0 grey, 1 white, 2 green, 3 blue, 4 purple, 5 orange, 6 artifact, 7 heirloom |

A tier switched off produces **no beacon at all** for corpses whose best loot falls into it, so you
can hide e.g. currency-only or uncommon bodies and keep the rest. The defaults are the game's own
quality colours; a body whose loot (and so tier) is not known still uses `Color`.

## Server module

The companion AzerothCore module lives in this repo under `server/mod-loot-beam/`. Copy that whole
folder into your server's `modules/` directory so it becomes `<azerothcore>/modules/mod-loot-beam/`,
then rebuild `worldserver`; the module loader picks it up automatically (no core edits needed). It
adds one setting, `LootBeam.Enable` (default 1), to `mod_loot_beam.conf`.

The server half is optional. Without it the module still colours a corpse from the loot the client
learns when the window opens; with it the colour -- including the money-only `Currency` tier -- is
known the instant the body dies. The server never reads anything from the client: it only writes the
corpse's own best-loot quality back onto that corpse.

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
| `ThroughWalls` | draw through terrain and walls (on by default, so a rise cannot hide the beacon) |
| `WidthPerYard` | minimum beam half-width per yard of camera distance |
| `RequireLootable` | only beam corpses the server still flags lootable (default on) |
| `LootColor` | tint a known corpse by its best loot quality (default on) instead of `Color` |
| `ServerColor` | prefer the quality the server puts on the corpse (default on) over the locally read loot |
| `Tier.<name>.Enabled`, `Tier.<name>.Color` | per-tier on/off and colour; `<name>` is `Currency`, `Poor`, `Common`, `Uncommon`, `Rare`, `Epic`, `Legendary`, `Artifact`, `Heirloom` |

## Notes

- A looted body loses its beam. The default (`RequireLootable=1`) marks only corpses the server still
  flags `UNIT_DYNFLAG_LOOTABLE`, so once you loot one the flag clears and the beacon goes away. Set it
  to 0 to mark every dead NPC instead. If the flags field cannot be trusted on a given client build it
  falls back to the health-only verdict rather than turning into noise.
- Loot quality is only known for a body the client has been sent loot for, unless the server runs
  the companion **mod-loot-beam** module (see **Server-driven colour** above). The module does not
  request loot itself: it never asks the server for anything and never opens the loot window.
- Player corpses are left alone -- this marks NPC bodies.
- Purely visual: the module sends nothing to the server, and on a stock server the server never
  learns the beams exist. When the companion server module is present it only writes the corpse's own
  best-loot-quality back onto that corpse; it does not read anything from the client.
