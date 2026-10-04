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
//  Hotkeys:
//    F8  - fingerprint scan: locate known players (Izawa/Sawada/
//          Hyuga) anywhere in process memory, report candidates.
//    F7  - re-finds the Izawa anchor, dumps the surrounding array
//          sequence, and chases the pointer chain up to 5 hops
//          looking for something stable inside the game module.
//    F6  - arms a self-rearming write/access trap on whatever F7
//          last found - catches the actual instruction(s) that
//          touch that memory, deduped by RIP.
//    F9  - unload the DLL.
//    ESC - stop the watcher thread (anything already armed/applied
//          stays as-is).
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

static constexpr const char* TRAINER_NAME = "CT2 AllMoves (research build)";
static constexpr const char* TRAINER_VER  = "0.3";

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
    WriteLog("[i] No gameplay hooks installed yet - this build is for finding the real one. "
             "F8=fingerprint scan, F7=array dump+pointer chase, F6=write trap, F9=unload.");

    bool unlockDone = false;
    int unlockAttempts = 0;
    bool lastF9 = false, lastF8 = false, lastF7 = false, lastF6 = false;
    int pollTick = 0;

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
            if (PlayerScanner::g_lastFoundRecordAddr) WriteTrap::Arm(PlayerScanner::g_lastFoundRecordAddr);
            else WriteLog("[!] WriteTrap: press F7 first so there's something to watch.");
        }
        lastF6 = f6;

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
