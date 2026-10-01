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

            uintptr_t descriptors = 0;
            if (!ReadPtr(reinterpret_cast<uintptr_t>(unit) + kObjectDescriptorField, descriptors))
                return false;
            const size_t needed = style.requireLootable ? kUnitDynamicFlagsField + sizeof(uint32_t)
                                                        : kUnitHealthField + sizeof(uint32_t);
            if (!ValidPointer(descriptors, needed))
                return false;

            uint32_t health = 0;
            if (!ReadU32(descriptors + kUnitHealthField, health)) return false;
            if (health != 0) return false;

            if (style.requireLootable)
            {
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
                   a.color[0] == b.color[0] && a.color[1] == b.color[1] && a.color[2] == b.color[2] &&
                   a.groundAlpha == b.groundAlpha && a.ringAlpha == b.ringAlpha &&
                   a.beamAlpha == b.beamAlpha && a.pulse == b.pulse && a.pulseSpeed == b.pulseSpeed &&
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
    }

    void LootBeam::LoadConfigNow()
    {
        BeamStyle s = BeamStyle{};
        s.enabled        = ReadBool(iniPath_,  "Enabled",       s.enabled);
        s.height         = ReadFloat(iniPath_, "Height",        s.height,        0.0f, 40.0f);
        s.groundRadius   = ReadFloat(iniPath_, "GroundRadius",  s.groundRadius,  0.2f, 6.0f);
        s.beamWidth      = ReadFloat(iniPath_, "BeamWidth",     s.beamWidth,     0.05f, 2.0f);
        s.groundAlpha    = ReadFloat(iniPath_, "GroundAlpha",   s.groundAlpha,   0.0f, 1.0f);
        s.ringAlpha      = ReadFloat(iniPath_, "RingAlpha",     s.ringAlpha,     0.0f, 1.0f);
        s.beamAlpha      = ReadFloat(iniPath_, "BeamAlpha",     s.beamAlpha,     0.0f, 1.0f);
        s.pulse          = ReadFloat(iniPath_, "Pulse",         s.pulse,         0.0f, 1.0f);
        s.pulseSpeed     = ReadFloat(iniPath_, "PulseSpeed",    s.pulseSpeed,    0.0f, 6.0f);
        s.maxDistance    = ReadFloat(iniPath_, "MaxDistance",   s.maxDistance,   0.0f, 400.0f);
        s.showGround     = ReadBool(iniPath_,  "ShowGround",    s.showGround);
        s.showBeam       = ReadBool(iniPath_,  "ShowBeam",      s.showBeam);
        s.throughWalls   = ReadBool(iniPath_,  "ThroughWalls",  s.throughWalls);
        s.requireLootable= ReadBool(iniPath_,  "RequireLootable", s.requireLootable);
        ReadColor(iniPath_, "Color", s.color);

        style_ = s;
        saved_ = s;
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
        WriteFloat(iniPath_, "GroundAlpha",     style_.groundAlpha);
        WriteFloat(iniPath_, "RingAlpha",       style_.ringAlpha);
        WriteFloat(iniPath_, "BeamAlpha",       style_.beamAlpha);
        WriteFloat(iniPath_, "Pulse",           style_.pulse);
        WriteFloat(iniPath_, "PulseSpeed",      style_.pulseSpeed);
        WriteFloat(iniPath_, "MaxDistance",     style_.maxDistance);
        WriteInt(iniPath_,   "ShowGround",      style_.showGround ? 1 : 0);
        WriteInt(iniPath_,   "ShowBeam",        style_.showBeam ? 1 : 0);
        WriteInt(iniPath_,   "ThroughWalls",    style_.throughWalls ? 1 : 0);
        WriteInt(iniPath_,   "RequireLootable", style_.requireLootable ? 1 : 0);
        WriteColor(iniPath_, "Color",           style_.color);

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
                dumped = true;
                DumpUnit(obj, guid);
            }
            if (beaconCount_ >= kMaxBeacons) return false;
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

            beacons_[beaconCount_].pos[0] = p[0];
            beacons_[beaconCount_].pos[1] = p[1];
            beacons_[beaconCount_].pos[2] = p[2];
            ++beaconCount_;
            return true;
        });

        return enumerated;
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
        // Health lives at 0x60; the strict path's dynamic flags at 0x124. Dump both neighbourhoods so
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
        for (size_t off = 0x11C; off <= 0x12C && n < int(sizeof(window)) - 24; off += 4)
        {
            uint32_t v = 0;
            if (!ReadU32(descriptors + off, v))
                v = 0xDEADBEEFu;
            n += std::snprintf(window + n, sizeof(window) - n, " %02X=%u", unsigned(off), v);
        }
        Log(WXL_LOG_INFO, "diag: first unit guid=%llX dynflags%s", guid, window);
    }

    void LootBeam::QueueBeacon(const float pos[3], float pulseScale)
    {
        const gfx::Depth depth = style_.throughWalls ? gfx::Depth::Through : gfx::Depth::Tested;

        float groundZ = pos[2];
        if (!world::GroundZ(pos[0], pos[1], pos[2], groundZ))
            groundZ = pos[2];

        if (style_.showGround)
        {
            gfx::GroundDisc(pos, style_.groundRadius,
                            Pack(style_.groundAlpha * pulseScale, style_.color), 28, 2, depth);
            gfx::GroundRing(pos, style_.groundRadius * 1.08f,
                            Pack(style_.ringAlpha * pulseScale, style_.color), 40, depth);
        }

        if (!style_.showBeam || style_.height <= 0.01f)
            return;

        // Billboard toward the camera: a tube built from two crossed, camera-facing planes reads as a
        // volume from any viewing angle without meshing an actual cylinder.
        float camera[3];
        cam::GetPosition(camera);
        const float dx = camera[0] - pos[0];
        const float dy = camera[1] - pos[1];
        const float len = sqrtf(dx * dx + dy * dy);
        float fx = 1.0f, fy = 0.0f;
        if (len > 1e-3f) { fx = dx / len; fy = dy / len; }
        const float sx = -fy, sy = fx;

        const float baseZ = groundZ + 0.05f;
        const float topZ  = groundZ + style_.height;
        constexpr int kRings = 10;

        for (int plane = 0; plane < 2; ++plane)
        {
            const float ax = (plane == 0) ? sx : fx;
            const float ay = (plane == 0) ? sy : fy;

            for (int r = 0; r < kRings; ++r)
            {
                const float t0 = float(r) / float(kRings);
                const float t1 = float(r + 1) / float(kRings);
                const float z0 = baseZ + (topZ - baseZ) * t0;
                const float z1 = baseZ + (topZ - baseZ) * t1;
                const float w0 = style_.beamWidth * (1.0f - 0.65f * t0);
                const float w1 = style_.beamWidth * (1.0f - 0.65f * t1);
                const float a0 = style_.beamAlpha * (1.0f - t0) * pulseScale;
                const float a1 = style_.beamAlpha * (1.0f - t1) * pulseScale;

                const float p0[3] = { pos[0] - ax * w0, pos[1] - ay * w0, z0 };
                const float p1[3] = { pos[0] + ax * w0, pos[1] + ay * w0, z0 };
                const float p2[3] = { pos[0] + ax * w1, pos[1] + ay * w1, z1 };
                const float p3[3] = { pos[0] - ax * w1, pos[1] - ay * w1, z1 };

                const gfx::Color color = Pack((a0 + a1) * 0.5f, style_.color);
                gfx::Triangle(p0, p1, p2, color, depth);
                gfx::Triangle(p0, p2, p3, color, depth);
            }
        }
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
            beaconCount_ = 0;
            return;
        }

        const int enumerated = ScanUnits();
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
        // alpha and a pulse of 0 is perfectly steady.
        const float pulseScale = 1.0f - 0.5f * style_.pulse * (1.0f - sinf(phase_));
        for (int i = 0; i < beaconCount_; ++i)
            QueueBeacon(beacons_[i].pos, pulseScale);
    }

    void LootBeam::OnWorldSceneEnd(const ev::WorldSceneEndArgs& a)
    {
        if (!style_.enabled || !inWorld_ || beaconCount_ == 0)
        {
            gfx::Clear();
            return;
        }

        gx::Device9 dev(a.device);
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
