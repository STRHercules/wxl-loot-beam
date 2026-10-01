Stop hunting for the body you just killed. **Loot Beam** plants a warm glow on the ground and raises a
pillar of light from every NPC corpse that can still be looted, so the one worth walking to announces
itself from across the camp.

The beam is capped at 15 yards by default -- tall enough to spot across a camp, short enough to stay a
marker rather than a light show. It breathes slowly so a live beacon never reads as scenery, and the
glow hugs the terrain so it lies flat on a slope. It also sits in the world rather than over it: trees,
walls and other players hide it, exactly as they hide anything else -- turn on **Through walls** if you
would rather it always show.

**Every knob is live-tunable** from the in-game overlay panel or from `wxl-loot-beam.ini` next to the
DLL: height, base offset, width, ground radius, colour, opacity, pulse, range, and whether the world is
allowed to hide it. Want only the ground halo, or only the beam? Turn the other one off. Prefer to
leave already-looted bodies dark? Enable **Only lootable corpses** and the beacon disappears the moment
the body is emptied.

Purely visual and entirely client-side -- the server never learns the beams exist, and nothing is
retained once a corpse stops counting.
