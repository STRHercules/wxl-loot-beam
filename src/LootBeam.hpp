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

#include <cstdint>
#include <string>

// World-space beacon over lootable corpses. It never touches a raw client address for anything the SDK
// models: the object walk, the unit position and the drawing all go through wxl::game, the loot's
// contents are read through the client's own script functions, and the only raw addresses the module
// carries -- the descriptor pointer and the two unit fields in UnitFields.hpp, and the live-loot GUID
// in LootFields.hpp -- are the ones the SDK does not model.
//
// The beacon is queued the same frame it is decided, on the logic tick, and handed to the draw at
// OnWorldSceneEnd, which is the one slot where geometry placed by world coordinate lands where its
// coordinates say (the scene's matrices are still on the device and its depth buffer is complete).
// Nothing is retained: an enemy that stops being dead stops having a beam with no cleanup.
namespace wxl::scripts::loot_beam
{
    // The beam categories a corpse's loot can fall into. The first is a body whose loot held only
    // money (no gear to colour by); the rest are the game's item qualities in quality order, so a
    // quality q maps to tier q + kTierPoor. A beacon whose tier is switched off is not drawn at all.
    enum GearTier
    {
        kTierCurrency = 0, // money-only loot (server hint 9)
        kTierPoor,         // quality 0  grey
        kTierCommon,       // quality 1  white
        kTierUncommon,     // quality 2  green
        kTierRare,         // quality 3  blue
        kTierEpic,         // quality 4  purple
        kTierLegendary,    // quality 5  orange
        kTierArtifact,     // quality 6  gold
        kTierHeirloom,     // quality 7  cyan
        kTierCount
    };

    /**
     * @brief One sparkle mote: a small glint that drifts, twinkles and is reborn where it flies.
     *
     * Positions are offsets from the beacon's own base, in yards. Each mote is simulated once per
     * frame and respawned in place when it outlives its life or climbs out of the top of the beam, so
     * a beacon carries its own self-refreshing field of motes rather than a global particle pool.
     */
    struct Sparkle
    {
        float pos[3]       = {};    // offset from the beacon base, yards
        float vel[3]       = {};    // the drift the mote moves with, yards/s
        float age          = 0.0f;  // seconds it has lived
        float life         = 1.0f;  // seconds before it respawns
        float twinkle      = 0.0f;  // phase of its brightness flicker, radians
        float twinkleSpeed = 0.0f;  // radians/s the flicker advances at
    };

    /** @brief One tier's look: whether it is drawn at all, and the colour it is drawn in. */
    struct TierStyle
    {
        bool  enabled = true;
        float color[3] = { 1.0f, 1.0f, 1.0f };
    };

    /** @brief User-tunable look, read from wxl-loot-beam.ini beside the DLL. */
    struct BeamStyle
    {
        bool  enabled        = true;   // master switch
        float height         = 15.0f;  // how far the beam rises, yards (the "15 metres" of the brief)
        float baseOffset     = 1.0f;   // how far above the body the shaft begins, yards; it fades in
                                       // from transparent there, so it gathers out of the air
        float groundRadius   = 1.70f;  // radius of the glow painted on the ground, yards
        float beamWidth      = 0.70f;  // half-width of the beam at its base, yards
        float widthPerYard   = 0.010f; // minimum half-width per yard of camera distance (0 = off); a
                                       // beam that never thins with distance stays a legible column
        float color[3]       = { 1.00f, 0.82f, 0.42f }; // warm gold; the fallback while a body's
                                                       // loot (and so its tier) is not yet known

        // Per-tier tint and on/off. Indexed by GearTier; the defaults below are the game's own item
        // quality colours, with the money-only "currency" tier given a bright gold of its own. A tier
        // switched off produces no beacon for corpses that fall into it.
        TierStyle tiers[kTierCount] = {
            { true, { 1.00f, 0.82f, 0.00f } }, // kTierCurrency
            { true, { 0.62f, 0.62f, 0.62f } }, // kTierPoor
            { true, { 1.00f, 1.00f, 1.00f } }, // kTierCommon
            { true, { 0.12f, 1.00f, 0.00f } }, // kTierUncommon
            { true, { 0.00f, 0.44f, 0.87f } }, // kTierRare
            { true, { 0.64f, 0.21f, 0.93f } }, // kTierEpic
            { true, { 1.00f, 0.50f, 0.00f } }, // kTierLegendary
            { true, { 0.90f, 0.80f, 0.50f } }, // kTierArtifact
            { true, { 0.00f, 0.80f, 1.00f } }, // kTierHeirloom
        };

        float groundAlpha    = 0.45f;  // opacity of the filled ground glow at its centre
        float beamAlpha      = 0.60f;  // opacity of the beam at its base
        float pulse          = 0.20f;  // slow breathing depth, 0 = steady
        float pulseSpeed     = 1.60f;  // breathing rate
        float fadeIn         = 0.35f;  // seconds for a new beacon to reach full opacity (0 = instant)
        float fadeOut        = 0.70f;  // seconds for a lost beacon to fade away (0 = instant)

