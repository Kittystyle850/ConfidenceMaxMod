// ============================================================
//  player_scanner.h  -  automated roster-array hunter
//  ------------------------------------------------------------
//  We don't know where the live "all 358 players" array lives in
//  memory yet - nobody has reverse engineered it (not the CT table,
//  not the save editor). Rather than you doing this by hand in
//  Cheat Engine, this scans the whole process address space itself
//  for known "fingerprint" players (CharaID + their exact equipped
//  moveset, pulled from the save editor's own verified database)
//  and reports every spot where several of those values turn up
//  close together - that clustering is what a real struct record
//  looks like, as opposed to coincidental noise.
//
//  Triggered by F8. Writes CT2_PlayerScan_Report.txt. Safe to run
//  while playing - runs on our own background thread, not the
//  game's render thread, so at worst it causes a brief stutter.
// ============================================================
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <functional>
#include <algorithm>
#include "player_names.h"

extern void WriteLog(const char* fmt, ...);

namespace PlayerScanner {

// ------------------------------------------------------------
//  Known players, straight from the save editor's database.
//  "primary" is the field we scan for first - pick a value that's
//  rare enough to keep the initial hit count manageable (not 0-20,
//  which collide with timers/counters/UI state everywhere).
// ------------------------------------------------------------
struct Fingerprint {
    const char* label;
    int32_t charaId;
    int32_t tackle, dribble, shot, airshot, superId;
    int32_t primary; // which of the above to scan for first
};

static const Fingerprint g_fingerprints[] = {
    { "Izawa",  16, 913, 903, 910, 1304, 902, 1304 },
    { "Sawada", 17, 905, 304, 318, 1906, 305,  304 },
    { "Hyuga",   2, 908,   2,   1, 1905,   3, 1905 },
};
static constexpr int kFingerprintCount = sizeof(g_fingerprints) / sizeof(g_fingerprints[0]);

static constexpr int   kWindowBytes   = 0x80;   // search +/- this many bytes around each primary hit
static constexpr size_t kMaxHitsPerPrimary = 500000; // safety cap, a real roster won't trigger this

// ------------------------------------------------------------
//  Walk every committed, readable region of our own process.
// ------------------------------------------------------------
inline void ForEachRegion(const std::function<void(uint8_t*, size_t)>& visit) {
    uint8_t* addr = nullptr;
    MEMORY_BASIC_INFORMATION mbi{};
    for (;;) {
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        bool readable = mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READ | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0 &&
            (mbi.Protect & PAGE_GUARD) == 0 && (mbi.Protect & PAGE_NOACCESS) == 0;
        if (readable && mbi.RegionSize > 0) visit(reinterpret_cast<uint8_t*>(mbi.BaseAddress), mbi.RegionSize);
        uint8_t* next = reinterpret_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= addr) break; // overflow / no progress guard
        addr = next;
    }
}

