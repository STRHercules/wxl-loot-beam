// wxl-loot-beam: the additive triangle queue behind the beacon.
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

#include "BeaconRenderer.hpp"

#include "game/Camera.hpp"

#include <vector>

namespace wxl::scripts::loot_beam::beacon_gfx
{
    namespace gfx = wxl::game::gfx;
    namespace gx  = wxl::game::gx;
    namespace cam = wxl::game::camera;

    namespace
    {
        struct Vertex { float x, y, z; gfx::Color color; };
        static_assert(sizeof(Vertex) == 16, "Vertex must match the declared vertex format stride");

        std::vector<Vertex> g_vertices;
        gfx::Depth          g_depth = gfx::Depth::Tested;

        // D3DBLEND_ONE. The SDK names only the two source-over factors it needs; additive is the whole
        // point here, so the constant is carried locally rather than widening the core's enum.
        constexpr unsigned kBlendOne = 2;
        // D3DCMP_GREATEREQUAL. Absent from the gx constants, which only ever needed the standard one.
        constexpr unsigned kGreaterEqual = 7;

        constexpr unsigned kTouchedStates[] = {
            gx::rs::kZEnable, gx::rs::kShadeMode, gx::rs::kZWrite, gx::rs::kAlphaTest,
            gx::rs::kSrcBlend, gx::rs::kDestBlend, gx::rs::kCullMode, gx::rs::kZFunc,
            gx::rs::kAlphaBlend, gx::rs::kFogEnable, gx::rs::kStencilEnable,
            gx::rs::kLighting, gx::rs::kColorWrite, gx::rs::kScissorTest,
        };
        constexpr size_t kTouchedStateCount = sizeof(kTouchedStates) / sizeof(kTouchedStates[0]);

        constexpr unsigned kTouchedStages[][2] = {
            { 0, gx::tss::kColorOp },  { 0, gx::tss::kColorArg1 },
            { 0, gx::tss::kAlphaOp },  { 0, gx::tss::kAlphaArg1 },
            { 1, gx::tss::kColorOp },
        };
        constexpr size_t kTouchedStageCount = sizeof(kTouchedStages) / sizeof(kTouchedStages[0]);
    }

    void Clear() { g_vertices.clear(); }
    size_t Pending() { return g_vertices.size() / 3; }
    void SetDepth(gfx::Depth depth) { g_depth = depth; }

    void Triangle(const float a[3], const float b[3], const float c[3],
                  gfx::Color ca, gfx::Color cb, gfx::Color cc)
    {
        g_vertices.push_back(Vertex{ a[0], a[1], a[2], ca });
        g_vertices.push_back(Vertex{ b[0], b[1], b[2], cb });
        g_vertices.push_back(Vertex{ c[0], c[1], c[2], cc });
    }

