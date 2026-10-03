// ============================================================
//  custom_player_editor.h  -  live Custom Player moveset editor
//  ------------------------------------------------------------
//  Ported from the CT table's entries 300 (Live Master Save Data
//  Pointer), 504 (Load into Stage) and 520 (Commit/Synchronize).
//  No AOB scan involved - everything here is a fixed RVA chain,
//  validated the same way the source Lua script validates it
//  (UObject flags + exact class-pointer match against two known
//  static class-singleton slots per object).
//
//  Pointer chain:
//    controller = *(module+0x97B04B0)                 "UGameSaveCtrl"
//    save       = *(controller+0x28)                   "UGameSaveData"
//    avatarCom  = *(module+0x97AC890)
//    editor     = *(avatarCom+0x298)                    [only if the
//                                                         custom-player
//                                                         menu is open]
//
//  Moveset struct (identical layout, written to up to 3 places so
//  the change shows immediately, not just after a reload):
//    +0x00 Dribble   +0x04 Tackle   +0x08 Shot   +0x0C AirShot
//    +0x10 Saving    +0x14 SuperType(byte)  +0x18 SuperID  +0x1C Miracle
//  save+0x1B4, avatarCom+0x1AC, editor+0x1AC all use this layout.
//
//  SuperType is still UNVERIFIED (see README) - we only accept it as
//  a raw 0-8 number from the config file, we don't claim to know
//  which English category each number corresponds to yet.
// ============================================================
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <string>
#include "movedb.h"

extern void WriteLog(const char* fmt, ...);

namespace CustomPlayerEditor {

// RVAs lifted verbatim from the CT table's Lua script.
static constexpr uintptr_t RVA_CONTROLLER_GLOBAL = 0x97B04B0;
static constexpr uintptr_t RVA_CONTROLLER_CLASS_A = 0x9796280;
static constexpr uintptr_t RVA_CONTROLLER_CLASS_B = 0x9796288;
static constexpr uintptr_t RVA_SAVE_CLASS_A       = 0x9796250;
static constexpr uintptr_t RVA_SAVE_CLASS_B       = 0x9796258;
static constexpr uintptr_t RVA_AVATARCOM_GLOBAL   = 0x97AC890;
static constexpr uintptr_t RVA_AVATARCOM_CLASS    = 0x978C020;
static constexpr uintptr_t RVA_EDITOR_CLASS       = 0x978C008;

static constexpr uintptr_t OFF_SAVE_VERSION   = 0x28;  // on `save`, must be 102
static constexpr uintptr_t OFF_SAVE_MOVESET   = 0x1B4; // on `save`
static constexpr uintptr_t OFF_CTRL_SAVEPTR   = 0x28;  // on `controller`, -> save
static constexpr uintptr_t OFF_AVATARCOM_MOVESET = 0x1AC;
static constexpr uintptr_t OFF_AVATARCOM_EDITOR  = 0x298; // on avatarCom, -> editor
static constexpr uintptr_t OFF_EDITOR_MOVESET    = 0x1AC;
static constexpr uint32_t  OBJFLAGS_INVALID_MASK = 0x40018000; // pending-kill / garbage

struct Moveset {
    int32_t dribble;
    int32_t tackle;
    int32_t shot;
    int32_t airshot;
    int32_t saving;
    uint8_t superType;
    uint8_t _pad[3];
    int32_t superID;
    int32_t miracle;
};
static_assert(sizeof(Moveset) == 0x20, "Moveset layout must match the live struct");

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

// UObject sanity check: 8-byte aligned, flags don't have the
// pending-kill/garbage bits set, and ClassPrivate (at +0x10) matches
// one of the two expected static class-singleton slots.
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

// Resolves the full chain fresh every call - cheap (a handful of
// reads), and avoids caching a stale pointer across save/menu
// transitions, which is exactly what the CT's 250ms poll guarded
// against.
struct ResolvedChain {
    bool      ok = false;
    uintptr_t controller = 0;
    uintptr_t save = 0;
    uintptr_t avatarCom = 0;
    uintptr_t editor = 0; // 0 if the avatar edit menu isn't open
};

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

    // AvatarCom/editor are optional extras: if the custom-player menu
    // isn't open these legitimately won't resolve, that's not a failure
    // of the main chain.
    uint64_t avatarClass = 0, editorClass = 0;
    if (ReadQ(base + RVA_AVATARCOM_CLASS, avatarClass) && avatarClass &&
        ReadQ(base + RVA_EDITOR_CLASS, editorClass) && editorClass) {
        uint64_t avatarCom = 0;
        if (ReadQ(base + RVA_AVATARCOM_GLOBAL, avatarCom) && avatarCom) {
            uint64_t classPtr = 0;
            if (ReadQ(static_cast<uintptr_t>(avatarCom) + 0x10, classPtr) && classPtr == avatarClass) {
                r.avatarCom = static_cast<uintptr_t>(avatarCom);
                uint64_t editor = 0;
                if (ReadQ(r.avatarCom + OFF_AVATARCOM_EDITOR, editor) && editor) {
                    uint64_t eClassPtr = 0;
                    if (ReadQ(static_cast<uintptr_t>(editor) + 0x10, eClassPtr) && eClassPtr == editorClass) {
                        r.editor = static_cast<uintptr_t>(editor);
                    }
                }
            }
        }
    }
    return r;
}

