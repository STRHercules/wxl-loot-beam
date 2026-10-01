// wxl-loot-beam: a light on the ground that rises a fixed distance into the sky over every NPC body
// that can still be looted.
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

#include "wxl/PluginApi.h"
#include "wxl/EventScript.hpp"

#include <string>

// World-space beacon over lootable corpses. It never touches a raw client address for anything the SDK
// models: the object walk, the unit position and the drawing all go through wxl::game, and the only
// two layout numbers the module carries -- the descriptor pointer and the health field -- live in
// UnitFields.hpp.
//
// The beacon is queued the same frame it is decided, on the logic tick, and handed to the draw at
// OnWorldSceneEnd, which is the one slot where geometry placed by world coordinate lands where its
// coordinates say (the scene's matrices are still on the device and its depth buffer is complete).
// Nothing is retained: an enemy that stops being dead stops having a beam with no cleanup.
namespace wxl::scripts::loot_beam
{
    /** @brief User-tunable look, read from wxl-loot-beam.ini beside the DLL. */
    struct BeamStyle
    {
        bool  enabled        = true;   // master switch
        float height         = 15.0f;  // how far the beam rises, yards (the "15 metres" of the brief)
        float groundRadius   = 1.70f;  // radius of the glow painted on the ground, yards
        float beamWidth      = 0.70f;  // half-width of the beam at its base, yards
        float widthPerYard   = 0.010f; // minimum half-width per yard of camera distance (0 = off); a
                                       // beam that never thins with distance stays a legible column
        float color[3]       = { 1.00f, 0.82f, 0.42f }; // warm gold

        float groundAlpha    = 0.45f;  // opacity of the filled ground disc
        float ringAlpha      = 0.90f;  // opacity of the bright ground ring
        float beamAlpha      = 0.60f;  // opacity of the beam at its base
        float pulse          = 0.20f;  // slow breathing depth, 0 = steady
        float pulseSpeed     = 1.60f;  // breathing rate

        float maxDistance    = 0.0f;   // ignore corpses farther than this, yards (0 = unlimited)
        bool  showGround     = true;   // paint the glow on the ground
        bool  showBeam       = true;   // raise the beam
        bool  throughWalls   = true;   // draw through terrain (a marker you can always find) or not

        // false (the default) marks every dead NPC; true additionally requires the server's
        // UNIT_DYNFLAG_LOOTABLE bit, so an already-looted corpse goes dark.
        bool  requireLootable = false;
    };

    class LootBeam final : public wxl::ext::EventScript
    {
    public:
        LootBeam(); // binds the event handlers

        /** @brief Stores the core's service table (used for logging). */
        void SetApi(const WXL_Api* api) { api_ = api; }

        /** @brief Loads the look from an INI file; later edits are picked up automatically. */
        void LoadConfig(const std::string& iniPath);

        /** @brief Draws the module's overlay panel body; called while the overlay is open. */
        void DrawPanel(const WXL_Api& api);

        /** @brief Writes the live look back to the INI. @return true when it was written. */
        bool SaveConfig();

        /** @brief Discards unsaved panel edits and re-reads the INI. */
        void RevertConfig();

        /** @brief True when the live look differs from what is on disk. */
        bool HasUnsavedChanges() const;

        static constexpr int kMaxBeacons = 64; // corpses beamed at once; the rest wait for a slot

    private:
        // --- event handlers ---
        void OnUpdate(const events::UpdateArgs& a);
        void OnWorldSceneEnd(const events::WorldSceneEndArgs& a);
        void OnWorldEnter(const events::WorldEnterArgs& a);
        void OnWorldLeave(const events::WorldLeaveArgs& a);

        // --- steps ---
        int  ScanUnits();                       // rebuild beacons_ for this frame; returns units seen
        void DumpUnit(void* unit, unsigned long long guid); // one-shot descriptor window for debugging
        void QueueBeacon(const float pos[3], float pulseScale); // ground glow + beam into the gfx queue
        void LoadConfigNow();
        void ReloadConfigIfChanged();
        void Log(int level, const char* fmt, ...) const;

        struct Beacon { float pos[3]; };

        Beacon         beacons_[kMaxBeacons]{};
        int            beaconCount_ = 0;

        BeamStyle      style_{};
        BeamStyle      saved_{}; // what the INI holds, for the unsaved-changes test
        std::string    iniPath_;
        unsigned long long configStamp_ = 0;
        bool           inWorld_ = false;
        float          phase_   = 0.0f;
        const WXL_Api* api_     = nullptr;

        // One-shot diagnostics: each fires once and then stays quiet.
        bool           loggedFirstScan_  = false;
        bool           loggedFirstFlush_ = false;
        bool           loggedClipDiag_   = false;
        int            emptyFrameStreak_ = 0;
        int            emptyWarnings_    = 0;
    };
}
