// ============================================================
//  write_trap.h  -  "find out what accesses this address" (CE-style),
//                    self-rearming and deduplicated
//  ------------------------------------------------------------
//  Mark the 4KB page containing the watched address with PAGE_GUARD.
//  The next access raises STATUS_GUARD_PAGE_VIOLATION, caught here
//  via a Vectored Exception Handler. Windows auto-clears PAGE_GUARD
//  after each trap, so we re-arm it ourselves every time - meaning
//  this keeps watching across however many menu navigations the
//  user does, instead of needing an F6 press per attempt.
//
//  Naive "log every hit" would flood the file if something (e.g.
//  render code) reads this memory every frame. Instead we dedupe by
//  RIP: each NEW distinct instruction address gets one full report
//  block (with a code dump), repeats of an already-seen RIP just
//  bump a counter silently. Stops auto-rearming after kMaxDistinctRips
//  distinct hits or kAutoDisarmMs, whichever comes first.
// ============================================================
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <vector>

extern void WriteLog(const char* fmt, ...);

namespace WriteTrap {

inline uintptr_t g_watchAddr = 0;
inline uintptr_t g_watchPageBase = 0;
inline size_t    g_pageSize = 0x1000;
inline bool      g_armed = false;
inline void*     g_vehHandle = nullptr;

struct SeenRip { uintptr_t rip; int count; };
inline std::vector<SeenRip> g_seenRips;
inline int   g_totalHits = 0;
inline DWORD g_armStartTick = 0;

static constexpr int   kMaxDistinctRips = 25;
static constexpr DWORD kAutoDisarmMs = 90000; // 90s safety cap

inline void DumpCodeAround(FILE* f, uintptr_t rip) {
    fprintf(f, "  code bytes RIP-32..RIP+32:\n   ");
    for (int off = -32; off <= 32; ++off) {
        uint8_t b = 0;
        uintptr_t p = rip + off;
        if (!IsBadReadPtr(reinterpret_cast<void*>(p), 1)) b = *reinterpret_cast<volatile uint8_t*>(p);
        fprintf(f, "%02X ", b);
        if (off == 0) fprintf(f, "<RIP> ");
    }
    fprintf(f, "\n");
}

inline bool Rearm() {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(g_watchPageBase), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    DWORD oldProt = 0;
    DWORD baseProt = mbi.Protect & ~PAGE_GUARD; // don't stack GUARD onto itself
    return VirtualProtect(reinterpret_cast<LPVOID>(g_watchPageBase), g_pageSize,
                           baseProt | PAGE_GUARD, &oldProt) != 0;
}

inline LONG CALLBACK GuardPageHandler(EXCEPTION_POINTERS* ep) {
    if (!g_armed) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_GUARD_PAGE) return EXCEPTION_CONTINUE_SEARCH;

    uintptr_t faultAddr = static_cast<uintptr_t>(ep->ExceptionRecord->ExceptionInformation[1]);
    if (faultAddr < g_watchPageBase || faultAddr >= g_watchPageBase + g_pageSize)
        return EXCEPTION_CONTINUE_SEARCH; // not our page

    uintptr_t rip = static_cast<uintptr_t>(ep->ContextRecord->Rip);
    ++g_totalHits;

    SeenRip* existing = nullptr;
    for (auto& s : g_seenRips) if (s.rip == rip) { existing = &s; break; }