inline bool ReadMoveset(uintptr_t objAddr, uintptr_t fieldOffset, Moveset& out) {
    uintptr_t p = objAddr + fieldOffset;
    if (IsBadReadPtr(reinterpret_cast<void*>(p), sizeof(Moveset))) return false;
    memcpy(&out, reinterpret_cast<void*>(p), sizeof(Moveset));
    return true;
}

inline bool WriteMoveset(uintptr_t objAddr, uintptr_t fieldOffset, const Moveset& m) {
    uintptr_t p = objAddr + fieldOffset;
    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(p), sizeof(Moveset), PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    memcpy(reinterpret_cast<void*>(p), &m, sizeof(Moveset));
    VirtualProtect(reinterpret_cast<LPVOID>(p), sizeof(Moveset), oldProt, &oldProt);
    return true;
}

// ------------------------------------------------------------
//  Name -> ID lookup (case-insensitive exact match)
// ------------------------------------------------------------
inline std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

struct NameLookup {
    std::unordered_map<std::string, int> byName;
    void Build(const MoveEntry* entries, int count) {
        for (int i = 0; i < count; ++i) byName[Lower(entries[i].name)] = entries[i].id;
    }
    bool Find(const std::string& name, int& outId) const {
        auto it = byName.find(Lower(name));
        if (it == byName.end()) return false;
        outId = it->second;
        return true;
    }
};

struct Databases {
    NameLookup dribble, tackle, shot, airshot, saving, miracle;
    void BuildAll() {
        dribble.Build(g_dribbleMoves, g_dribbleMoves_count);
        tackle.Build(g_tackleMoves, g_tackleMoves_count);
        shot.Build(g_shotMoves, g_shotMoves_count);
        airshot.Build(g_airshotMoves, g_airshotMoves_count);
        saving.Build(g_savingMoves, g_savingMoves_count);
        miracle.Build(g_miracleMoves, g_miracleMoves_count);
    }
};
inline Databases g_db;

// ------------------------------------------------------------
//  Reference lists - written once at startup so the player has
//  real move names to copy from, generated straight from the
//  same embedded tables the lookup uses (always in sync).
// ------------------------------------------------------------
// Single consolidated reference file instead of 7 separate ones - same
// content, far less clutter in the game folder.
inline void WriteSection(FILE* f, const char* title, const MoveEntry* entries, int count) {
    fprintf(f, "\n== %s (%d) ==\n", title, count);
    for (int i = 0; i < count; ++i) fprintf(f, "%s\n", entries[i].name);
}

inline void WriteAllRefFiles() {
    FILE* f = nullptr;
    if (fopen_s(&f, "CT2_MoveList_Reference.txt", "w") != 0 || !f) return;
    fprintf(f, "CT2 move name reference - copy exact names into CT2_CustomPlayerMoves.txt\n");
    WriteSection(f, "Dribble", g_dribbleMoves, g_dribbleMoves_count);
    WriteSection(f, "Tackle", g_tackleMoves, g_tackleMoves_count);
    WriteSection(f, "Shot", g_shotMoves, g_shotMoves_count);
    WriteSection(f, "AirShot", g_airshotMoves, g_airshotMoves_count);
    WriteSection(f, "Saving", g_savingMoves, g_savingMoves_count);
    WriteSection(f, "MiracleSaving", g_miracleMoves, g_miracleMoves_count);

    fprintf(f, "\n== Super (%d) == name | catalog type | ID\n", g_superMoves_count);
    fprintf(f, "# SuperType byte (0-8) not yet confirmed to match these type strings.\n");
    for (int i = 0; i < g_superMoves_count; ++i) {
        const auto& e = g_superMoves[i];
        fprintf(f, "%-32s | %-18s | ID %d\n", e.name, e.type, e.id);
    }
    fclose(f);
}

// ------------------------------------------------------------
//  Config file: simple "Key=Value" text, re-read and (re-)applied
//  whenever it changes.
// ------------------------------------------------------------
static constexpr const char* CONFIG_PATH = "CT2_CustomPlayerMoves.txt";

