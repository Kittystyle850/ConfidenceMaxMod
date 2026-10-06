// ============================================================
//  write_trap.h  -  "find out what accesses this address" (CE-style),
//                    multi-target, self-rearming, crash-safe
//  ------------------------------------------------------------
//  v2: the exception handler used to call std::vector::push_back and
//  fopen/fprintf directly. That is unsafe - a Vectored Exception
//  Handler runs synchronously on whatever thread faulted, and if that
//  thread already held the CRT heap lock (e.g. it faulted from inside
//  an allocation-adjacent code path), re-entering malloc/fopen from
//  the handler deadlocks or corrupts the heap. That's the most likely
//  cause of the "fatal error" crash.
//
//  Fix: the handler ONLY writes into pre-allocated fixed-size arrays
//  via Interlocked ops - no malloc, no file I/O, nothing that can
//  block or touch a lock the interrupted thread might hold. A normal
//  thread (our own background loop in main.cpp) drains the pending
//  queue every tick and does the actual file writing from a safe,
//  ordinary context.
// ============================================================
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern void WriteLog(const char* fmt, ...);

namespace WriteTrap {

static constexpr int kMaxSlots = 4;
static constexpr int kMaxDistinctRipsPerSlot = 20;
static constexpr int kSeenCap = kMaxSlots * kMaxDistinctRipsPerSlot;
static constexpr int kPendingCap = 64; // ring buffer drained by the main thread
static constexpr DWORD kRearmCooldownMs = 25; // prevents a same-instruction retrigger storm

struct Slot {
    char      label[32] = {};
    uintptr_t watchAddr = 0;
    uintptr_t pageBase = 0;
    volatile LONG armed = 0;
    volatile LONG finished = 0;
    volatile LONG lastRearmTick = 0;
};
inline Slot g_slots[kMaxSlots];
inline volatile LONG g_slotCount = 0;

struct SeenRip { int slot; uintptr_t rip; volatile LONG count; };
inline SeenRip g_seenRips[kSeenCap];
inline volatile LONG g_seenCount = 0;

// Snapshot captured by the handler (no pointers into non-fixed memory),
// drained and turned into a report entry by the background thread.
struct PendingHit {
    int slot;
    uintptr_t rip;
    uintptr_t faultAddr;
    uint64_t rcx, rdx, r8, r9, rax, rbx, rsi, rdi, rbp, rsp;
    uint8_t code[65]; // RIP-32..RIP+32, snapshotted NOW while we know it's valid
};
inline PendingHit g_pending[kPendingCap];
inline volatile LONG g_pendingWrite = 0; // next index to write (mod kPendingCap)
inline volatile LONG g_pendingRead = 0;  // next index the drainer consumes

inline void* g_vehHandle = nullptr;
inline size_t g_pageSize = 0x1000;
inline volatile LONG g_totalHits = 0;

inline bool RearmSlot(Slot& s) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(s.pageBase), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    DWORD oldProt = 0;
    DWORD baseProt = mbi.Protect & ~PAGE_GUARD;
    return VirtualProtect(reinterpret_cast<LPVOID>(s.pageBase), g_pageSize, baseProt | PAGE_GUARD, &oldProt) != 0;
}