    long Draw(gx::Device9 dev, void* sceneDepth)
    {
        // Emptied on every path, including a graphics-down one, so a module that queues while the
        // device is away never has its shapes appear all at once when it comes back.
        struct Emptied { ~Emptied() { g_vertices.clear(); } } emptied;

        if (!dev || g_vertices.empty()) return 0;

        float view[16], projection[16];
        if (!gfx::SceneMatrices(view, projection)) return 0;

        void* oldVS = nullptr; dev.GetVertexShader(&oldVS);
        void* oldPS = nullptr; dev.GetPixelShader(&oldPS);
        void* oldTex = nullptr; dev.GetTexture(0, &oldTex);

        void* oldDepth = nullptr;
        if (sceneDepth)
        {
            dev.GetDepthStencil(&oldDepth);
            dev.SetDepthStencil(sceneDepth);
        }

        float oldWorld[16], oldView[16], oldProjection[16];
        dev.GetTransform(gx::ts::kWorld, oldWorld);
        dev.GetTransform(gx::ts::kView, oldView);
        dev.GetTransform(gx::ts::kProjection, oldProjection);

        unsigned oldStates[kTouchedStateCount];
        for (size_t i = 0; i < kTouchedStateCount; ++i)
            oldStates[i] = dev.GetRenderState(kTouchedStates[i]);

        unsigned oldStages[kTouchedStageCount];
        for (size_t i = 0; i < kTouchedStageCount; ++i)
            oldStages[i] = dev.GetTextureStageState(kTouchedStages[i][0], kTouchedStages[i][1]);

        // Untextured, unlit, vertex-coloured geometry through the fixed-function pipeline. Every one of
        // these is inherited from whatever drew last, and any one left wrong rejects the draw outright
        // or repaints it in a colour that is not the one asked for.
        dev.SetVertexShader(nullptr);
        dev.SetPixelShader(nullptr);
        dev.SetTexture(0, nullptr);
        dev.SetFVF(gx::fvf::kXyz | gx::fvf::kDiffuse);

        dev.SetTextureStageState(0, gx::tss::kColorOp,   gx::top::kSelectArg1);
        dev.SetTextureStageState(0, gx::tss::kColorArg1, gx::ta::kDiffuse);
        dev.SetTextureStageState(0, gx::tss::kAlphaOp,   gx::top::kSelectArg1);
        dev.SetTextureStageState(0, gx::tss::kAlphaArg1, gx::ta::kDiffuse);
        dev.SetTextureStageState(1, gx::tss::kColorOp,   gx::top::kDisable);

        // The scene's view matrix carries no translation: the world is drawn about the camera, so a
        // world-space vertex has to be moved to that origin or it projects thousands of units away.
        float eye[3];
        cam::GetPosition(eye);

        const float toCameraOrigin[16] = {
            1.0f,    0.0f,    0.0f,    0.0f,
            0.0f,    1.0f,    0.0f,    0.0f,
            0.0f,    0.0f,    1.0f,    0.0f,
            -eye[0], -eye[1], -eye[2], 1.0f,
        };

        dev.SetTransform(gx::ts::kWorld, toCameraOrigin);
        dev.SetTransform(gx::ts::kView, view);
        dev.SetTransform(gx::ts::kProjection, projection);

        dev.SetRenderState(gx::rs::kLighting, 0);
        dev.SetRenderState(gx::rs::kFogEnable, 0); // world fog would tint the beacon with distance
        dev.SetRenderState(gx::rs::kCullMode, gx::cull::kNone);
        dev.SetRenderState(gx::rs::kAlphaTest, 0);
        dev.SetRenderState(gx::rs::kStencilEnable, 0);
        dev.SetRenderState(gx::rs::kScissorTest, 0);
        dev.SetRenderState(gx::rs::kColorWrite, gx::colorwrite::kAll);
        dev.SetRenderState(gx::rs::kShadeMode, gx::shade::kGouraud);

        // Light adds to the frame rather than covering it: source colour scaled by its own alpha,
        // added straight onto what is behind. That both keeps the shaft from looking like a pane of
        // coloured glass and makes overlapping soft geometry sum into a glow.
        dev.SetRenderState(gx::rs::kAlphaBlend, 1);
        dev.SetRenderState(gx::rs::kSrcBlend, gx::blend::kSrcAlpha);
        dev.SetRenderState(gx::rs::kDestBlend, kBlendOne);
        dev.SetRenderState(gx::rs::kZWrite, 0); // a marker is not part of the world

        if (g_depth == gfx::Depth::Through)
        {
            dev.SetRenderState(gx::rs::kZEnable, 0);
        }
        else
        {
            // Which end of the depth range is near is a property of the client's projection, not a
            // constant: standard depth maps near to 0 and wants LessEqual, a reversed-Z projection maps
            // near to 1 and wants GreaterEqual. d(ndcZ)/d(viewZ) is -proj[14]*proj[11], so its sign
            // says which one this is -- and using the wrong one inverts the whole test, hiding the
            // beacon where it should show and showing it where something should hide it.
            const bool reversed = (-projection[14] * projection[11]) < 0.0f;

            // A plain depth test, with no D3D9 depth bias. DEPTHBIAS is not measured in depth-buffer
            // LSbs: on DXVK (this client's d3d9 shim) a value of 1.0 scales to ~2^23 and offsets the
            // fragment by essentially the whole depth range, which pulled the beacon in front of every
            // wall. The scene's own depth is what hides it; leave it unmodified.
            dev.SetRenderState(gx::rs::kZEnable, 1);
            dev.SetRenderState(gx::rs::kZFunc, reversed ? kGreaterEqual : gx::cmp::kLessEqual);
        }

        const long result = dev.DrawPrimitiveUP(gx::prim::kTriangleList,
                                                unsigned(g_vertices.size() / 3),
                                                g_vertices.data(), sizeof(Vertex));

        for (size_t i = 0; i < kTouchedStageCount; ++i)
            dev.SetTextureStageState(kTouchedStages[i][0], kTouchedStages[i][1], oldStages[i]);
        for (size_t i = 0; i < kTouchedStateCount; ++i)
            dev.SetRenderState(kTouchedStates[i], oldStates[i]);

        dev.SetTransform(gx::ts::kWorld, oldWorld);
        dev.SetTransform(gx::ts::kView, oldView);
        dev.SetTransform(gx::ts::kProjection, oldProjection);

        if (sceneDepth)
        {
            dev.SetDepthStencil(oldDepth);
            gx::Release(oldDepth);
        }

        dev.SetTexture(0, oldTex);
        dev.SetPixelShader(oldPS);
        dev.SetVertexShader(oldVS);

        gx::Release(oldTex);
        gx::Release(oldPS);
        gx::Release(oldVS);

        return result;
    }
}
