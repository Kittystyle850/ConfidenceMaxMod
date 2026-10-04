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

static constexpr int kMaxDistinctRips = 40; // accumulates for the WHOLE session now, not per-arm
inline bool g_finished = false;

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

    if (static_cast<int>(g_seenRips.size()) >= kMaxDistinctRips) {
        g_armed = false;
        g_finished = true;
        FILE* f = nullptr;
        fopen_s(&f, "CT2_WriteTrap_Report.txt", "a");
        if (f) {
            fprintf(f, "=== watch finished (distinct-RIP cap reached): %d total hits, %zu distinct RIPs ===\n\n",
                    g_totalHits, g_seenRips.size());
            fclose(f);
        }
        WriteLog("[i] WriteTrap: finished (cap reached) - %d total hits, %zu distinct RIPs, see CT2_WriteTrap_Report.txt",
                 g_totalHits, g_seenRips.size());
        return EXCEPTION_CONTINUE_EXECUTION; // leave the page unguarded
    }

    Rearm(); // keep watching the same page
    return EXCEPTION_CONTINUE_EXECUTION;
}

inline void EnsureHandlerInstalled() {
    if (!g_vehHandle) g_vehHandle = AddVectoredExceptionHandler(1, GuardPageHandler);
}

inline bool ArmInternal(uintptr_t addr, bool verbose) {
    EnsureHandlerInstalled();
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    g_pageSize = si.dwPageSize ? si.dwPageSize : 0x1000;
    g_watchAddr = addr;
    g_watchPageBase = addr & ~(static_cast<uintptr_t>(g_pageSize) - 1);

    if (!Rearm()) {
        WriteLog("[!] WriteTrap: VirtualProtect failed on 0x%llX (err=%lu)",
                 (unsigned long long)g_watchPageBase, GetLastError());
        return false;
    }
    g_armed = true;
    if (verbose) {
        WriteLog("[i] WriteTrap: watching 0x%llX, self-rearming up to %d distinct instructions "
                  "(accumulates for the whole session).", (unsigned long long)addr, kMaxDistinctRips);
    }
    return true;
}

// Starts a watch session on the page containing `addr`, clearing any
// accumulated findings first - only used by the manual F6 hotkey, for
// an explicit "start fresh" request.
inline bool ArmFresh(uintptr_t addr) {
    g_seenRips.clear();
    g_totalHits = 0;
    g_finished = false;
    g_armStartTick = GetTickCount();
    return ArmInternal(addr, true);
}

// Re-points the watch at a new address WITHOUT clearing anything already
// collected - this is what the automatic background loop uses, since the
// array keeps getting reallocated at a different address every time the
// roster screen is touched, but we still want one cumulative report.
inline bool RetargetKeepHistory(uintptr_t addr) {
    if (g_finished) return false; // already hit the cap, stop bothering it
    bool first = (g_armStartTick == 0);
    if (first) g_armStartTick = GetTickCount();
    return ArmInternal(addr, false);
}

} // namespace WriteTrap