// Scanning in-process means a direct pointer deref, not a safe
// ReadProcessMemory from outside - if the game frees/decommits a
// page between VirtualQuery and our read (very possible, the engine
// is allocating/freeing constantly), a naive scan can crash the
// game. Wrap each region in SEH so a bad page just gets skipped.
inline void ScanRegionSafe(uint8_t* base, size_t size, int32_t target,
                            std::vector<uintptr_t>& hits, size_t maxHits) {
    size_t count = size / 4;
    int32_t* p = reinterpret_cast<int32_t*>(base);
#if defined(_MSC_VER)
    __try {
        for (size_t i = 0; i < count; ++i) {
            if (hits.size() >= maxHits) return;
            if (p[i] == target) hits.push_back(reinterpret_cast<uintptr_t>(&p[i]));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // region went bad mid-scan - skip it, not fatal
    }
#else
    for (size_t i = 0; i < count; ++i) {
        if (hits.size() >= maxHits) return;
        if (p[i] == target) hits.push_back(reinterpret_cast<uintptr_t>(&p[i]));
    }
#endif
}

inline void ScanForInt32(int32_t target, std::vector<uintptr_t>& hits) {
    ForEachRegion([&](uint8_t* base, size_t size) {
        if (hits.size() >= kMaxHitsPerPrimary) return;
        ScanRegionSafe(base, size, target, hits, kMaxHitsPerPrimary);
    });
}

inline bool SafeReadI32(uintptr_t addr, int32_t& out) {
    if (IsBadReadPtr(reinterpret_cast<void*>(addr), 4)) return false;
    out = *reinterpret_cast<volatile int32_t*>(addr);
    return true;
}

struct Candidate {
    uintptr_t primaryAddr;
    int       score; // how many companion fields were found nearby
    int32_t   foundOffsets[8];
    const char* foundNames[8];
    int       foundCount;
};

// Scans one fingerprint: finds every occurrence of its "primary"
// value, then checks a window around each hit for the OTHER known
// field values (at any 4-byte-aligned offset, since we don't yet
// know the real struct layout - that's exactly what we're after).
inline std::vector<Candidate> ScanFingerprint(const Fingerprint& fp, FILE* rawLog) {
    std::vector<uintptr_t> hits;
    ScanForInt32(fp.primary, hits);
    if (rawLog) fprintf(rawLog, "[%s] primary value %d: %zu raw hits\n", fp.label, fp.primary, hits.size());

    struct Field { int32_t value; const char* name; };
    Field fields[] = {
        {fp.charaId, "CharaID"}, {fp.tackle, "Tackle"}, {fp.dribble, "Dribble"},
        {fp.shot, "Shot"}, {fp.airshot, "AirShot"}, {fp.superId, "SuperID"},
    };

    std::vector<Candidate> candidates;
    for (uintptr_t h : hits) {
        Candidate c{};
        c.primaryAddr = h;
        c.score = 0;
        c.foundCount = 0;
        for (int off = -kWindowBytes; off <= kWindowBytes; off += 4) {
            if (off == 0) continue; // that's the primary field itself
            int32_t v = 0;
            if (!SafeReadI32(h + off, v)) continue;
            for (auto& f : fields) {
                if (v == f.value && c.foundCount < 8) {
                    c.foundOffsets[c.foundCount] = off;
                    c.foundNames[c.foundCount] = f.name;
                    ++c.foundCount;
                    ++c.score;
                    break;
                }
            }
        }
        if (c.score >= 2) candidates.push_back(c); // primary + at least 2 companions nearby
    }
    return candidates;
}

inline void DumpAround(FILE* f, uintptr_t addr) {
    fprintf(f, "    dump %+d..%+d (int32, little-endian):\n", -kWindowBytes, kWindowBytes);
    for (int off = -kWindowBytes; off <= kWindowBytes; off += 16) {
        fprintf(f, "    %+5d:", off);
        for (int sub = 0; sub < 16 && off + sub <= kWindowBytes; sub += 4) {
            int32_t v = 0;
            if (SafeReadI32(addr + off + sub, v)) fprintf(f, " %10d", v);
            else fprintf(f, "       ----");
        }
        fprintf(f, "\n");
    }
}

// ------------------------------------------------------------
//  Array dump: given the already-confirmed Izawa anchor, walk
//  `stride` bytes at a time both directions and print each record's
//  CharaID (resolved to a real name) plus its next 7 raw int32
//  fields. Lets us read the field order/stride off in plain text
//  across many consecutive players at once, instead of guessing
//  from one isolated pair.
// ------------------------------------------------------------
inline const char* LookupPlayerName(int32_t charaId) {
    for (int i = 0; i < g_playerNames_count; ++i)
        if (g_playerNames[i].chara_id == charaId) return g_playerNames[i].name;
    return nullptr;
}

static constexpr size_t kAssumedStride = 0x28; // confirmed from the F8 report (Izawa -> Sawada gap)

inline void DumpArraySequence(uintptr_t recordBase, int before, int after, FILE* out) {
    for (int i = -before; i <= after; ++i) {
        uintptr_t rec = recordBase + static_cast<intptr_t>(i) * static_cast<intptr_t>(kAssumedStride);
        int32_t charaId = 0;
        if (!SafeReadI32(rec, charaId)) { fprintf(out, "[%+3d] 0x%llX  (unreadable)\n", i, (unsigned long long)rec); continue; }
        const char* name = LookupPlayerName(charaId);
        fprintf(out, "[%+3d] 0x%llX  CharaID=%-6d %-22s", i, (unsigned long long)rec, charaId, name ? name : "? (unknown id)");
        for (int off = 4; off <= 28; off += 4) {
            int32_t v = 0;
            if (SafeReadI32(rec + off, v)) fprintf(out, "+%02d:%-7d", off, v);
        }
        fprintf(out, "\n");
    }
}

inline void ScanForPointer(uintptr_t target, std::vector<uintptr_t>& hits, size_t maxHits) {
    ForEachRegion([&](uint8_t* base, size_t size) {
        if (hits.size() >= maxHits) return;
        size_t count = size / 8;
        uint64_t* p = reinterpret_cast<uint64_t*>(base);
#if defined(_MSC_VER)
        __try {
            for (size_t i = 0; i < count; ++i) {
                if (hits.size() >= maxHits) return;
                if (p[i] == target) hits.push_back(reinterpret_cast<uintptr_t>(&p[i]));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
#else
        for (size_t i = 0; i < count; ++i) {
            if (hits.size() >= maxHits) return;
            if (p[i] == target) hits.push_back(reinterpret_cast<uintptr_t>(&p[i]));
        }
#endif
    });
}

// After confirming the array's real base (the CharaID=1 record address),
// find who points TO it - and if that's still heap, keep hopping (pointer
// to a pointer to a pointer...) until something lands inside the game's
// main module. An in-module hit is gold: RVA = hit - moduleBase is stable
// across restarts, so we can hardcode "read *(moduleBase+RVA)" to reach
// the live array every session, exactly like we already do for
// UGameSaveData. Capped depth/branching so one F7 press stays fast.
inline void ScanForArrayPointer(uintptr_t arrayBase, FILE* out) {
    uintptr_t modBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    size_t modSize = 0;
    if (modBase) {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(modBase);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(modBase + dos->e_lfanew);
        modSize = nt->OptionalHeader.SizeOfImage;
    }

    static constexpr int kMaxDepth = 5;
    static constexpr int kBranchPerLevel = 3; // how many hits per target we follow onward
    static constexpr int kMaxFrontier = 6;    // total addresses carried into the next hop

    fprintf(out, "\nPointer chain hunt for array base 0x%llX (up to %d hops):\n",
            (unsigned long long)arrayBase, kMaxDepth);

    std::vector<uintptr_t> current = { arrayBase };
    bool foundStable = false;

    for (int depth = 1; depth <= kMaxDepth && !foundStable; ++depth) {
        fprintf(out, " -- hop %d, following %zu address(es) --\n", depth, current.size());
        std::vector<uintptr_t> next;

        for (uintptr_t target : current) {
            std::vector<uintptr_t> hits;
            ScanForPointer(target, hits, 50000);
            fprintf(out, "   0x%llX : %zu hit(s)", (unsigned long long)target, hits.size());
            if (hits.empty()) { fprintf(out, "  (dead end)\n"); continue; }
            fprintf(out, "\n");

            int taken = 0;
            for (uintptr_t h : hits) {
                bool inModule = modBase && h >= modBase && h < modBase + modSize;
                if (inModule) {
                    fprintf(out, "      0x%llX  -> INSIDE MODULE, RVA = 0x%llX  *** STABLE, use this ***\n",
                            (unsigned long long)h, (unsigned long long)(h - modBase));
                    foundStable = true;
                } else if (taken < kBranchPerLevel) {
                    fprintf(out, "      0x%llX  -> heap, following further\n", (unsigned long long)h);
                    next.push_back(h);
                    ++taken;
                }
            }
            if (hits.size() > static_cast<size_t>(taken) && !foundStable)
                fprintf(out, "      (+%zu more hit(s) not followed, capped)\n", hits.size() - taken);
        }

        if (foundStable) { fprintf(out, " -- stable anchor found at hop %d, stopping --\n", depth); break; }
        if (next.empty()) { fprintf(out, " -- dead end, no more pointers at hop %d --\n", depth); break; }

        std::sort(next.begin(), next.end());
        next.erase(std::unique(next.begin(), next.end()), next.end());
        if (next.size() > kMaxFrontier) next.resize(kMaxFrontier);
        current = std::move(next);
    }

    if (!foundStable)
        fprintf(out, "\nNo in-module anchor found within %d hops. Try again from a different "
                      "screen (one with the roster freshly loaded), or widen kBranchPerLevel.\n", kMaxDepth);
}

// Exposed so F6 (write_trap.h) can arm a guard-page trap on whatever F7
// most recently found, without the two modules needing to know much
// about each other.
inline uintptr_t g_lastFoundRecordAddr = 0;

// F7: re-finds the best Izawa candidate fresh (addresses are heap-based,
// they move every game restart), dumps the sequence around it, then
// hunts for whoever points at the array's true base (CharaID=1 record).
inline void RunArrayDump() {
    WriteLog("[i] PlayerScanner: F7 pressed, re-locating Izawa anchor then dumping array...");
    const Fingerprint& izawa = g_fingerprints[0]; // must stay "Izawa" at index 0

    FILE* raw = nullptr; // ScanFingerprint wants a log, we don't need it here
    auto candidates = ScanFingerprint(izawa, raw);
    if (candidates.empty()) {
        WriteLog("[!] PlayerScanner: no Izawa candidate found - are you on the right screen?");
        return;
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    const Candidate& best = candidates[0];

    // Find where CharaID landed among this candidate's companion matches.
    int charaOffset = INT32_MIN;
    for (int j = 0; j < best.foundCount; ++j) {
        if (strcmp(best.foundNames[j], "CharaID") == 0) { charaOffset = best.foundOffsets[j]; break; }
    }
    if (charaOffset == INT32_MIN) {
        WriteLog("[!] PlayerScanner: best candidate (score %d) has no CharaID match, can't anchor the dump", best.score);
        return;
    }
    uintptr_t recordBase = best.primaryAddr + charaOffset;
    g_lastFoundRecordAddr = recordBase;

    FILE* out = nullptr;
    fopen_s(&out, "CT2_ArrayDump_Report.txt", "w");
    if (!out) { WriteLog("[!] PlayerScanner: could not open dump report file"); return; }
    fprintf(out, "Array dump around Izawa (score %d), record_base=0x%llX, assumed stride=0x%zX\n",
            best.score, (unsigned long long)recordBase, kAssumedStride);
    fprintf(out, "Columns after the name are raw int32 at +04..+28 relative to each record's start.\n\n");
    DumpArraySequence(recordBase, 15, 60, out);

    // CharaID=1 should be the true array base if CharaIDs start at 1 with
    // no gaps (true for every session so far) - walk back from Izawa (16).
    uintptr_t arrayBase = recordBase - static_cast<uintptr_t>(15) * kAssumedStride;
    int32_t firstId = 0;
    if (SafeReadI32(arrayBase, firstId) && firstId == 1) {
        ScanForArrayPointer(arrayBase, out);
    } else {
        fprintf(out, "\n[!] record_base - 15*stride didn't land on CharaID=1 (got %d) - "
                      "skipping pointer scan, the array may not start where expected.\n", firstId);
    }

    fclose(out);
    WriteLog("[+] PlayerScanner: array dump + pointer scan written to CT2_ArrayDump_Report.txt (base=0x%llX)",
             (unsigned long long)recordBase);
}

inline void RunScanAndReport() {
    WriteLog("[i] PlayerScanner: F8 pressed, scanning process memory (may take a few seconds)...");
    DWORD t0 = GetTickCount();

    FILE* report = nullptr;
    fopen_s(&report, "CT2_PlayerScan_Report.txt", "w");
    FILE* raw = nullptr;
    fopen_s(&raw, "CT2_PlayerScan_Raw.txt", "w");

    if (!report) { WriteLog("[!] PlayerScanner: could not open report file"); return; }

    fprintf(report, "CT2 Player Scanner report\n");
    fprintf(report, "==========================\n");
    fprintf(report, "Window around each primary hit: +/- 0x%X bytes. Score = how many OTHER\n", kWindowBytes);
    fprintf(report, "known fields from the same fingerprint were found nearby (higher = more\n");
    fprintf(report, "likely a real struct record, not coincidence).\n\n");

    int totalCandidates = 0;
    for (int i = 0; i < kFingerprintCount; ++i) {
        const auto& fp = g_fingerprints[i];
        auto candidates = ScanFingerprint(fp, raw);
        // best candidates first
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return a.score > b.score;
        });

        fprintf(report, "---- %s (CharaID %d) ---- %zu candidate(s) with score>=2\n",
                fp.label, fp.charaId, candidates.size());

        int shown = 0;
        for (const auto& c : candidates) {
            if (shown >= 15) { fprintf(report, "  ... %zu more, see pattern above\n", candidates.size() - shown); break; }
            fprintf(report, "  primary(%s=%d) @ 0x%llX  score=%d\n",
                    "value", fp.primary, (unsigned long long)c.primaryAddr, c.score);
            for (int j = 0; j < c.foundCount; ++j) {
                fprintf(report, "      %-8s found at offset %+d\n", c.foundNames[j], c.foundOffsets[j]);
            }
            DumpAround(report, c.primaryAddr);
            fprintf(report, "\n");
            ++shown;
            ++totalCandidates;
        }
        fprintf(report, "\n");
    }

    DWORD elapsed = GetTickCount() - t0;
    fprintf(report, "Scan finished in %lu ms. Total high-confidence candidates: %d\n", elapsed, totalCandidates);
    fclose(report);
    if (raw) fclose(raw);

    WriteLog("[+] PlayerScanner: done in %lu ms, %d candidate(s) written to CT2_PlayerScan_Report.txt",
             elapsed, totalCandidates);
}

} // namespace PlayerScanner