        float maxDistance    = 0.0f;   // ignore corpses farther than this, yards (0 = unlimited)
        bool  showGround     = true;   // paint the glow on the ground
        bool  showBeam       = true;   // raise the beam
        // Draw the beacon through terrain and walls (the default), so a corpse tucked behind a rise is
        // never missed. Turn it off to let the world occlude the marker as real light would.
        bool  throughWalls   = true;

        // true (the default) marks only corpses the server still flags lootable, so an already-looted
        // body goes dark; false marks every dead NPC.
        bool  requireLootable = true;

        // true (the default) tints a corpse's beacon with the quality colour of the best item its loot
        // is known to hold, and falls back to Color / the panel's tint while that loot is unknown. The
        // 3.3.5a client only learns a corpse's loot when loot is requested for it, so the tint appears
        // once the body has been opened.
        bool  lootColor = true;

        // true (the default) prefers the quality the server put on the corpse (see UnitFields.hpp /
        // the companion AzerothCore module) over the loot the client discovers for itself. That lets
        // the beacon be the right colour the moment the body dies, without waiting for the loot
        // window. When the server sends no hint the local loot is used as before.
        bool  serverColor = true;

        // Sparkle motes that drift and twinkle around the beam, per beacon. The field is seeded from
        // the corpse's GUID, so no two bodies flicker in lockstep. Count is capped at kMaxSparkles.
        bool  showSparkles   = true;   // draw drifting sparkle motes around the beam
        int   sparkleCount   = 8;      // motes per beacon
        float sparkleSize    = 0.09f;  // half-extent of a mote, yards
        float sparkleAlpha   = 0.90f;  // peak opacity of a mote at the crest of its twinkle
        float sparkleRise    = 0.90f;  // upward drift, yards/s
        float sparkleDrift   = 0.35f;  // lateral wander, yards/s
        float sparkleLife    = 2.40f;  // seconds a mote lives before it respawns
        float sparkleTwinkle = 4.00f;  // flicker speed, radians/s
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

        static constexpr int kMaxBeacons  = 64; // corpses beamed at once; the rest wait for a slot
        static constexpr int kMaxSparkles = 32; // mote field size a single beacon can carry

    private:
        struct Beacon; // one tracked corpse, defined below but named by the steps above

        // --- event handlers ---
        void OnUpdate(const events::UpdateArgs& a);
        void OnWorldSceneEnd(const events::WorldSceneEndArgs& a);
        void OnWorldEnter(const events::WorldEnterArgs& a);
        void OnWorldLeave(const events::WorldLeaveArgs& a);

        // --- steps ---
        int  ScanUnits();                       // mark tracked beacons seen this frame; returns units seen
        void ScanLoot();                        // adopt the open loot's best quality onto its corpse
        int  ReadLootTier();                    // GearTier of the currently open loot, or -1
        void UpdateFade(float dt);              // advance each beacon's fade; drop the dead ones
        void DumpUnit(void* unit, unsigned long long guid); // one-shot descriptor window for debugging
        void QueueBeacon(const Beacon& beacon, float alphaScale, const float rgb[3]); // glow + beam + motes
        void SeedSparkles(Beacon& b);  // fill a new beacon's mote field from its GUID
        void AdvanceSparkles(Beacon& b, float dt); // drift and respawn a beacon's motes
        void LoadConfigNow();
        void ReloadConfigIfChanged();
        void Log(int level, const char* fmt, ...) const;

        struct Beacon
        {
            unsigned long long guid = 0;
            float              pos[3] = {};
            float              fade   = 0.0f; // 0..1 opacity multiplier, eased over fadeIn/fadeOut
            int                tier = -1;     // GearTier of the corpse's loot, -1 until known
            bool               serverTint = false; // tier came from the server, not the open loot
            bool               seen   = false;

            Sparkle            sparkles[kMaxSparkles]{}; // self-refreshing mote field for this body
            uint32_t           rngSeed = 0;              // xorshift state the respawns draw from
        };

        Beacon         beacons_[kMaxBeacons]{}; // persists across frames so a beacon can fade
        int            beaconCount_ = 0;        // tracked entries with a visible fade this frame
        int            trackedCount_ = 0;

        BeamStyle      style_{};
        BeamStyle      saved_{}; // what the INI holds, for the unsaved-changes test
        unsigned long long lootGuid_    = 0;  // GUID of the loot the cached tier belongs to
        int                lootTier_    = -1; // its GearTier, recomputed when the GUID changes
        std::string    iniPath_;
        unsigned long long configStamp_ = 0;
        bool           inWorld_ = false;
        float          phase_   = 0.0f;
        const WXL_Api* api_     = nullptr;

        // One-shot diagnostics: each fires once and then stays quiet.
        bool           loggedFirstScan_  = false;
        bool           loggedFirstFlush_ = false;
        bool           loggedClipDiag_   = false;
        bool           loggedServerHint_ = false;
        int            emptyFrameStreak_ = 0;
        int            emptyWarnings_    = 0;
    };
}
