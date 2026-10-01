// wxl-loot-beam: the two update-field offsets the module reads, kept apart from the feature so they
// are the only place a client layout number lives.
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

#include <cstddef>
#include <cstdint>

// The client keeps every object's update fields in a block reachable from the object at
// kObjectDescriptorField. That block is the raw OBJECT_FIELD_* / UNIT_FIELD_* array of the 3.3.5a
// (12340) update-field contract, not a struct with a private header:
//
//   * the core's own ObjectBase reads the GUID from the block's first dword (offset 0), and
//   * wxl-modern-water reads the type mask at byte 0x08, which is OBJECT_FIELD_TYPE at dword 2.
//
// A unit field therefore sits at descriptor + index * 4. Only the two fields this module needs are
// named here; the module never includes offsets/ itself, so it stays on the SDK side of the boundary
// while still reading the live state the SDK does not model.
//
// Both reads are SEH-guarded at the call site. A wrong index reads a nearby heap dword rather than a
// field, which is why the dynamic-flags value is additionally checked against the bits the field can
// legitimately hold before it is trusted (see LootBeam.cpp).
namespace wxl_loot_beam
{
    /// Object -> its update-field block (CGObject_C::m_descriptors). The core's ObjectBase.header and
    /// wxl-modern-water's IsUnit both read this same pointer.
    constexpr size_t kObjectDescriptorField = 0x08;

    /// UNIT_FIELD_HEALTH: absolute field index 0x18 = OBJECT_END (6) + 0x12 -> byte 0x18 * 4 = 0x60.
    /// A living unit is nonzero here; a corpse reads 0. (Not +0x18 from OBJECT_END -- 0x18 is the
    /// absolute index, the delta is 0x12.)
    constexpr size_t kUnitHealthField = 0x60;

    /// UNIT_DYNAMIC_FLAGS: absolute field index 0x49 -> byte 0x124. Only used when the strict-lootable
    /// option is on; the module model is health-driven, and this offset is a best-effort reconstruction
    /// of the 3.3.5 layout rather than one verified in a live client, hence the bit validation below.
    constexpr size_t kUnitDynamicFlagsField = 0x124;

    /// UNIT_DYNFLAG_LOOTABLE, the bit the client sets while a corpse can still be looted.
    constexpr uint32_t kDynamicFlagLootable = 0x0001;

    /// The bits UNIT_DYNAMIC_FLAGS may carry. A dword outside this mask did not land on the field, so
    /// the strict test falls back to the health-only verdict instead of trusting it.
    constexpr uint32_t kDynamicFlagKnownMask = 0x0000FFFF;
}
