// ============================================================
//  write_trap.h  -  "find out what accesses this address" (CE-style),
//                    multi-target, self-rearming, deduplicated
//  ------------------------------------------------------------
//  Watches up to kMaxSlots labeled addresses AT THE SAME TIME (e.g.
//  "Izawa" = a regular player, "CustomPlayer" = the user-made one),
//  via PAGE_GUARD + a Vectored Exception Handler. Each hit is
//  attributed to whichever slot's page it belongs to, deduped by
//  (slot, RIP) pair, so the report directly shows whether opening
//  the move-equip menu for a regular player hits the SAME code as
//  for the custom player (data-driven restriction, no code branch to
//  patch) or a DIFFERENT one (a real branch we can hook/bypass).
// ============================================================
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern void WriteLog(const char* fmt, ...);

namespace WriteTrap {

static constexpr int kMaxSlots = 4;
static constexpr int kMaxDistinctRipsPerSlot = 20; // per label, so two labels = up to 40 total

struct Slot {
    char      label[32] = {};
    uintptr_t watchAddr = 0;
    uintptr_t pageBase = 0;
    bool      armed = false;
    bool      finished = false;
};
inline Slot g_slots[kMaxSlots];
inline int  g_slotCount = 0;

struct SeenRip { int slot; uintptr_t rip; int count; };
inline std::vector<SeenRip> g_seenRips;
inline int   g_totalHits = 0;
inline void* g_vehHandle = nullptr;
inline size_t g_pageSize = 0x1000;

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

inline bool RearmSlot(Slot& s) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(s.pageBase), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    DWORD oldProt = 0;
    DWORD baseProt = mbi.Protect & ~PAGE_GUARD;
    return VirtualProtect(reinterpret_cast<LPVOID>(s.pageBase), g_pageSize, baseProt | PAGE_GUARD, &oldProt) != 0;
}

inline int CountDistinctForSlot(int slotIdx) {
    int n = 0;
    for (auto& r : g_seenRips) if (r.slot == slotIdx) ++n;
    return n;
}

inline LONG CALLBACK GuardPageHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_GUARD_PAGE) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t faultAddr = static_cast<uintptr_t>(ep->ExceptionRecord->ExceptionInformation[1]);

    int hitSlot = -1;
    for (int i = 0; i < g_slotCount; ++i) {
        Slot& s = g_slots[i];
        if (s.armed && faultAddr >= s.pageBase && faultAddr < s.pageBase + g_pageSize) { hitSlot = i; break; }
    }
    if (hitSlot < 0) return EXCEPTION_CONTINUE_SEARCH; // not one of ours

    Slot& s = g_slots[hitSlot];
    uintptr_t rip = static_cast<uintptr_t>(ep->ContextRecord->Rip);
    ++g_totalHits;

    SeenRip* existing = nullptr;
    for (auto& r : g_seenRips) if (r.slot == hitSlot && r.rip == rip) { existing = &r; break; }

    if (existing) {
        ++existing->count;
    } else if (CountDistinctForSlot(hitSlot) < kMaxDistinctRipsPerSlot) {
        g_seenRips.push_back({ hitSlot, rip, 1 });
        uintptr_t modBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));

        FILE* f = nullptr;
        fopen_s(&f, "CT2_WriteTrap_Report.txt", "a");
        if (f) {
            fprintf(f, "=== [%s] hit #%d, distinct RIP #%d for this label ===\n",
                    s.label, g_totalHits, CountDistinctForSlot(hitSlot));
            fprintf(f, "  watched address: 0x%llX   faulting access: 0x%llX\n",
                    (unsigned long long)s.watchAddr, (unsigned long long)faultAddr);
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
        WriteLog("[+] WriteTrap[%s]: new distinct RIP = 0x%llX (total hits: %d)",
                 s.label, (unsigned long long)rip, g_totalHits);
    }

    if (CountDistinctForSlot(hitSlot) >= kMaxDistinctRipsPerSlot) {
        s.armed = false;
        s.finished = true;
        WriteLog("[i] WriteTrap[%s]: finished (cap reached) - see CT2_WriteTrap_Report.txt", s.label);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    RearmSlot(s);
    return EXCEPTION_CONTINUE_EXECUTION;
}

inline void EnsureHandlerInstalled() {
    if (!g_vehHandle) g_vehHandle = AddVectoredExceptionHandler(1, GuardPageHandler);
    if (g_pageSize == 0x1000) {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        if (si.dwPageSize) g_pageSize = si.dwPageSize;
    }
}

// Finds (or creates) the slot for `label`, points it at `addr`, keeping
// anything already collected under that same label. This is what both
// the manual hotkeys and the automatic background loop use - call it as
// often as you like, re-targeting is cheap and non-destructive.
inline bool Watch(const char* label, uintptr_t addr) {
    if (!addr) return false;
    EnsureHandlerInstalled();

    int idx = -1;
    for (int i = 0; i < g_slotCount; ++i) if (strcmp(g_slots[i].label, label) == 0) { idx = i; break; }
    if (idx < 0) {
        if (g_slotCount >= kMaxSlots) { WriteLog("[!] WriteTrap: no free slot for '%s'", label); return false; }
        idx = g_slotCount++;
        strncpy_s(g_slots[idx].label, label, sizeof(g_slots[idx].label) - 1);
    }
    Slot& s = g_slots[idx];
    if (s.finished) return false; // this label already hit its cap, leave it alone

    s.watchAddr = addr;
    s.pageBase = addr & ~(static_cast<uintptr_t>(g_pageSize) - 1);
    bool ok = RearmSlot(s);
    s.armed = ok;
    return ok;
}

// Explicit reset for one label (clears its collected RIPs) - only used by
// the manual F6 hotkey for an intentional "start this one over".
inline void ResetLabel(const char* label) {
    for (int i = (int)g_seenRips.size() - 1; i >= 0; --i)
        if (g_slots[g_seenRips[i].slot].label == std::string(label)) g_seenRips.erase(g_seenRips.begin() + i);
    for (int i = 0; i < g_slotCount; ++i)
        if (strcmp(g_slots[i].label, label) == 0) g_slots[i].finished = false;
}

} // namespace WriteTrap
