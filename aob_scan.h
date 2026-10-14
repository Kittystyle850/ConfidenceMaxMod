// aob_scan.h - AOB scanner + patch/hook helpers for in-process use.
// Same idea as the base repo (aobscanmodule semantics, "??" wildcards) but:
//   * walks the module with VirtualQuery and only reads committed, readable
//     pages (no AV on guard/no-access pages),
//   * can restrict to executable pages (code AOBs) or any readable page,
//   * accepts a "verify" callback so a short pattern with several matches
//     only resolves to the match whose surrounding bytes are the expected ones.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

struct PatternByte { uint8_t value; bool wildcard; };

inline std::vector<PatternByte> ParsePattern(const char* pattern) {
    std::vector<PatternByte> out;
    std::string tok;
    auto flush = [&]() {
        if (tok.empty()) return;
        if (tok == "??" || tok == "?" || tok == "**") out.push_back({0, true});
        else out.push_back({ (uint8_t)strtoul(tok.c_str(), nullptr, 16), false });
        tok.clear();
    };
    for (const char* p = pattern; *p; ++p) {
        if (*p == ' ') flush(); else tok += *p;
    }
    flush();
    return out;
}

struct MemRun { uint8_t* begin; size_t size; };

inline bool ModuleExtent(HMODULE mod, uint8_t*& base, size_t& size) {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>((uint8_t*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    base = reinterpret_cast<uint8_t*>(mod);
    size = nt->OptionalHeader.SizeOfImage;
    return true;
}

inline bool ProtIsReadable(DWORD p) {
    if (p & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                 PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}
inline bool ProtIsExecReadable(DWORD p) {
    if (p & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (p & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Contiguous readable (or exec+readable) runs inside the module image.
inline std::vector<MemRun> ModuleRuns(HMODULE mod, bool execOnly) {
    std::vector<MemRun> runs;
    uint8_t* base; size_t size;
    if (!ModuleExtent(mod, base, size)) return runs;
    uint8_t* p = base;
    uint8_t* end = base + size;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        uint8_t* rb = (uint8_t*)mbi.BaseAddress;
        uint8_t* re = rb + mbi.RegionSize;
        if (re > end) re = end;
        bool ok = mbi.State == MEM_COMMIT &&
                  (execOnly ? ProtIsExecReadable(mbi.Protect) : ProtIsReadable(mbi.Protect));
        if (ok) {
            uint8_t* s = rb < p ? p : rb;
            if (!runs.empty() && runs.back().begin + runs.back().size == s)
                runs.back().size += (size_t)(re - s);
            else
                runs.push_back({ s, (size_t)(re - s) });
        }
        if (re <= p) break;
        p = re;
    }
    return runs;
}

// First match of `pattern` for which verify(hit) is true.
template <typename Verify>
inline uint8_t* FindPatternVerified(HMODULE mod, const char* pattern, bool execOnly, Verify verify) {
    auto pat = ParsePattern(pattern);
    if (pat.empty()) return nullptr;
    size_t k = 0;
    while (k < pat.size() && pat[k].wildcard) ++k;
    if (k == pat.size()) return nullptr;

    for (const MemRun& run : ModuleRuns(mod, execOnly)) {
        if (run.size < pat.size()) continue;
        const uint8_t* d = run.begin;
        const size_t last = run.size - pat.size();
        for (size_t i = 0; i <= last; ++i) {
            if (d[i + k] != pat[k].value) continue;
            bool ok = true;
            for (size_t j = 0; j < pat.size(); ++j) {
                if (!pat[j].wildcard && d[i + j] != pat[j].value) { ok = false; break; }
            }
            if (ok && verify(run.begin + i)) return run.begin + i;
        }
    }
    return nullptr;
}

inline uint8_t* FindPatternInModule(HMODULE mod, const char* pattern, bool execOnly = true) {
    return FindPatternVerified(mod, pattern, execOnly, [](uint8_t*) { return true; });
}

// True if memory at addr equals `expected` (CT assert()).
inline bool BytesEqual(const uint8_t* addr, const uint8_t* expected, size_t n) {
    return memcmp(addr, expected, n) == 0;
}

// Writes bytes into (possibly read-only) code and restores protection.
inline bool WriteCode(uint8_t* addr, const uint8_t* bytes, size_t n) {
    DWORD oldProt;
    if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    memcpy(addr, bytes, n);
    DWORD tmp;
    VirtualProtect(addr, n, oldProt, &tmp);
    FlushInstructionCache(GetCurrentProcess(), addr, n);
    return true;
}

// Allocates RWX memory within +-2GB of the module so a 5-byte "jmp rel32"
// placed inside the module can reach it (what CE's alloc(newmem,$1000,inj) does).
inline uint8_t* AllocNearModule(HMODULE mod, size_t size) {
    uint8_t* base; size_t msize;
    if (!ModuleExtent(mod, base, msize)) return nullptr;
    const uintptr_t gran = 0x10000;
    const uintptr_t span = 0x70000000;
    auto tryRange = [&](uintptr_t from, uintptr_t to, bool up) -> uint8_t* {
        uintptr_t a = from;
        while (up ? a < to : a > to) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((void*)a, &mbi, sizeof(mbi))) break;
            if (mbi.State == MEM_FREE) {
                uintptr_t cand = up ? ((uintptr_t)mbi.BaseAddress + gran - 1) & ~(gran - 1)
                                    : ((uintptr_t)mbi.BaseAddress + mbi.RegionSize - size) & ~(gran - 1);
                if (cand >= (uintptr_t)mbi.BaseAddress &&
                    cand + size <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
                    void* m = VirtualAlloc((void*)cand, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                    if (m) return (uint8_t*)m;
                }
            }
            if (up) a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            else    a = (uintptr_t)mbi.BaseAddress - 1;
        }
        return nullptr;
    };
    uintptr_t b = (uintptr_t)base, e = b + msize;
    if (uint8_t* r = tryRange(e, e + span, true)) return r;
    if (b > span) { if (uint8_t* r = tryRange(b - 1, b - span, false)) return r; }
    return nullptr;
}
