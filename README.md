# wxl-loot-beam

A pillar of light over every corpse that can still be looted.

Hunting for the body you just killed means squinting at a pile of grey models and reading nameplates.
**Loot Beam** puts a warm glow on the ground and a beam of light rising straight up from every NPC body
that can still be looted, so the one worth walking to announces itself. The beam is capped at 15 yards
by default -- tall enough to spot over a hill or through a crowd, short enough to stay a marker rather
than a light show.

- a filled, ground-hugging disc plus a bright ring marks the spot on the terrain;
- a soft, tapering beam rises from it, fading out toward the top;
- the whole thing breathes slowly so a live beacon never reads as scenery;
- colour, size, height, opacity, range and the pulse are all tunable from the in-game overlay panel
  under **Loot Beam**.

## How it works

Every frame the module walks the resident unit objects through the SDK's object enumerator, drops any
that are the player or still alive, and keeps the rest as beacon positions (optionally filtered to the
ones the server still flags `UNIT_DYNFLAG_LOOTABLE`). Positions are turned into world-space shapes
through the core's `wxl::game::gfx` toolbox and handed to the draw on `OnWorldSceneEnd`, which is the
one point in the frame where geometry placed by world coordinate lands where its coordinates say -- the
scene's own view and projection are still on the device and its depth buffer is complete. Nothing is
retained: a body that stops being lootable loses its beacon with no cleanup.

The ground disc follows the terrain (one collision query per vertex), so it lies flat on a slope; the
beam is billboarded toward the camera and built from two crossed planes, so it reads as a volume from
any angle without meshing a cylinder.

## Tuning

The look lives in `wxl-loot-beam.ini` next to the DLL. The file is read live: save a change and the
module picks it up within about a second, no restart. The panel's **Save** button writes the current
slider values back to the file; **Revert** discards unsaved edits. If the file is missing it is written
with the defaults on first load.

| Key | Meaning |
|---|---|
| `Height` | how far the beam rises, yards (default 15) |
| `GroundRadius` | radius of the glow on the ground |
| `BeamWidth` | half-width of the beam at its base |
| `GroundAlpha`, `RingAlpha`, `BeamAlpha` | opacity of the disc, ring and beam |
| `Color` | tint, as `#RRGGBB` |
| `Pulse`, `PulseSpeed` | breathing depth and rate |
| `FadeIn`, `FadeOut` | seconds to ease a beacon in on appear / out on loot (0 = instant) |
| `MaxDistance` | ignore corpses beyond this range (0 = unlimited) |
| `ShowGround`, `ShowBeam` | keep just one half of the marker |
| `ThroughWalls` | draw through terrain so the marker is always findable |
| `WidthPerYard` | minimum beam half-width per yard of camera distance |
| `RequireLootable` | only beam corpses the server still flags lootable (default on) |

## Notes

- A looted body loses its beam. The default (`RequireLootable=1`) marks only corpses the server still
  flags `UNIT_DYNFLAG_LOOTABLE`, so once you loot one the flag clears and the beacon goes away. Set it
  to 0 to mark every dead NPC instead. If the flags field cannot be trusted on a given client build it
  falls back to the health-only verdict rather than turning into noise.
- Player corpses are left alone -- this marks NPC bodies.
- Purely visual and client-side: the server never learns the beams exist.
