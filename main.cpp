// ============================================================
//  CT2 "All Moves For Everyone" research build
//  ------------------------------------------------------------
//  Single purpose: find and build the hook that lets every one of
//  the 358 players equip any move in the game, not just the custom
//  player. This is a SEPARATE mod from the CHAIN MAX / Unique
//  Technique Limit one - this file carries no gameplay hooks yet,
//  only the investigation tools that get us there, plus the one
//  prerequisite we've already confirmed (move-card ownership, which
//  turned out necessary-but-not-sufficient on its own).
//
//  Fully automatic, no keys needed for the main workflow: every ~10s
//  the DLL re-locates Izawa (a regular player) AND the Custom Player's
//  live moveset, and watches BOTH at once. Open the move-equip menu
//  for each and compare: same RIP on both = data-driven restriction;
//  different RIP = a real code branch worth hooking.
//
//  Hotkeys (manual overrides / diagnostics, rarely needed now):
//    F8  - fingerprint scan report (no watching).
//    F7  - array dump + pointer chase diagnostic.
//    F6  - reset both watches and start collecting fresh.
//    F9  - unload the DLL.
//    ESC - stop the watcher thread (anything already armed stays armed).
//
//  Log: CT2CheatDLL_log.txt (next to the game EXE)
// ============================================================
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

void WriteLog(const char* fmt, ...);

#include "save_pointer.h"
#include "move_inventory_unlocker.h"
#include "player_scanner.h"
#include "write_trap.h"
#include "gui.h"

static constexpr const char* TRAINER_NAME = "CT2 AllMoves (research build)";
static constexpr const char* TRAINER_VER  = "0.4";

// Confirmed stable from the first WriteTrap capture: a record-copy
// routine (0x28-byte struct, matches our confirmed layout exactly)
// living at this fixed RVA. Dumped wide and unconditionally at startup
// now - no need to catch it via the trap again, it's not heap-based.
static constexpr uintptr_t RVA_RECORD_COPY_FN = 0x48E3870;

// Second confirmed-stable find: a comparison inside what looks like a
// linear-search loop (cmp [rcx+rdx],r11d ; je found), caught watching
// Izawa. Candidate for the per-character move-allow-list check.
static constexpr uintptr_t RVA_IZAWA_COMPARE_FN = 0x4A9CFD9;

static void DumpFunctionAt(uintptr_t base, uintptr_t rva, const char* label,
                            const char* outPath, int windowBack, int windowFwd) {
    if (!base) return;
    uintptr_t addr = base + rva;
    FILE* f = nullptr;
    if (fopen_s(&f, outPath, "w") != 0 || !f) return;
    fprintf(f, "%s, module RVA 0x%llX (confirmed stable, caught via WriteTrap)\n", label, (unsigned long long)rva);
    fprintf(f, "16 bytes per row, <HERE> marks the exact instruction the trap caught.\n\n");
    for (int rowStart = -windowBack; rowStart <= windowFwd; rowStart += 16) {
        fprintf(f, "%+5d: ", rowStart);
        for (int i = 0; i < 16 && rowStart + i <= windowFwd; ++i) {
            int off = rowStart + i;
            uint8_t b = 0;
            uintptr_t p = addr + off;
            if (!IsBadReadPtr(reinterpret_cast<void*>(p), 1)) b = *reinterpret_cast<volatile uint8_t*>(p);
            fprintf(f, "%02X%s", b, off == 0 ? "*" : " ");
        }
        fprintf(f, "\n");
    }
    fclose(f);
}

static HMODULE g_hSelf = nullptr;

// ============================================================
//  Log
// ============================================================
void WriteLog(const char* fmt, ...) {
    FILE* f = nullptr;
    if (fopen_s(&f, "CT2CheatDLL_log.txt", "a") != 0 || !f) return;
    std::time_t t = std::time(nullptr);
    std::tm tmBuf{};
    localtime_s(&tmBuf, &t);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmBuf);
    std::fprintf(f, "[%s] ", ts);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);
    std::fputc('\n', f);
    std::fclose(f);
}

