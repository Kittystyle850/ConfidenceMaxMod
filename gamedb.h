// gamedb.h - minimal reader for the game's in-memory "gamedb" (lists of named
// entries: loot tables, items, ...), ported from the CT's Lua helpers.
//
//   db            = *(u64*)(match+2)  where match = AOB for the path string
//                   "database\game\game.gamedb"   (registerDB_f1 in the CT)
//   list[i]       = *(db+0x50) + 0x58*i ; name = cstr(*(list+0x00))
//   list.count    = i32 @ list+0x28 ; list.entries = ptr @ list+0x38
//   entry[j]      = entries + 0x28*j    ; id = cstr(*(entry+0x20))
#pragma once
#include <windows.h>
#include <string>
#include "aob_scan.h"

inline bool SafeRead(uintptr_t addr, void* out, size_t n) {
    __try { memcpy(out, (const void*)addr, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template <typename T> inline bool Rd(uintptr_t a, T& v) { return SafeRead(a, &v, sizeof(T)); }

inline bool ReadCStr(uintptr_t p, std::string& s, size_t max = 160) {
    s.clear();
    for (size_t i = 0; i < max; ++i) {
        char c;
        if (!Rd(p + i, c)) return false;
        if (c == 0) return !s.empty();
        if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) return false;
        s.push_back(c);
    }
    return false;
}

inline uintptr_t DbListBase(uintptr_t db, const char* listName) {
    uintptr_t arr;
    if (!Rd(db + 0x50, arr) || arr < 0x10000) return 0;
    std::string s;
    for (int i = 0; i < 3000; ++i) {
        uintptr_t base = arr + 0x58 * (uintptr_t)i;
        uintptr_t pname;
        if (!Rd(base, pname) || pname < 0x10000) continue;
        if (ReadCStr(pname, s) && s == listName) return base;
    }
    return 0;
}

inline bool DbListEntries(uintptr_t list, uintptr_t& entries, int& count) {
    if (!Rd(list + 0x28, count) || count <= 0 || count > 200000) return false;
    if (!Rd(list + 0x38, entries) || entries < 0x10000) return false;
    return true;
}

inline uintptr_t DbEntryBase(uintptr_t list, const std::string& id) {
    uintptr_t entries; int count;
    if (!DbListEntries(list, entries, count)) return 0;
    std::string s;
    for (int i = 0; i < count; ++i) {
        uintptr_t e = entries + 0x28 * (uintptr_t)i;
        uintptr_t pname;
        if (Rd(e + 0x20, pname) && ReadCStr(pname, s) && s == id) return e;
    }
    return 0;
}

inline std::string DbDumpList(uintptr_t list) {
    std::string out;
    uintptr_t entries; int count;
    if (!DbListEntries(list, entries, count)) return out;
    std::string s;
    for (int i = 0; i < count; ++i) {
        uintptr_t pname;
        if (Rd(entries + 0x28 * (uintptr_t)i + 0x20, pname) && ReadCStr(pname, s))
            out += std::to_string(i) + "\t" + s + "\n";
    }
    return out;
}

// Finds the gamedb pointer (CT registerDB_f1) and only accepts a candidate
// whose list table really contains `mustHaveList`. Returns db, sets listBase.
inline uintptr_t FindGameDb(HMODULE mod, const char* mustHaveList, uintptr_t& listBase) {
    static const char* kPat =
        "00 00 ?? ?? ?? ?? ?? ?? 00 00 64 61 74 61 62 61 73 65 5C 67 61 6D 65 5C "
        "67 61 6D 65 2E 67 61 6D 65 64 62 00";
    uintptr_t foundDb = 0;
    FindPatternVerified(mod, kPat, /*execOnly=*/false, [&](uint8_t* hit) {
        uintptr_t db;
        if (!Rd((uintptr_t)hit + 2, db) || db < 0x10000) return false;
        uintptr_t lb = DbListBase(db, mustHaveList);
        if (!lb) return false;
        foundDb = db; listBase = lb;
        return true;
    });
    return foundDb;
}