inline void WriteDefaultConfigIfMissing() {
    FILE* f = nullptr;
    if (fopen_s(&f, CONFIG_PATH, "r") == 0 && f) { fclose(f); return; } // already exists
    if (fopen_s(&f, CONFIG_PATH, "w") != 0 || !f) return;
    fprintf(f,
        "# CT2 Custom Player live editor.\n"
        "# Edit the values below with names copied EXACTLY from the\n"
        "# CT2_MoveList_*.txt reference files, then save this file.\n"
        "# Picked up automatically within a couple seconds, no restart needed.\n"
        "#\n"
        "# Super Move: SuperType is a raw number 0-8 (mapping not yet\n"
        "# confirmed - see CT2_MoveList_Super.txt and the README).\n"
        "# Leave a line as-is / empty to not touch that field.\n"
        "\n"
        "Dribble=\n"
        "Tackle=\n"
        "Shot=\n"
        "AirShot=\n"
        "Saving=\n"
        "MiracleSaving=\n"
        "SuperType=\n"
        "SuperID=\n"
    );
    fclose(f);
}

struct ConfigValues {
    std::string dribble, tackle, shot, airshot, saving, miracle;
    std::string superType, superID;
};

inline bool ReadConfig(ConfigValues& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, CONFIG_PATH, "r") != 0 || !f) return false;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty() || s[0] == '#') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = s.substr(0, eq);
        std::string val = s.substr(eq + 1);
        if (key == "Dribble") out.dribble = val;
        else if (key == "Tackle") out.tackle = val;
        else if (key == "Shot") out.shot = val;
        else if (key == "AirShot") out.airshot = val;
        else if (key == "Saving") out.saving = val;
        else if (key == "MiracleSaving") out.miracle = val;
        else if (key == "SuperType") out.superType = val;
        else if (key == "SuperID") out.superID = val;
    }
    fclose(f);
    return true;
}

// Applies the config on top of whatever is CURRENTLY equipped
// (reads live first, only overwrites fields the config specifies),
// then writes to save+/avatarCom+/editor+ so it shows immediately.
inline bool ApplyConfig() {
    ResolvedChain chain = Resolve();
    if (!chain.ok) { WriteLog("[!] CustomPlayerEditor: save pointer chain not ready (open a menu or match first)"); return false; }

    Moveset m{};
    if (!ReadMoveset(chain.save, OFF_SAVE_MOVESET, m)) {
        WriteLog("[!] CustomPlayerEditor: failed to read current moveset");
        return false;
    }

    ConfigValues cfg;
    if (!ReadConfig(cfg)) { WriteLog("[!] CustomPlayerEditor: could not read %s", CONFIG_PATH); return false; }

    bool changed = false;
    int id;
    if (!cfg.dribble.empty() && g_db.dribble.Find(cfg.dribble, id)) { m.dribble = id; changed = true; }
    else if (!cfg.dribble.empty()) WriteLog("[!] Unknown Dribble name: '%s'", cfg.dribble.c_str());

    if (!cfg.tackle.empty() && g_db.tackle.Find(cfg.tackle, id)) { m.tackle = id; changed = true; }
    else if (!cfg.tackle.empty()) WriteLog("[!] Unknown Tackle name: '%s'", cfg.tackle.c_str());

    if (!cfg.shot.empty() && g_db.shot.Find(cfg.shot, id)) { m.shot = id; changed = true; }
    else if (!cfg.shot.empty()) WriteLog("[!] Unknown Shot name: '%s'", cfg.shot.c_str());

    if (!cfg.airshot.empty() && g_db.airshot.Find(cfg.airshot, id)) { m.airshot = id; changed = true; }
    else if (!cfg.airshot.empty()) WriteLog("[!] Unknown AirShot name: '%s'", cfg.airshot.c_str());

    if (!cfg.saving.empty() && g_db.saving.Find(cfg.saving, id)) { m.saving = id; changed = true; }
    else if (!cfg.saving.empty()) WriteLog("[!] Unknown Saving name: '%s'", cfg.saving.c_str());

    if (!cfg.miracle.empty() && g_db.miracle.Find(cfg.miracle, id)) { m.miracle = id; changed = true; }
    else if (!cfg.miracle.empty()) WriteLog("[!] Unknown MiracleSaving name: '%s'", cfg.miracle.c_str());

    if (!cfg.superType.empty()) {
        int v = atoi(cfg.superType.c_str());
        if (v >= 0 && v <= 8) { m.superType = static_cast<uint8_t>(v); changed = true; }
        else WriteLog("[!] SuperType out of range (0-8): '%s'", cfg.superType.c_str());
    }
    if (!cfg.superID.empty()) { m.superID = atoi(cfg.superID.c_str()); changed = true; }

    if (!changed) return true; // nothing to do, not an error

    bool ok = WriteMoveset(chain.save, OFF_SAVE_MOVESET, m);
    int syncCount = ok ? 1 : 0;
    if (chain.avatarCom) { if (WriteMoveset(chain.avatarCom, OFF_AVATARCOM_MOVESET, m)) ++syncCount; }
    if (chain.editor)    { if (WriteMoveset(chain.editor, OFF_EDITOR_MOVESET, m)) ++syncCount; }

    WriteLog(ok ? "[+] CustomPlayerEditor: applied (%d location%s synced)"
                : "[!] CustomPlayerEditor: write to save object FAILED",
             syncCount, syncCount == 1 ? "" : "s");
    return ok;
}

} // namespace CustomPlayerEditor