// ============================================================
//  Thread: no gameplay hooks installed yet (nothing confirmed
//  enough to hook) - just the investigation tools, plus the
//  inventory-unlock prerequisite applied automatically once.
// ============================================================
static DWORD WINAPI MainThread(LPVOID) {
    uintptr_t base = SavePointer::ModuleBase();
    WriteLog("[i] %s v%s started. base=0x%llX", TRAINER_NAME, TRAINER_VER, (unsigned long long)base);

    DumpFunctionAt(base, RVA_RECORD_COPY_FN, "Record-copy routine",
                   "CT2_InterestingFunction_Dump.txt", 900, 128);
    DumpFunctionAt(base, RVA_IZAWA_COMPARE_FN, "Izawa linear-search compare",
                   "CT2_IzawaCompare_Dump.txt", 600, 400);
    WriteLog("[i] Dumped both confirmed-stable functions to CT2_InterestingFunction_Dump.txt "
             "and CT2_IzawaCompare_Dump.txt");

    Gui::Launch();
    WriteLog("[i] GUI window launched.");
    WriteLog("[i] No gameplay hooks installed yet. Fully automatic: every ~10s this watches BOTH "
             "a regular player (Izawa) and your Custom Player's live moveset at once. Open the "
             "move-equip menu on each and compare the RVAs in CT2_WriteTrap_Report.txt. "
             "F6=reset both watches, F7/F8=manual diagnostics, F9=unload.");

    bool unlockDone = false;
    int unlockAttempts = 0;
    bool lastF9 = false, lastF8 = false, lastF7 = false, lastF6 = false;
    int pollTick = 0;
    uintptr_t lastAutoAnchor = 0;
    uintptr_t lastCustomAnchor = 0;

    while (true) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { WriteLog("[i] ESC, stopping watcher thread."); break; }

        bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9 && !lastF9) { WriteLog("[i] F9, unloading DLL."); FreeLibraryAndExitThread(g_hSelf, 0); }
        lastF9 = f9;

        bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (f8 && !lastF8) PlayerScanner::RunScanAndReport();
        lastF8 = f8;

        bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (f7 && !lastF7) PlayerScanner::RunArrayDump();
        lastF7 = f7;

        bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (f6 && !lastF6) {
            WriteTrap::ResetLabel("Izawa");
            WriteTrap::ResetLabel("CustomPlayer");
            WriteLog("[i] WriteTrap: both watches reset, will re-arm on the next automatic pass.");
        }
        lastF6 = f6;

        // Fully automatic, TWO watches at once: a regular player (Izawa)
        // and the Custom Player's own live moveset (save+0x1B4, reached
        // the same way the old custom-player editor did). If opening the
        // move menu hits the SAME RIP for both, the restriction is
        // data-driven, not a code branch. If it hits a DIFFERENT RIP for
        // Izawa, that's the function to patch.
        if ((pollTick % 300) == 0) { // ~10s at 30ms/tick
            uintptr_t izawaAnchor = PlayerScanner::FindBestIzawaAnchor();
            if (izawaAnchor && izawaAnchor != lastAutoAnchor) {
                lastAutoAnchor = izawaAnchor;
                PlayerScanner::g_lastFoundRecordAddr = izawaAnchor;
                if (WriteTrap::Watch("Izawa", izawaAnchor))
                    WriteLog("[i] Auto-scan: watching Izawa @ 0x%llX", (unsigned long long)izawaAnchor);
            }

            auto chain = SavePointer::Resolve();
            if (chain.ok) {
                uintptr_t customAddr = chain.save + 0x1B4; // confirmed offset from earlier work
                if (customAddr != lastCustomAnchor) {
                    lastCustomAnchor = customAddr;
                    if (WriteTrap::Watch("CustomPlayer", customAddr))
                        WriteLog("[i] Auto-scan: watching CustomPlayer @ 0x%llX", (unsigned long long)customAddr);
                }
            }
        }

        // Prerequisite confirmed necessary (though not sufficient alone):
        // make sure every mv_/smv_ move card is owned. Tries for ~20s
        // after the save pointer resolves, then gives up quietly.
        if (!unlockDone && unlockAttempts < 40 && (pollTick % 15) == 0) {
            auto chain = SavePointer::Resolve();
            if (chain.ok) {
                ++unlockAttempts;
                auto res = MoveInventoryUnlocker::UnlockAll(chain.save);
                if (res.ok) {
                    unlockDone = true;
                    WriteLog("[+] MoveInventoryUnlocker: unlocked %d/%d move cards (%d mv_, %d smv_, %d already owned)",
                             res.newlyUnlocked, res.totalKeys, res.normalKeys, res.superKeys, res.alreadyOwned);
                } else if (unlockAttempts >= 40) {
                    WriteLog("[!] MoveInventoryUnlocker: giving up after %d tries - %s",
                             unlockAttempts, res.failReason ? res.failReason : "unknown reason");
                }
            }
        }
        WriteTrap::DrainPending(); // safe context: actual file I/O happens here, never in the VEH handler

        ++pollTick;

        Sleep(30);
    }

    WriteLog("[i] Watcher thread terminated.");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hSelf = hModule;
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
