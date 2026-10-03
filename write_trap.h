// ============================================================
//  write_trap.h  -  "find out what writes to this address" (CE-style)
//  ------------------------------------------------------------
//  The F7 roster scan showed the array's content changes between
//  visits to the same screen - it's rebuilt on the fly, not a fixed
//  table. Chasing a pointer to an ephemeral buffer is fragile. The
//  robust fix: catch the exact instruction that WRITES a record's
//  field, the moment it happens, via a guard-page trap - then hook
//  that function directly, the same way we hooked CHAIN MAX.
//
//  Mechanism: mark the 4KB page containing the watched address with
//  PAGE_GUARD. The next access to that page raises
//  STATUS_GUARD_PAGE_VIOLATION, caught here via a Vectored Exception
//  Handler, which logs the faulting instruction's RIP (+ module RVA
//  if it's inside the game) and a hex dump of the surrounding code
//  bytes - everything needed to identify and hook that function.
//  One-shot: Windows auto-clears PAGE_GUARD after the first trap, we
//  don't re-arm, so normal execution resumes right after.
// ============================================================
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>

extern void WriteLog(const char* fmt, ...);

namespace WriteTrap {

inline uintptr_t g_watchAddr = 0;
inline uintptr_t g_watchPageBase = 0;
inline size_t    g_pageSize = 0x1000;
inline bool      g_armed = false;
inline void*     g_vehHandle = nullptr;

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

inline LONG CALLBACK GuardPageHandler(EXCEPTION_POINTERS* ep) {
    if (!g_armed) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_GUARD_PAGE) return EXCEPTION_CONTINUE_SEARCH;

    uintptr_t faultAddr = static_cast<uintptr_t>(ep->ExceptionRecord->ExceptionInformation[1]);
    if (faultAddr < g_watchPageBase || faultAddr >= g_watchPageBase + g_pageSize)
        return EXCEPTION_CONTINUE_SEARCH; // not our page, let someone else handle it

    g_armed = false; // one-shot
    uintptr_t rip = static_cast<uintptr_t>(ep->ContextRecord->Rip);
    uintptr_t modBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));

    FILE* f = nullptr;
    fopen_s(&f, "CT2_WriteTrap_Report.txt", "a");
    if (f) {
        fprintf(f, "=== write trapped ===\n");
        fprintf(f, "  watched address: 0x%llX\n", (unsigned long long)g_watchAddr);
        fprintf(f, "  faulting access: 0x%llX\n", (unsigned long long)faultAddr);
        fprintf(f, "  RIP: 0x%llX", (unsigned long long)rip);
        if (modBase && rip >= modBase) fprintf(f, "  (module RVA = 0x%llX)", (unsigned long long)(rip - modBase));
        fprintf(f, "\n");
        fprintf(f, "  RCX=0x%llX RDX=0x%llX R8=0x%llX R9=0x%llX\n",
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
    WriteLog("[+] WriteTrap: caught a write to 0x%llX from RIP 0x%llX - see CT2_WriteTrap_Report.txt",
             (unsigned long long)faultAddr, (unsigned long long)rip);

    // Let Windows clear PAGE_GUARD on this page (it does so automatically
    // for the page that faulted) and let the instruction re-execute
    // normally - we don't touch protection here, continuing is enough.
    return EXCEPTION_CONTINUE_EXECUTION;
}

inline void EnsureHandlerInstalled() {
    if (!g_vehHandle) g_vehHandle = AddVectoredExceptionHandler(1, GuardPageHandler);
}

// Arms a one-shot trap on the page containing `addr`. Call this, then do
// whatever in-game action should trigger a rewrite of that memory
// (scroll away and back, re-open the screen, etc).
inline bool Arm(uintptr_t addr) {
    EnsureHandlerInstalled();
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    g_pageSize = si.dwPageSize ? si.dwPageSize : 0x1000;
    g_watchAddr = addr;
    g_watchPageBase = addr & ~(static_cast<uintptr_t>(g_pageSize) - 1);

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(g_watchPageBase), &mbi, sizeof(mbi)) != sizeof(mbi)) {
        WriteLog("[!] WriteTrap: VirtualQuery failed on 0x%llX", (unsigned long long)g_watchPageBase);
        return false;
    }
    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(g_watchPageBase), g_pageSize,
                         mbi.Protect | PAGE_GUARD, &oldProt)) {
        WriteLog("[!] WriteTrap: VirtualProtect failed on 0x%llX (err=%lu)",
                 (unsigned long long)g_watchPageBase, GetLastError());
        return false;
    }
    g_armed = true;
    WriteLog("[i] WriteTrap: armed on page 0x%llX (watching 0x%llX) - now trigger a refresh in-game.",
             (unsigned long long)g_watchPageBase, (unsigned long long)addr);
    return true;
}

} // namespace WriteTrap