// ------------------------------------------------------------
//  Handler: fixed-array writes only, no malloc, no I/O, no locks
//  beyond what Interlocked* itself needs (lock-free).
// ------------------------------------------------------------
inline LONG CALLBACK GuardPageHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_GUARD_PAGE) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t faultAddr = static_cast<uintptr_t>(ep->ExceptionRecord->ExceptionInformation[1]);

    int hitSlot = -1;
    LONG n = g_slotCount;
    for (int i = 0; i < n; ++i) {
        Slot& s = g_slots[i];
        if (s.armed && faultAddr >= s.pageBase && faultAddr < s.pageBase + g_pageSize) { hitSlot = i; break; }
    }
    if (hitSlot < 0) return EXCEPTION_CONTINUE_SEARCH; // not ours

    Slot& s = g_slots[hitSlot];
    uintptr_t rip = static_cast<uintptr_t>(ep->ContextRecord->Rip);
    InterlockedIncrement(&g_totalHits);

    // Dedup scan - plain linear read over a fixed array, no allocation.
    SeenRip* existing = nullptr;
    LONG seenNow = g_seenCount;
    for (LONG i = 0; i < seenNow; ++i) {
        if (g_seenRips[i].slot == hitSlot && g_seenRips[i].rip == rip) { existing = &g_seenRips[i]; break; }
    }

    if (existing) {
        InterlockedIncrement(&existing->count);
    } else if (seenNow < kSeenCap) {
        LONG idx = InterlockedIncrement(&g_seenCount) - 1;
        if (idx < kSeenCap) {
            g_seenRips[idx].slot = hitSlot;
            g_seenRips[idx].rip = rip;
            g_seenRips[idx].count = 1;

            // Queue a snapshot for the background thread to write out -
            // reading nearby memory here is fine (plain reads, no locks),
            // just no malloc/file-IO.
            LONG pIdx = InterlockedIncrement(&g_pendingWrite) - 1;
            PendingHit& ph = g_pending[pIdx % kPendingCap];
            ph.slot = hitSlot;
            ph.rip = rip;
            ph.faultAddr = faultAddr;
            ph.rcx = ep->ContextRecord->Rcx; ph.rdx = ep->ContextRecord->Rdx;
            ph.r8 = ep->ContextRecord->R8;   ph.r9 = ep->ContextRecord->R9;
            ph.rax = ep->ContextRecord->Rax; ph.rbx = ep->ContextRecord->Rbx;
            ph.rsi = ep->ContextRecord->Rsi; ph.rdi = ep->ContextRecord->Rdi;
            ph.rbp = ep->ContextRecord->Rbp; ph.rsp = ep->ContextRecord->Rsp;
            for (int off = -32; off <= 32; ++off) {
                uintptr_t p = rip + off;
                // best-effort: this address is executable code we're not
                // guarding, a plain read is safe without IsBadReadPtr's
                // extra SEH wrapping (which itself is safe here too, but
                // keep the handler as lean as possible).
                ph.code[off + 32] = *reinterpret_cast<const uint8_t*>(p);
            }
        }
    }

    LONG countForSlot = 0;
    for (LONG i = 0; i < g_seenCount; ++i) if (g_seenRips[i].slot == hitSlot) ++countForSlot;
    if (countForSlot >= kMaxDistinctRipsPerSlot) {
        InterlockedExchange(&s.armed, 0);
        InterlockedExchange(&s.finished, 1);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Cooldown: if this exact page was just re-armed a moment ago, skip
    // re-guarding it this time - prevents a tight loop from re-faulting
    // thousands of times a second on the same instruction.
    DWORD now = GetTickCount();
    DWORD last = static_cast<DWORD>(s.lastRearmTick);
    if (now - last >= kRearmCooldownMs) {
        InterlockedExchange(&s.lastRearmTick, static_cast<LONG>(now));
        RearmSlot(s);
    }
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

inline bool Watch(const char* label, uintptr_t addr) {
    if (!addr) return false;
    EnsureHandlerInstalled();

    int idx = -1;
    LONG n = g_slotCount;
    for (int i = 0; i < n; ++i) if (strcmp(g_slots[i].label, label) == 0) { idx = i; break; }
    if (idx < 0) {
        if (n >= kMaxSlots) { WriteLog("[!] WriteTrap: no free slot for '%s'", label); return false; }
        idx = InterlockedIncrement(&g_slotCount) - 1;
        strncpy_s(g_slots[idx].label, label, sizeof(g_slots[idx].label) - 1);
    }
    Slot& s = g_slots[idx];
    if (s.finished) return false;

    s.watchAddr = addr;
    s.pageBase = addr & ~(static_cast<uintptr_t>(g_pageSize) - 1);
    bool ok = RearmSlot(s);
    InterlockedExchange(&s.armed, ok ? 1 : 0);
    return ok;
}

inline void ResetLabel(const char* label) {
    for (int i = 0; i < g_slotCount; ++i) {
        if (strcmp(g_slots[i].label, label) != 0) continue;
        g_slots[i].finished = 0;
        // Compact out this slot's entries from g_seenRips (simple approach:
        // mark slot index as -1 so dedup/count scans skip them; good enough,
        // we don't bother reclaiming the array slots themselves).
        for (int j = 0; j < g_seenCount; ++j) if (g_seenRips[j].slot == i) g_seenRips[j].slot = -1;
    }
}

// ------------------------------------------------------------
//  Drain: call this from the normal background thread (NOT the
//  handler) every tick. Safe to do file I/O and normal work here.
// ------------------------------------------------------------
inline void DumpCodeBytes(FILE* f, const uint8_t* code) {
    fprintf(f, "  code bytes RIP-32..RIP+32:\n   ");
    for (int i = 0; i < 65; ++i) {
        fprintf(f, "%02X ", code[i]);
        if (i == 32) fprintf(f, "<RIP> ");
    }
    fprintf(f, "\n");
}

inline void DrainPending() {
    LONG writeNow = g_pendingWrite;
    LONG readPos = g_pendingRead;
    if (readPos >= writeNow) return;

    uintptr_t modBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    FILE* f = nullptr;
    bool opened = fopen_s(&f, "CT2_WriteTrap_Report.txt", "a") == 0 && f;

    for (; readPos < writeNow; ++readPos) {
        const PendingHit& ph = g_pending[readPos % kPendingCap];
        const char* label = (ph.slot >= 0 && ph.slot < g_slotCount) ? g_slots[ph.slot].label : "?";

        WriteLog("[+] WriteTrap[%s]: new distinct RIP = 0x%llX (total hits so far: %ld)",
                 label, (unsigned long long)ph.rip, g_totalHits);

        if (opened) {
            fprintf(f, "=== [%s] RIP 0x%llX", label, (unsigned long long)ph.rip);
            if (modBase && ph.rip >= modBase) fprintf(f, " (module RVA = 0x%llX)", (unsigned long long)(ph.rip - modBase));
            fprintf(f, " ===\n");
            fprintf(f, "  watched address: 0x%llX   faulting access: 0x%llX\n",
                    (unsigned long long)(ph.slot >= 0 ? g_slots[ph.slot].watchAddr : 0),
                    (unsigned long long)ph.faultAddr);
            fprintf(f, "  RCX=0x%llX RDX=0x%llX R8=0x%llX R9=0x%llX\n",
                    (unsigned long long)ph.rcx, (unsigned long long)ph.rdx,
                    (unsigned long long)ph.r8, (unsigned long long)ph.r9);
            fprintf(f, "  RAX=0x%llX RBX=0x%llX RSI=0x%llX RDI=0x%llX RBP=0x%llX RSP=0x%llX\n",
                    (unsigned long long)ph.rax, (unsigned long long)ph.rbx,
                    (unsigned long long)ph.rsi, (unsigned long long)ph.rdi,
                    (unsigned long long)ph.rbp, (unsigned long long)ph.rsp);
            DumpCodeBytes(f, ph.code);
            fprintf(f, "\n");
        }
    }
    if (opened) fclose(f);
    InterlockedExchange(&g_pendingRead, writeNow);
}

} // namespace WriteTrap
