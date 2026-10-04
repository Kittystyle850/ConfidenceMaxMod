// ============================================================
//  move_inventory_unlocker.h  -  "be like the protagonist" unlock
//  ------------------------------------------------------------
//  Ported from the CT table's entry 330 ("Permanently Grant All
//  353 Existing Local Move Cards"). The game gates which moves you
//  can equip on ANY player through an ownership inventory - a
//  TMap<FString,int32> living at save+0xF98, where each of the 353
//  "mv_"/"smv_" keyed entries has an amount: 0 = locked, >0 = owned.
//  We don't touch individual players at all - we just flip every
//  locked entry to owned. After that, the game's own menus let you
//  equip any move on any of the 358 players, same as the protagonist.
// ============================================================
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include "save_pointer.h"

extern void WriteLog(const char* fmt, ...);

namespace MoveInventoryUnlocker {

using SavePointer::ReadQ;
using SavePointer::ReadD;

inline bool ReadFStringAscii(uintptr_t dataPtr, uint32_t count, std::string& out) {
    if (count < 2 || count > 0x400) return false;
    out.clear();
    out.reserve(count);
    for (uint32_t j = 0; j + 1 < count; ++j) {
        uintptr_t p = dataPtr + static_cast<uintptr_t>(j) * 2;
        if (IsBadReadPtr(reinterpret_cast<void*>(p), 2)) return false;
        uint16_t ch = *reinterpret_cast<volatile uint16_t*>(p);
        if (ch < 0x20 || ch > 0x7E) return false; // not plain ASCII, bail - wrong map
        out.push_back(static_cast<char>(ch));
    }
    return true;
}

inline bool WriteD(uintptr_t addr, uint32_t value) {
    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(addr), 4, PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    *reinterpret_cast<uint32_t*>(addr) = value;
    VirtualProtect(reinterpret_cast<LPVOID>(addr), 4, oldProt, &oldProt);
    return true;
}

struct Result {
    bool ok = false;
    int  totalKeys = 0;
    int  normalKeys = 0;
    int  superKeys = 0;
    int  alreadyOwned = 0;
    int  newlyUnlocked = 0;
    const char* failReason = nullptr;
};

// Walks the TMap, validates it's really the 206 mv_ + 147 smv_ ownership
// map (same check the CT table does) before touching anything, then sets
// every amount==0 entry to 1. Entries that aren't mv_/smv_ keyed are left
// completely alone.
inline Result UnlockAll(uintptr_t save) {
    Result r;
    uintptr_t map = save + 0xF98;

    uint64_t data = 0;
    uint32_t count = 0, capacity = 0;
    if (!ReadQ(map, data) || !ReadD(map + 0x08, count) || !ReadD(map + 0x0C, capacity)) {
        r.failReason = "could not read inventory TMap header"; return r;
    }
    if (!data || data % 8 != 0 || count == 0 || count > 0x2000 || capacity < count || capacity > 0x2000) {
        r.failReason = "inventory TMap header failed sanity bounds"; return r;
    }

    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t node = static_cast<uintptr_t>(data) + static_cast<uintptr_t>(i) * 0x20;
        uint64_t keyPtr = 0;
        uint32_t keyCount = 0, keyCap = 0;
        if (!ReadQ(node, keyPtr) || !ReadD(node + 0x08, keyCount) || !ReadD(node + 0x0C, keyCap)) {
            r.failReason = "unreadable inventory node"; return r;
        }
        if (!keyPtr || keyCount < 2 || keyCount > 0x400 || keyCap < keyCount) {
            r.failReason = "inventory FString header failed sanity bounds"; return r;
        }
        std::string key;
        if (!ReadFStringAscii(static_cast<uintptr_t>(keyPtr), keyCount, key)) {
            r.failReason = "inventory key is not plain ASCII - wrong map, aborting"; return r;
        }

        bool isMv  = key.rfind("mv_", 0) == 0;
        bool isSmv = key.rfind("smv_", 0) == 0;
        if (isMv) ++r.normalKeys;
        else if (isSmv) ++r.superKeys;
        else continue; // some other inventory entry entirely - don't touch it

        uint32_t amount = 0;
        if (!ReadD(node + 0x10, amount)) { r.failReason = "could not read amount field"; return r; }
        if (amount == 0) {
            if (WriteD(node + 0x10, 1)) ++r.newlyUnlocked;
        } else {
            ++r.alreadyOwned;
        }
    }

    r.totalKeys = r.normalKeys + r.superKeys;
    if (r.normalKeys != 206 || r.superKeys != 147) {
        r.failReason = "key counts didn't match the expected 206 mv_ + 147 smv_ (different game build?)";
        return r; // we already finished the loop, but flag as not-fully-trusted
    }
    r.ok = true;
    return r;
}

} // namespace MoveInventoryUnlocker
