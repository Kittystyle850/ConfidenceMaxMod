// unique_limit_patch.h
//
// Ports CT2 CheatEntry ID 521 ("In-Game Unique Technique Limit: 2 to 4
// (all four normal slots) - OFFLINE ONLY") from the Crack.CT / Steam.CT
// tables to a native patch, using the exact same AOB patterns and byte
// edits the CT script uses. Four sites are patched:
//
//   1) ct2_unique_count_aob + 0x8  : 1 byte,  02 -> 04
//        the hard "02" immediate the UI uses when building the equip-count
//        comparison (UBasicInfoMoveListForAvatarWidget's unique-slot counter)
//   2) ct2_unique_max_aob   + 0xA  : 9 bytes, replaces the max-slots calc
//        45 8B C6 48 8B CE 45 03 C0  ->  45 6B C6 04 48 8B CE 66 90
//        (changes an imul-by-1 pattern effectively scaling the slot cap to 4
//         and NOPs the trailing add, matching the CT table's ENABLE bytes)
//   3) ct2_unique_select_guard_aob + 0x2 : 1 byte, 02 -> 04
//        the "can I still pick another unique move" comparison guard
//   4) ct2_unique_confirm_guard_aob + 0x2 : 1 byte, 02 -> 04
//        the confirm/warning-dialog threshold guard
//
// Each site is located by AOB scan (not a fixed address), so this keeps
// working across game patches as long as the surrounding instruction
// sequence is unchanged. Each site is also byte-verified (assert-style)
// immediately before patching; if verification fails the site is skipped
// and logged, rather than blindly overwriting whatever happens to be there.

#pragma once
#include <windows.h>
#include <string>
#include "aob_scan.h"

struct UniqueLimitPatchResult {
    bool countPatched = false;
    bool maxPatched = false;
    bool selectGuardPatched = false;
    bool confirmGuardPatched = false;
    std::string log;
};

inline bool ApplySite(HMODULE mod, const char* label, const char* pattern,
                       size_t patchOffset,
                       std::initializer_list<uint8_t> expectedOriginal,
                       std::initializer_list<uint8_t> newBytes,
                       std::string& log) {
    uint8_t* hit = FindPatternInModule(mod, pattern);
    if (!hit) {
        log += std::string("[-] ") + label + ": AOB not found (game version mismatch or already patched)\n";
        return false;
    }
    uint8_t* patchAddr = hit + patchOffset;
    if (!VerifyBytes(patchAddr, expectedOriginal)) {
        log += std::string("[-] ") + label + ": found AOB but bytes at patch offset don't match expected original - skipped for safety\n";
        return false;
    }
    if (!PatchBytes(patchAddr, newBytes)) {
        log += std::string("[-] ") + label + ": VirtualProtect/write failed\n";
        return false;
    }
    char buf[256];
    wsprintfA(buf, "[+] %s: patched @ module+0x%p\n", label, (void*)((uint8_t*)patchAddr - (uint8_t*)mod));
    log += buf;
    return true;
}

// Applies all four sites. Safe to call once; re-calling after a successful
// patch will fail the byte-verification step on each site (bytes no longer
// match "original") and simply no-op, so this can be retried if the module
// wasn't fully ready on the first attempt without risk of double-patching.
inline UniqueLimitPatchResult ApplyUniqueTechniqueLimitPatch(HMODULE mod) {
    UniqueLimitPatchResult r;

    r.countPatched = ApplySite(
        mod, "unique_count",
        "03 D3 03 D7 03 D6 41 B8 02 00 00 00 41 3B D0 41 0F 47 D0 41 89 94 24 04 05 00 00",
        0x8,
        { 0x02 },
        { 0x04 },
        r.log);

    r.maxPatched = ApplySite(
        mod, "unique_max",
        "8B 96 04 05 00 00 EB 02 33 D2 45 8B C6 48 8B CE 45 03 C0 48 8B 74 24 48",
        0xA,
        { 0x45, 0x8B, 0xC6, 0x48, 0x8B, 0xCE, 0x45, 0x03, 0xC0 },
        { 0x45, 0x6B, 0xC6, 0x04, 0x48, 0x8B, 0xCE, 0x66, 0x90 },
        r.log);

    r.selectGuardPatched = ApplySite(
        mod, "unique_select_guard",
        "83 FF 02 7C 58 48 8D 55 B7 48 8B CB E8 ?? ?? ?? ??",
        0x2,
        { 0x02 },
        { 0x04 },
        r.log);

    r.confirmGuardPatched = ApplySite(
        mod, "unique_confirm_guard",
        "83 FB 02 7C 3D 48 8D 15 ?? ?? ?? ??",
        0x2,
        { 0x02 },
        { 0x04 },
        r.log);

    return r;
}
