// ============================================================
//  save_pointer.h  -  resolves UGameSaveCtrl / UGameSaveData
//  ------------------------------------------------------------
//  Ported from the CT table's entry 300 ("Live Master Save Data
//  Pointer"). Fixed RVAs, no AOB scan, validated via UObject
//  flags + class-pointer match. Shared by anything that needs to
//  reach the live save object (currently: move_inventory_unlocker.h).
// ============================================================
#pragma once
#include <windows.h>
#include <cstdint>

namespace SavePointer {

static constexpr uintptr_t RVA_CONTROLLER_GLOBAL  = 0x97B04B0;
static constexpr uintptr_t RVA_CONTROLLER_CLASS_A = 0x9796280;
static constexpr uintptr_t RVA_CONTROLLER_CLASS_B = 0x9796288;
static constexpr uintptr_t RVA_SAVE_CLASS_A       = 0x9796250;
static constexpr uintptr_t RVA_SAVE_CLASS_B       = 0x9796258;

static constexpr uintptr_t OFF_SAVE_VERSION = 0x28; // on `save`, must be 102
static constexpr uintptr_t OFF_CTRL_SAVEPTR = 0x28; // on `controller`, -> save
static constexpr uint32_t  OBJFLAGS_INVALID_MASK = 0x40018000; // pending-kill / garbage

inline uintptr_t ModuleBase() {
    return reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
}

inline bool ReadQ(uintptr_t addr, uint64_t& out) {
    return !IsBadReadPtr(reinterpret_cast<void*>(addr), 8) &&
           (out = *reinterpret_cast<volatile uint64_t*>(addr), true);
}
inline bool ReadD(uintptr_t addr, uint32_t& out) {
    return !IsBadReadPtr(reinterpret_cast<void*>(addr), 4) &&
           (out = *reinterpret_cast<volatile uint32_t*>(addr), true);
}

inline bool ValidObject(uintptr_t obj, uintptr_t classSlotA, uintptr_t classSlotB) {
    if (!obj || (obj & 7) != 0) return false;
    uint32_t flags = 0;
    if (!ReadD(obj + 0x08, flags)) return false;
    if (flags & OBJFLAGS_INVALID_MASK) return false;
    uint64_t classPtr = 0;
    if (!ReadQ(obj + 0x10, classPtr) || classPtr == 0) return false;

    uint64_t expectedA = 0, expectedB = 0;
    bool haveA = classSlotA && ReadQ(classSlotA, expectedA) && expectedA != 0;
    bool haveB = classSlotB && ReadQ(classSlotB, expectedB) && expectedB != 0;
    if (!haveA && !haveB) return false;
    return (haveA && classPtr == expectedA) || (haveB && classPtr == expectedB);
}

struct ResolvedChain {
    bool      ok = false;
    uintptr_t controller = 0;
    uintptr_t save = 0;
};

// Resolved fresh every call - cheap (a handful of reads), avoids caching
// a stale pointer across save/menu transitions.
inline ResolvedChain Resolve() {
    ResolvedChain r;
    uintptr_t base = ModuleBase();
    if (!base) return r;

    uint64_t controller = 0;
    if (!ReadQ(base + RVA_CONTROLLER_GLOBAL, controller)) return r;
    if (!ValidObject(static_cast<uintptr_t>(controller), base + RVA_CONTROLLER_CLASS_A, base + RVA_CONTROLLER_CLASS_B)) return r;

    uint64_t save = 0;
    if (!ReadQ(static_cast<uintptr_t>(controller) + OFF_CTRL_SAVEPTR, save)) return r;
    if (!ValidObject(static_cast<uintptr_t>(save), base + RVA_SAVE_CLASS_A, base + RVA_SAVE_CLASS_B)) return r;

    uint32_t version = 0;
    if (!ReadD(static_cast<uintptr_t>(save) + OFF_SAVE_VERSION, version) || version != 102) return r;

    r.controller = static_cast<uintptr_t>(controller);
    r.save = static_cast<uintptr_t>(save);
    r.ok = true;
    return r;
}

} // namespace SavePointer
