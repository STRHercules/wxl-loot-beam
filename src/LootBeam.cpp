// wxl-loot-beam: the world-space beacon over lootable NPC bodies.
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

#include "LootBeam.hpp"
#include "UnitFields.hpp"

#include "game/Camera.hpp"
#include "game/Gfx.hpp"
#include "game/Pick.hpp"
#include "game/World.hpp"

#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wxl::scripts::loot_beam
{
    namespace ev    = wxl::events;
    namespace gfx   = wxl::game::gfx;
    namespace gx    = wxl::game::gx;
    namespace world = wxl::game::world;
    namespace cam   = wxl::game::camera;

    namespace
    {
        using namespace wxl_loot_beam; // the descriptor-field offsets from UnitFields.hpp

        constexpr const char* kIniSection = "LootBeam";
        constexpr float       kTwoPi      = 6.28318530717959f;
        constexpr float       kUnitToByte = 255.0f;
        // Bumped when a shipped default changes in a way an existing file must adopt. A file older
        // than this has its stale distance/depth keys replaced with the always-visible defaults.
        constexpr int         kConfigVersion = 3;

        // Descriptor reads are guarded: a wrong field index or a half-built object reads a nearby heap
        // dword. The validators reject an address that cannot be a live block before the SEH frame is
        // even entered, and the frame catches the rest.
        bool ValidPointer(uintptr_t address, size_t size)
        {
            return address >= 0x10000 && (address & 3) == 0 && address < 0xFFF00000 &&
                   size <= 0xFFF00000 - address;
        }

        bool ReadU32(uintptr_t address, uint32_t& out)
        {
            __try
            {
                out = *reinterpret_cast<const uint32_t*>(address);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool ReadPtr(uintptr_t address, uintptr_t& out)
        {
            __try
            {
                out = *reinterpret_cast<const uintptr_t*>(address);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

        gfx::Color Pack(float alpha, const float rgb[3])
        {
            const uint32_t a = uint32_t(Clamp01(alpha) * kUnitToByte + 0.5f);
            const uint32_t r = uint32_t(Clamp01(rgb[0]) * kUnitToByte + 0.5f);
            const uint32_t g = uint32_t(Clamp01(rgb[1]) * kUnitToByte + 0.5f);
            const uint32_t b = uint32_t(Clamp01(rgb[2]) * kUnitToByte + 0.5f);
            return (a << 24) | (r << 16) | (g << 8) | b;
        }

        /**
         * @brief Reads a unit's health field. False when the object has no readable update block.
         */
        bool UnitHealth(void* unit, uint32_t& health)
        {
            uintptr_t descriptors = 0;
            if (!ReadPtr(reinterpret_cast<uintptr_t>(unit) + kObjectDescriptorField, descriptors))
                return false;
            if (!ValidPointer(descriptors, kUnitHealthField + sizeof(uint32_t)))
                return false;
            return ReadU32(descriptors + kUnitHealthField, health);
        }

        /**
         * @brief True when the object is a unit that is dead and (optionally) still flagged lootable.
         *
         * Dead is the health field being zero. The strict test adds UNIT_DYNFLAG_LOOTABLE, but only
         * trusts the flags value when every set bit is one the field may carry -- a wrong offset lands
         * on unrelated heap whose high bits betray it, and the test then falls back to the health-only
         * verdict instead of turning into noise.
         */
        bool IsLootableCorpse(void* unit, const BeamStyle& style)
        {
            const unsigned mask = world::TypeMask(unit);
            if (!(mask & world::kTypeMaskUnit)) return false;
            if (mask & world::kTypeMaskPlayer) return false; // a player corpse gets its own marker

            uint32_t health = 1;
            if (!UnitHealth(unit, health) || health != 0) return false;

            if (style.requireLootable)
            {
                uintptr_t descriptors = 0;
                if (!ReadPtr(reinterpret_cast<uintptr_t>(unit) + kObjectDescriptorField, descriptors))
                    return false;
                if (!ValidPointer(descriptors, kUnitDynamicFlagsField + sizeof(uint32_t)))
                    return false;

                uint32_t flags = 0;
                const bool read = ReadU32(descriptors + kUnitDynamicFlagsField, flags);
                if (read && (flags & ~kDynamicFlagKnownMask) == 0)
                {
                    if (!(flags & kDynamicFlagLootable)) return false;
                }
            }
            return true;
        }

        // --- INI helpers -------------------------------------------------------------------------

        unsigned long long FileStamp(const std::string& path)
        {
            WIN32_FILE_ATTRIBUTE_DATA data;
            if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
                return 0;
            return (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                   data.ftLastWriteTime.dwLowDateTime;
        }

        std::string Trim(std::string text)
        {
            const size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
                return std::string();
            const size_t last = text.find_last_not_of(" \t\r\n");
            return text.substr(first, last - first + 1);
        }

        bool ReadBool(const std::string& path, const char* key, bool fallback)
        {
            return GetPrivateProfileIntA(kIniSection, key, fallback ? 1 : 0, path.c_str()) != 0;
        }

        float ReadFloat(const std::string& path, const char* key, float fallback, float lo, float hi)
        {
            char buf[64] = {};
            GetPrivateProfileStringA(kIniSection, key, "", buf, sizeof(buf), path.c_str());
            if (!buf[0])
                return fallback;
            char* end = nullptr;
            const float v = std::strtof(buf, &end);
            if (end == buf || v != v)
                return fallback;
            return v < lo ? lo : (v > hi ? hi : v);
        }

        // Accepts "#RRGGBB" / "RRGGBB" or "R,G,B" (0-255). Anything else leaves out untouched.
        void ReadColor(const std::string& path, const char* key, float out[3])
        {
            char buf[64] = {};
            GetPrivateProfileStringA(kIniSection, key, "", buf, sizeof(buf), path.c_str());
            const std::string text = Trim(buf);
            if (text.empty())
                return;

            int r = -1, g = -1, b = -1;
            if (text.find(',') != std::string::npos)
            {
                if (std::sscanf(text.c_str(), "%d , %d , %d", &r, &g, &b) != 3)
                    return;
            }
            else
            {
                const char* hex = text.c_str();
                if (*hex == '#')
                    ++hex;
                char* end = nullptr;
                const unsigned long v = std::strtoul(hex, &end, 16);
                if (end == hex || *end != '\0' || std::strlen(hex) != 6)
                    return;
                r = int((v >> 16) & 0xFF);
                g = int((v >> 8) & 0xFF);
                b = int(v & 0xFF);
            }

            out[0] = Clamp01(float(r < 0 ? 0 : (r > 255 ? 255 : r)) / 255.0f);
            out[1] = Clamp01(float(g < 0 ? 0 : (g > 255 ? 255 : g)) / 255.0f);
            out[2] = Clamp01(float(b < 0 ? 0 : (b > 255 ? 255 : b)) / 255.0f);
        }

        void WriteInt(const std::string& path, const char* key, int value)
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d", value);
            WritePrivateProfileStringA(kIniSection, key, buf, path.c_str());
        }

        void WriteFloat(const std::string& path, const char* key, float value)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.3f", value);
            WritePrivateProfileStringA(kIniSection, key, buf, path.c_str());
        }

        void WriteColor(const std::string& path, const char* key, const float rgb[3])
        {
            const int r = int(Clamp01(rgb[0]) * 255.0f + 0.5f);
            const int g = int(Clamp01(rgb[1]) * 255.0f + 0.5f);
            const int b = int(Clamp01(rgb[2]) * 255.0f + 0.5f);
            char buf[16];
            std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
            WritePrivateProfileStringA(kIniSection, key, buf, path.c_str());
        }

        bool SameStyle(const BeamStyle& a, const BeamStyle& b)
        {
            return a.enabled == b.enabled && a.height == b.height &&
                   a.groundRadius == b.groundRadius && a.beamWidth == b.beamWidth &&
                   a.widthPerYard == b.widthPerYard &&
                   a.color[0] == b.color[0] && a.color[1] == b.color[1] && a.color[2] == b.color[2] &&
                   a.groundAlpha == b.groundAlpha && a.ringAlpha == b.ringAlpha &&
                   a.beamAlpha == b.beamAlpha && a.pulse == b.pulse && a.pulseSpeed == b.pulseSpeed &&
                   a.fadeIn == b.fadeIn && a.fadeOut == b.fadeOut &&
                   a.maxDistance == b.maxDistance && a.showGround == b.showGround &&
                   a.showBeam == b.showBeam && a.throughWalls == b.throughWalls &&
                   a.requireLootable == b.requireLootable;
        }
    }

    LootBeam::LootBeam()
    {
        on<&LootBeam::OnUpdate>(ev::Event::OnUpdate);
        on<&LootBeam::OnWorldSceneEnd>(ev::Event::OnWorldSceneEnd);
        on<&LootBeam::OnWorldEnter>(ev::Event::OnWorldEnter);
        on<&LootBeam::OnWorldLeave>(ev::Event::OnWorldLeave);
    }

    void LootBeam::Log(int level, const char* fmt, ...) const
    {
        if (!api_ || !api_->Log) return;
        va_list ap;
        va_start(ap, fmt);
        char msg[512];
        std::vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        api_->Log(level, "wxl-loot-beam", "%s", msg);
    }

    void LootBeam::LoadConfig(const std::string& iniPath)
    {
        iniPath_     = iniPath;
        configStamp_ = FileStamp(iniPath_);
        LoadConfigNow();

        // A first run has no file to edit, so lay one down from the defaults.
        if (FileStamp(iniPath_) == 0)
        {
            SaveConfig();
            Log(WXL_LOG_INFO, "wrote default config to %s", iniPath_.c_str());
        }

        Log(WXL_LOG_INFO,
            "config: throughWalls=%d maxDistance=%.0f height=%.0f beamWidth=%.2f widthPerYard=%.3f "
            "beamAlpha=%.2f",
            style_.throughWalls ? 1 : 0, style_.maxDistance, style_.height, style_.beamWidth,
            style_.widthPerYard, style_.beamAlpha);
    }

    void LootBeam::LoadConfigNow()
    {
        BeamStyle s = BeamStyle{};
        s.enabled        = ReadBool(iniPath_,  "Enabled",       s.enabled);
        s.height         = ReadFloat(iniPath_, "Height",        s.height,        0.0f, 40.0f);
        s.groundRadius   = ReadFloat(iniPath_, "GroundRadius",  s.groundRadius,  0.2f, 6.0f);
        s.beamWidth      = ReadFloat(iniPath_, "BeamWidth",     s.beamWidth,     0.05f, 2.0f);
        s.widthPerYard   = ReadFloat(iniPath_, "WidthPerYard",  s.widthPerYard,  0.0f, 0.05f);
        s.groundAlpha    = ReadFloat(iniPath_, "GroundAlpha",   s.groundAlpha,   0.0f, 1.0f);
        s.ringAlpha      = ReadFloat(iniPath_, "RingAlpha",     s.ringAlpha,     0.0f, 1.0f);
        s.beamAlpha      = ReadFloat(iniPath_, "BeamAlpha",     s.beamAlpha,     0.0f, 1.0f);
        s.pulse          = ReadFloat(iniPath_, "Pulse",         s.pulse,         0.0f, 1.0f);
        s.pulseSpeed     = ReadFloat(iniPath_, "PulseSpeed",    s.pulseSpeed,    0.0f, 6.0f);
        s.fadeIn         = ReadFloat(iniPath_, "FadeIn",        s.fadeIn,        0.0f, 5.0f);
        s.fadeOut        = ReadFloat(iniPath_, "FadeOut",       s.fadeOut,       0.0f, 5.0f);
        s.maxDistance    = ReadFloat(iniPath_, "MaxDistance",   s.maxDistance,   0.0f, 400.0f);
        s.showGround     = ReadBool(iniPath_,  "ShowGround",    s.showGround);
        s.showBeam       = ReadBool(iniPath_,  "ShowBeam",      s.showBeam);
        s.throughWalls   = ReadBool(iniPath_,  "ThroughWalls",  s.throughWalls);
        s.requireLootable= ReadBool(iniPath_,  "RequireLootable", s.requireLootable);
        ReadColor(iniPath_, "Color", s.color);

        // A file written by an older build carries a distance cap and a depth-tested beacon that made
        // it visible only up close. Adopt the always-visible defaults for exactly those keys and
        // persist them, so an existing install does not have to be edited by hand.
        const int version = GetPrivateProfileIntA(kIniSection, "ConfigVersion", 0, iniPath_.c_str());
        const bool migrated = version < kConfigVersion;
        if (migrated)
        {
            if (version < 2)
            {
                s.maxDistance = 0.0f;
                s.throughWalls = true;
                if (s.beamWidth < 0.7f) s.beamWidth = 0.7f;
                if (s.beamAlpha < 0.6f) s.beamAlpha = 0.6f;
            }
            // Version 3 marks only still-lootable corpses, so a looted body's beam goes away; older
            // files defaulted to marking every corpse.
            if (version < 3) s.requireLootable = true;
        }

        style_ = s;
        saved_ = s;

        if (migrated)
        {
            Log(WXL_LOG_INFO, "migrating config from version %d to %d", version, kConfigVersion);
            SaveConfig();
        }
    }

    void LootBeam::ReloadConfigIfChanged()
    {
        if (iniPath_.empty())
            return;

        // The stat is not free; a live edit does not need frame-accuracy.
        static DWORD lastCheck = 0;
        const DWORD now = GetTickCount();
        if (now - lastCheck < 1000u)
            return;
        lastCheck = now;

        const unsigned long long stamp = FileStamp(iniPath_);
        if (stamp == configStamp_)
            return;
        configStamp_ = stamp;
        LoadConfigNow();
        Log(WXL_LOG_INFO, "config reloaded from %s", iniPath_.c_str());
    }

    bool LootBeam::SaveConfig()
    {
        if (iniPath_.empty())
            return false;

        WriteInt(iniPath_,   "Enabled",         style_.enabled ? 1 : 0);
        WriteFloat(iniPath_, "Height",          style_.height);
        WriteFloat(iniPath_, "GroundRadius",    style_.groundRadius);
        WriteFloat(iniPath_, "BeamWidth",       style_.beamWidth);
        WriteFloat(iniPath_, "WidthPerYard",    style_.widthPerYard);
        WriteFloat(iniPath_, "GroundAlpha",     style_.groundAlpha);
        WriteFloat(iniPath_, "RingAlpha",       style_.ringAlpha);
        WriteFloat(iniPath_, "BeamAlpha",       style_.beamAlpha);
        WriteFloat(iniPath_, "Pulse",           style_.pulse);
        WriteFloat(iniPath_, "PulseSpeed",      style_.pulseSpeed);
        WriteFloat(iniPath_, "FadeIn",          style_.fadeIn);
        WriteFloat(iniPath_, "FadeOut",         style_.fadeOut);
        WriteFloat(iniPath_, "MaxDistance",     style_.maxDistance);
        WriteInt(iniPath_,   "ShowGround",      style_.showGround ? 1 : 0);
        WriteInt(iniPath_,   "ShowBeam",        style_.showBeam ? 1 : 0);
        WriteInt(iniPath_,   "ThroughWalls",    style_.throughWalls ? 1 : 0);
        WriteInt(iniPath_,   "RequireLootable", style_.requireLootable ? 1 : 0);
        WriteColor(iniPath_, "Color",           style_.color);
        WriteInt(iniPath_,   "ConfigVersion",   kConfigVersion);

        // The write bumps the file stamp; adopt it so the self-write is not mistaken for an external
        // edit (which would overwrite a panel tweak made right after Save).
        configStamp_ = FileStamp(iniPath_);
        saved_       = style_;
        Log(WXL_LOG_INFO, "settings saved to %s", iniPath_.c_str());
        return true;
    }

    void LootBeam::RevertConfig()
    {
        LoadConfigNow();
        Log(WXL_LOG_INFO, "settings reverted to %s", iniPath_.c_str());
    }

    bool LootBeam::HasUnsavedChanges() const
    {
        return !SameStyle(style_, saved_);
    }

    void LootBeam::DrawPanel(const WXL_Api& api)
    {
        if (!api.UiCheckbox || !api.UiSliderFloat || !api.UiSeparator || !api.UiText ||
            !api.UiColorEdit || !api.UiButton || !api.UiSameLine || !api.UiCollapsingHeader)
            return;

        int enabled = style_.enabled ? 1 : 0;
        if (api.UiCheckbox("Enable", &enabled)) style_.enabled = enabled != 0;

        api.UiText(inWorld_ ? "status: scanning for lootable bodies"
                            : "status: waiting for a world");

        if (api.UiCollapsingHeader("Beacon"))
        {
            api.UiSliderFloat("Height (yd)", &style_.height, 0.0f, 40.0f);
            api.UiSliderFloat("Ground radius (yd)", &style_.groundRadius, 0.2f, 6.0f);
            api.UiSliderFloat("Beam width (yd)", &style_.beamWidth, 0.05f, 2.0f);
            api.UiSliderFloat("Min width / yd", &style_.widthPerYard, 0.0f, 0.05f);
            api.UiSliderFloat("Ground alpha", &style_.groundAlpha, 0.0f, 1.0f);
            api.UiSliderFloat("Ring alpha", &style_.ringAlpha, 0.0f, 1.0f);
            api.UiSliderFloat("Beam alpha", &style_.beamAlpha, 0.0f, 1.0f);

            float rgba[4] = { style_.color[0], style_.color[1], style_.color[2], 1.0f };
            if (api.UiColorEdit("Colour", rgba))
            {
                style_.color[0] = rgba[0];
                style_.color[1] = rgba[1];
                style_.color[2] = rgba[2];
            }
        }

        if (api.UiCollapsingHeader("Behaviour"))
        {
            api.UiSliderFloat("Pulse", &style_.pulse, 0.0f, 1.0f);
            api.UiSliderFloat("Pulse speed", &style_.pulseSpeed, 0.0f, 6.0f);
            api.UiSliderFloat("Fade in (s)", &style_.fadeIn, 0.0f, 5.0f);
            api.UiSliderFloat("Fade out (s)", &style_.fadeOut, 0.0f, 5.0f);
            api.UiSliderFloat("Max distance (yd)", &style_.maxDistance, 0.0f, 400.0f);

            int ground = style_.showGround ? 1 : 0;
            if (api.UiCheckbox("Ground glow", &ground)) style_.showGround = ground != 0;
            int beam = style_.showBeam ? 1 : 0;
            if (api.UiCheckbox("Beam", &beam)) style_.showBeam = beam != 0;
            int walls = style_.throughWalls ? 1 : 0;
            if (api.UiCheckbox("Through walls", &walls)) style_.throughWalls = walls != 0;
            int lootable = style_.requireLootable ? 1 : 0;
            if (api.UiCheckbox("Only lootable corpses", &lootable)) style_.requireLootable = lootable != 0;
        }

        api.UiSeparator();
        if (api.UiButton("Save")) SaveConfig();
        api.UiSameLine();
        if (api.UiButton("Revert")) RevertConfig();
        api.UiText(HasUnsavedChanges() ? "Unsaved changes" : "Matches wxl-loot-beam.ini");
    }

    int LootBeam::ScanUnits()
    {
        // Everything tracked starts unseen; a corpse that qualifies this frame marks itself seen, so
        // what is left unseen afterward is a body that was looted or despawned and must fade out.
        for (int i = 0; i < trackedCount_; ++i)
            beacons_[i].seen = false;

        beaconCount_ = 0;

        // A zero active-player GUID means no live session, and the object walk dereferences the
        // thread-local object manager without checking -- do not enter it there.
        if (world::ActivePlayerGuid() == 0)
            return 0;

        float camera[3];
        cam::GetPosition(camera);
        const float maxD2 = style_.maxDistance > 0.0f ? style_.maxDistance * style_.maxDistance : 0.0f;

        int  enumerated = 0;
        bool dumped     = false;

        // The walk reads the resident-object list; it is main-thread only, which the logic tick is.
        world::ForEachObject(world::kTypeMaskUnit, [&](unsigned long long guid, void* obj) -> bool {
            ++enumerated;
            if (!dumped)
            {
                uint32_t h = 1;
                if (UnitHealth(obj, h) && h == 0)
                {
                    dumped = true;
                    DumpUnit(obj, guid);
                }
            }
            if (!IsLootableCorpse(obj, style_)) return true;

            float p[3];
            world::UnitPosition(obj, p);

            if (maxD2 > 0.0f)
            {
                const float dx = p[0] - camera[0];
                const float dy = p[1] - camera[1];
                const float dz = p[2] - camera[2];
                if (dx * dx + dy * dy + dz * dz > maxD2) return true;
            }

            // Match on GUID so a corpse keeps its fade level while it is tracked; a newly seen one
            // enters at zero and eases up.
            Beacon* b = nullptr;
            for (int i = 0; i < trackedCount_; ++i)
            {
                if (beacons_[i].guid == guid) { b = &beacons_[i]; break; }
            }
            if (!b)
            {
                if (trackedCount_ >= kMaxBeacons) return true; // table full; let it go this pass
                b = &beacons_[trackedCount_++];
                b->guid = guid;
                b->fade = 0.0f;
            }
            b->pos[0] = p[0];
            b->pos[1] = p[1];
            b->pos[2] = p[2];
            b->seen   = true;
            return true;
        });

        return enumerated;
    }

    // Advances every tracked beacon's fade toward its target -- full when it was seen this frame, zero
    // when it was not -- and forgets the ones that have finished fading out.
    void LootBeam::UpdateFade(float dt)
    {
        const float inRate  = style_.fadeIn  > 0.001f ? 1.0f / style_.fadeIn  : 1.0e9f;
        const float outRate = style_.fadeOut > 0.001f ? 1.0f / style_.fadeOut : 1.0e9f;

        int kept = 0;
        for (int i = 0; i < trackedCount_; ++i)
        {
            Beacon b = beacons_[i];
            if (b.seen)
            {
                b.fade += inRate * dt;
                if (b.fade > 1.0f) b.fade = 1.0f;
            }
            else
            {
                b.fade -= outRate * dt;
                if (b.fade <= 0.0f) continue; // finished fading; forget it
            }
            beacons_[kept++] = b;
        }
        trackedCount_ = kept;

        beaconCount_ = 0;
        for (int i = 0; i < trackedCount_; ++i)
        {
            if (beacons_[i].fade > 0.001f) ++beaconCount_;
        }
    }

    // Reads a window of the update-field block around where health is expected and writes it to the
    // log, once per session. The field that reads 0 on a corpse is the health field; if none of them
    // do, the descriptor layout assumption is wrong and the offsets in UnitFields.hpp need revising.
    void LootBeam::DumpUnit(void* unit, unsigned long long guid)
    {
        uintptr_t descriptors = 0;
        if (!ReadPtr(reinterpret_cast<uintptr_t>(unit) + kObjectDescriptorField, descriptors) ||
            !ValidPointer(descriptors, 0x100))
        {
            Log(WXL_LOG_INFO, "diag: first unit guid=%llX has no readable descriptor", guid);
            return;
        }

        char window[320] = {};
        int  n = 0;
        // Health lives at 0x60; the strict path's dynamic flags at 0x13C. Dump both neighbourhoods so
        // a future client build can be checked at a glance.
        for (size_t off = 0x58; off <= 0x78 && n < int(sizeof(window)) - 24; off += 4)
        {
            uint32_t v = 0;
            if (!ReadU32(descriptors + off, v))
                v = 0xDEADBEEFu;
            n += std::snprintf(window + n, sizeof(window) - n, " %02X=%u", unsigned(off), v);
        }
        Log(WXL_LOG_INFO, "diag: first unit guid=%llX desc=%p%s", guid, (void*)descriptors, window);

        n = 0;
        for (size_t off = 0x12C; off <= 0x140 && n < int(sizeof(window)) - 24; off += 4)
        {
            uint32_t v = 0;
            if (!ReadU32(descriptors + off, v))
                v = 0xDEADBEEFu;
            n += std::snprintf(window + n, sizeof(window) - n, " %02X=%u", unsigned(off), v);
        }
        Log(WXL_LOG_INFO, "diag: first unit guid=%llX dynflags%s", guid, window);
    }

    namespace
    {
        // Soft falloff from a centre value of 1 at x = 0 to 0 at x = 1, with a flat top and a gentle
        // shoulder -- the difference between a lit volume and a hard-edged quad.
        float SoftEdge(float x)
        {
            const float a = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
            return 1.0f - a * a * (3.0f - 2.0f * a);
        }

        // The tint, pulled toward white by `whiten`: the middle of a light shaft is hotter and whiter
        // than its cooler, more coloured edges.
        gfx::Color PackTint(float alpha, const float rgb[3], float whiten)
        {
            const float c[3] = {
                rgb[0] + (1.0f - rgb[0]) * whiten,
                rgb[1] + (1.0f - rgb[1]) * whiten,
                rgb[2] + (1.0f - rgb[2]) * whiten,
            };
            return Pack(alpha, c);
        }

        // A pool of light on the ground, built ring by ring so its opacity falls off smoothly from the
        // hot centre to nothing at the rim and follows the terrain at every vertex. A single flat disc
        // reads as a painted circle; stacking a few flat discs reads as bands.
        void QueueGroundGlow(const float pos[3], const BeamStyle& style, float alphaScale, gfx::Depth depth)
        {
            constexpr int   kSegments = 28;
            constexpr int   kRings    = 6;
            constexpr float kLift     = 0.12f; // clear of the terrain so it does not z-fight it

            float centre[3];
            {
                float z = pos[2];
                world::GroundZ(pos[0], pos[1], pos[2], z);
                centre[0] = pos[0]; centre[1] = pos[1]; centre[2] = z + kLift;
            }

            float inner[kSegments][3] = {};
            float outer[kSegments][3] = {};

            for (int r = 1; r <= kRings; ++r)
            {
                const float radius = style.groundRadius * float(r) / float(kRings);
                for (int i = 0; i < kSegments; ++i)
                {
                    const float angle = kTwoPi * float(i) / float(kSegments);
                    const float x = pos[0] + cosf(angle) * radius;
                    const float y = pos[1] + sinf(angle) * radius;
                    float z = pos[2];
                    world::GroundZ(x, y, pos[2], z);
                    outer[i][0] = x; outer[i][1] = y; outer[i][2] = z + kLift;
                }

                const float fall = SoftEdge(float(r) / float(kRings)); // 1 at the core, 0 at the rim
                const float alpha = style.groundAlpha * alphaScale * fall * 0.9f;
                if (alpha <= 0.002f)
                {
                    for (int i = 0; i < kSegments; ++i)
                        for (int k = 0; k < 3; ++k) inner[i][k] = outer[i][k];
                    continue;
                }
                const gfx::Color c = PackTint(alpha, style.color, 0.35f * fall);

                for (int i = 0; i < kSegments; ++i)
                {
                    const int n = (i + 1) % kSegments;
                    if (r == 1)
                        gfx::Triangle(centre, outer[i], outer[n], c, depth);
                    else
                    {
                        gfx::Triangle(inner[i], outer[i], outer[n], c, depth);
                        gfx::Triangle(inner[i], outer[n], inner[n], c, depth);
                    }
                }

                for (int i = 0; i < kSegments; ++i)
                    for (int k = 0; k < 3; ++k) inner[i][k] = outer[i][k];
            }

            // A faint halo at the rim to soften where the pool ends.
            gfx::GroundRing(pos, style.groundRadius,
                            PackTint(style.ringAlpha * 0.28f * alphaScale, style.color, 0.0f), 48, depth);
        }

        // The shaft: one camera-facing billboard, gridded across its width and up its height. Each cell
        // takes an opacity from a horizontal falloff (bright core, transparent edges) times a vertical
        // one (strong at the base, easing to nothing at the top) and its tint is whited toward the core.
        // Enough cells make the gradients read as a soft volume; a single quad per band read as a slab.
        void QueueBeamColumn(const float pos[3], float groundZ, const BeamStyle& style, float alphaScale,
                             gfx::Depth depth)
        {
            float camera[3];
            cam::GetPosition(camera);
            const float dx = camera[0] - pos[0];
            const float dy = camera[1] - pos[1];
            const float dz = camera[2] - pos[2];
            const float len = sqrtf(dx * dx + dy * dy);
            const float dist = sqrtf(dx * dx + dy * dy + dz * dz);

            // Billboard across the view direction: the only vertical plane the camera ever sees face-on.
            float fx = 1.0f, fy = 0.0f;
            if (len > 1e-3f) { fx = dx / len; fy = dy / len; }
            const float sx = -fy, sy = fx;

            // A fixed-width beam becomes a sub-pixel thread at range; grow a floor under the half-width
            // so a distant corpse still shows a column. 0 leaves the taper alone.
            const float minHalfWidth = style.widthPerYard > 0.0f ? style.widthPerYard * dist : 0.0f;

            const float baseZ = groundZ + 0.05f;
            const float topZ  = groundZ + style.height;
            constexpr int kRows = 12;
            constexpr int kCols = 6;

            for (int r = 0; r < kRows; ++r)
            {
                const float t0 = float(r) / float(kRows);
                const float t1 = float(r + 1) / float(kRows);
                const float z0 = baseZ + (topZ - baseZ) * t0;
                const float z1 = baseZ + (topZ - baseZ) * t1;
                const float w0 = fmaxf(style.beamWidth * (1.0f - 0.55f * t0), minHalfWidth);
                const float w1 = fmaxf(style.beamWidth * (1.0f - 0.55f * t1), minHalfWidth);
                const float vv = 0.5f * (powf(1.0f - t0, 1.4f) + powf(1.0f - t1, 1.4f));

                for (int c = 0; c < kCols; ++c)
                {
                    const float u0 = -1.0f + 2.0f * float(c) / float(kCols);
                    const float u1 = -1.0f + 2.0f * float(c + 1) / float(kCols);
                    const float h  = SoftEdge(fabsf(0.5f * (u0 + u1))); // 1 at the core, 0 at the rim
                    const float alpha = style.beamAlpha * vv * h * alphaScale;
                    if (alpha <= 0.002f) continue;

                    const gfx::Color col = PackTint(alpha, style.color, 0.45f * h);
                    const float ax = sx, ay = sy;

                    const float p0[3] = { pos[0] + ax * (w0 * u0), pos[1] + ay * (w0 * u0), z0 };
                    const float p1[3] = { pos[0] + ax * (w0 * u1), pos[1] + ay * (w0 * u1), z0 };
                    const float p2[3] = { pos[0] + ax * (w1 * u1), pos[1] + ay * (w1 * u1), z1 };
                    const float p3[3] = { pos[0] + ax * (w1 * u0), pos[1] + ay * (w1 * u0), z1 };
                    gfx::Triangle(p0, p1, p2, col, depth);
                    gfx::Triangle(p0, p2, p3, col, depth);
                }
            }
        }
    }

    void LootBeam::QueueBeacon(const float pos[3], float alphaScale)
    {
        const gfx::Depth depth = style_.throughWalls ? gfx::Depth::Through : gfx::Depth::Tested;

        float groundZ = pos[2];
        if (!world::GroundZ(pos[0], pos[1], pos[2], groundZ))
            groundZ = pos[2];

        if (style_.showGround)
            QueueGroundGlow(pos, style_, alphaScale, depth);

        if (style_.showBeam && style_.height > 0.01f)
            QueueBeamColumn(pos, groundZ, style_, alphaScale, depth);
    }

    void LootBeam::OnWorldEnter(const ev::WorldEnterArgs& a)
    {
        inWorld_ = true;
        Log(WXL_LOG_INFO, "world entered (map %u)", a.mapId);
    }

    void LootBeam::OnWorldLeave(const ev::WorldLeaveArgs&)
    {
        inWorld_    = false;
        beaconCount_ = 0;
        trackedCount_ = 0;
        gfx::Clear();
    }

    void LootBeam::OnUpdate(const ev::UpdateArgs& a)
    {
        ReloadConfigIfChanged();
        phase_ += a.dt * style_.pulseSpeed * kTwoPi;
        phase_ = fmodf(phase_, kTwoPi); // keep the phase bounded over a long session

        // One frame's worth of shapes only. The flush below empties the queue on the normal path, but
        // a frame that never reached the world scene pass would otherwise leave its shapes to pile up
        // under the next one.
        gfx::Clear();

        // Derive the world state live rather than trusting OnWorldEnter alone: a module loaded after
        // the client was already in-world would otherwise never see the enter event and stay dark.
        inWorld_ = world::CurrentMapId() >= 0;

        if (!style_.enabled || !inWorld_)
        {
            beaconCount_  = 0;
            trackedCount_ = 0;
            return;
        }

        const int enumerated = ScanUnits();
        UpdateFade(a.dt);
        if (beaconCount_ == 0)
        {
            if (!loggedFirstScan_)
            {
                loggedFirstScan_ = true;
                Log(WXL_LOG_INFO, "diag: map=%d player=%llu enumerated=%d beacons=0 (first scan)",
                    world::CurrentMapId(), world::ActivePlayerGuid(), enumerated);
            }
            if (++emptyFrameStreak_ >= 240 && emptyWarnings_ < 5)
            {
                ++emptyWarnings_;
                emptyFrameStreak_ = 0;
                Log(WXL_LOG_WARN,
                    "diag: 240 frames in world, enumerated=%d, no dead units -- the health field may be wrong",
                    enumerated);
            }
            return;
        }
        emptyFrameStreak_ = 0;

        if (!loggedFirstScan_)
        {
            loggedFirstScan_ = true;
            Log(WXL_LOG_INFO, "diag: map=%d player=%llu enumerated=%d beacons=%d (first scan)",
                world::CurrentMapId(), world::ActivePlayerGuid(), enumerated, beaconCount_);
        }

        // -1..+1 mapped into [1 - pulse, 1], so the beacon never gets brighter than the configured
        // alpha and a pulse of 0 is perfectly steady. The per-beacon fade folds in on top, so a body
        // still fading in or out is dimmed for the whole of its crossing.
        const float pulseScale = 1.0f - 0.5f * style_.pulse * (1.0f - sinf(phase_));
        for (int i = 0; i < trackedCount_; ++i)
        {
            if (beacons_[i].fade <= 0.001f) continue;
            QueueBeacon(beacons_[i].pos, pulseScale * beacons_[i].fade);
        }
    }

    void LootBeam::OnWorldSceneEnd(const ev::WorldSceneEndArgs& a)
    {
        if (!style_.enabled || !inWorld_ || beaconCount_ == 0)
        {
            gfx::Clear();
            return;
        }

        gx::Device9 dev(a.device);

        // One-shot: where the first beacon lands in clip space, so a beacon that draws only up close
        // can be told apart from one the far plane or an off-screen projection is rejecting.
        if (!loggedClipDiag_)
        {
            loggedClipDiag_ = true;
            int first = -1;
            for (int i = 0; i < trackedCount_; ++i)
            {
                if (beacons_[i].fade > 0.001f) { first = i; break; }
            }
            if (first < 0) first = 0;
            float eye[3];
            cam::GetPosition(eye);
            const float* view = cam::GetView();
            const float* proj = cam::GetProjection();
            const float px = beacons_[first].pos[0] - eye[0];
            const float py = beacons_[first].pos[1] - eye[1];
            const float pz = beacons_[first].pos[2] - eye[2];
            const float vx = px * view[0] + py * view[4] + pz * view[8] + view[12];
            const float vy = px * view[1] + py * view[5] + pz * view[9] + view[13];
            const float vz = px * view[2] + py * view[6] + pz * view[10] + view[14];
            const float cx = vx * proj[0] + vy * proj[4] + vz * proj[8] + proj[12];
            const float cy = vx * proj[1] + vy * proj[5] + vz * proj[9] + proj[13];
            const float cz = vx * proj[2] + vy * proj[6] + vz * proj[10] + proj[14];
            const float cw = vx * proj[3] + vy * proj[7] + vz * proj[11] + proj[15];
            const float dist = sqrtf(px * px + py * py + pz * pz);
            Log(WXL_LOG_INFO,
                "diag: beacon0=(%.0f,%.0f,%.0f) eye=(%.0f,%.0f,%.0f) dist=%.1f viewZ=%.1f "
                "ndc=(%.2f,%.2f,%.2f) w=%.1f projFar=%.0f",
                beacons_[first].pos[0], beacons_[first].pos[1], beacons_[first].pos[2], eye[0], eye[1],
                eye[2],
                dist, vz, cw != 0.0f ? cx / cw : 0.0f, cw != 0.0f ? cy / cw : 0.0f,
                cw != 0.0f ? cz / cw : 0.0f, cw,
                proj[10] > 0.0f && proj[10] < 1.0f ? -proj[14] / (proj[10] - 1.0f) : -1.0f);
        }

        const size_t queued = gfx::Pending();
        const long   result = gfx::Flush(dev, a.sceneDepth);

        if (!loggedFirstFlush_)
        {
            loggedFirstFlush_ = true;
            Log(WXL_LOG_INFO, "diag: first flush dev=%p depth=%p queued=%zu result=%ld",
                a.device, a.sceneDepth, queued, result);
        }
    }
}
