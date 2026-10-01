// wxl-loot-beam: the additive, per-vertex-graduated draw the beacon uses.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include "game/Gfx.hpp"
#include "game/Gx.hpp"

#include <cstddef>
#include <cstdint>

// The beacon does not use wxl::game::gfx's flush: that one blends source-over, which makes a light
// shaft read as a coloured pane and cannot make a gradient cheaper than one colour per triangle. This
// is a small, self-contained triangle queue drawn additively (source alpha added to the frame, never
// dimming what is behind it) with a colour on every vertex, so the GPU interpolates the falloffs and a
// handful of quads read as a soft volume. It reads the same scene matrices the world was drawn with,
// so it lands where its world coordinates say.
namespace wxl::scripts::loot_beam::beacon_gfx
{
    /// Empties the queue without drawing it.
    void Clear();

    /// How many triangles are queued.
    size_t Pending();

    /// The depth mode every queued shape is drawn with. Set once per frame before queueing.
    void SetDepth(wxl::game::gfx::Depth depth);

    ///
    /// The depth bias applied while testing against the scene, in depth-buffer units.
    ///
    /// A beacon placed on a ground height sampled from the collision mesh sits at a slightly different
    /// depth than the terrain the client actually renders at range (its LOD), so an unbiased test can
    /// lose at a distance. D3D's slope-scaled + constant depth bias pulls it toward the camera in depth
    /// only -- no vertex moves, so the beacon never leaves its world position or its screen pixel. 0
    /// disables it.
    ///
    void SetDepthBias(float bias);

    /// Queues a triangle with an independent colour at each vertex (Gouraud-interpolated).
    void Triangle(const float a[3], const float b[3], const float c[3],
                  wxl::game::gfx::Color ca, wxl::game::gfx::Color cb, wxl::game::gfx::Color cc);

    /**
     * @brief Draws everything queued and empties it, leaving the device as it was found.
     * @param dev         The live device.
     * @param sceneDepth  The world's depth surface, from OnWorldSceneEnd, or null.
     * @return The device's result for the draw, or 0 when nothing was queued.
     */
    long Draw(wxl::game::gx::Device9 dev, void* sceneDepth);
}