    if (existing) {
        ++existing->count; // already logged this one in full, just count it
    } else if (static_cast<int>(g_seenRips.size()) < kMaxDistinctRips) {
        g_seenRips.push_back({ rip, 1 });
        uintptr_t modBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));

        FILE* f = nullptr;
        fopen_s(&f, "CT2_WriteTrap_Report.txt", "a");
        if (f) {
            fprintf(f, "=== hit #%d, distinct RIP #%zu ===\n", g_totalHits, g_seenRips.size());
            fprintf(f, "  watched address: 0x%llX   faulting access: 0x%llX\n",
                    (unsigned long long)g_watchAddr, (unsigned long long)faultAddr);
            fprintf(f, "  RIP: 0x%llX", (unsigned long long)rip);
            if (modBase && rip >= modBase) fprintf(f, "  (module RVA = 0x%llX)", (unsigned long long)(rip - modBase));
            fprintf(f, "\n  RCX=0x%llX RDX=0x%llX R8=0x%llX R9=0x%llX\n",
                    (unsigned long long)ep->ContextRecord->Rcx, (unsigned long long)ep->ContextRecord->Rdx,
                    (unsigned long long)ep->ContextRecord->R8, (unsigned long long)ep->ContextRecord->R9);
            fprintf(f, "  RAX=0x%llX RBX=0x%llX RSI=0x%llX RDI=0x%llX RBP=0x%llX RSP=0x%llX\n",
                    (unsigned long long)ep->ContextRecord->Rax, (unsigned long long)ep->ContextRecord->Rbx,
                    (unsigned long long)ep->ContextRecord->Rsi, (unsigned long long)ep->ContextRecord->Rdi,
                    (unsigned long long)ep->ContextRecord->Rbp, (unsigned long long)ep->ContextRecord->Rsp);
            DumpCodeAround(f, rip);
            fprintf(f, "\n");
            fclose(f);
        }
        WriteLog("[+] WriteTrap: new distinct RIP #%zu = 0x%llX (total hits so far: %d)",
                 g_seenRips.size(), (unsigned long long)rip, g_totalHits);
    }

    bool timeUp = (GetTickCount() - g_armStartTick) > kAutoDisarmMs;
    bool full = static_cast<int>(g_seenRips.size()) >= kMaxDistinctRips;
    if (timeUp || full) {
        g_armed = false;
        FILE* f = nullptr;
        fopen_s(&f, "CT2_WriteTrap_Report.txt", "a");
        if (f) {
            fprintf(f, "=== watch finished (%s): %d total hits, %zu distinct RIPs ===\n\n",
                    full ? "distinct-RIP cap reached" : "time cap reached", g_totalHits, g_seenRips.size());
            fclose(f);
        }
        WriteLog("[i] WriteTrap: finished watching (%s) - %d total hits, %zu distinct RIPs, see CT2_WriteTrap_Report.txt",
                 full ? "cap reached" : "90s elapsed", g_totalHits, g_seenRips.size());
        // leave the page in its normal (non-guarded) state
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    Rearm(); // keep watching
    return EXCEPTION_CONTINUE_EXECUTION;
}

inline void EnsureHandlerInstalled() {
    if (!g_vehHandle) g_vehHandle = AddVectoredExceptionHandler(1, GuardPageHandler);
}

// Starts (or restarts) a watch session on the page containing `addr`.
// Keeps re-arming itself after every hit until it's seen kMaxDistinctRips
// different instructions or kAutoDisarmMs has passed - no need to press
// F6 again after every navigation, just keep playing and check the
// report file afterward.
inline bool Arm(uintptr_t addr) {
    EnsureHandlerInstalled();
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    g_pageSize = si.dwPageSize ? si.dwPageSize : 0x1000;
    g_watchAddr = addr;
    g_watchPageBase = addr & ~(static_cast<uintptr_t>(g_pageSize) - 1);
    g_seenRips.clear();
    g_totalHits = 0;
    g_armStartTick = GetTickCount();

    if (!Rearm()) {
        WriteLog("[!] WriteTrap: VirtualProtect failed on 0x%llX (err=%lu)",
                 (unsigned long long)g_watchPageBase, GetLastError());
        return false;
    }
    g_armed = true;
    WriteLog("[i] WriteTrap: watching 0x%llX, self-rearming for up to %d distinct hits / %lu s "
              "- just keep navigating in-game, no need to press F6 again.",
              (unsigned long long)addr, kMaxDistinctRips, kAutoDisarmMs / 1000);
    return true;
}

} // namespace WriteTrap
