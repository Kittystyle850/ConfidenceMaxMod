// aob_scan.h
// Minimal AOB (array-of-bytes) pattern scanner for in-process module memory.
// Mirrors Cheat Engine's aobscanmodule() semantics: a space-separated hex
// pattern where "??" is a wildcard byte, scanned linearly over a module's
// mapped image. This is what makes the patch below version-portable: it
// finds the patch sites by their surrounding byte context, not by a fixed
// address, so a game update that shifts code around (but keeps the same
// instruction sequence) does not break the mod.

#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

struct PatternByte {
    uint8_t value;
    bool wildcard;
};

inline std::vector<PatternByte> ParsePattern(const char* pattern) {
    std::vector<PatternByte> out;
    std::string tok;
    auto flush = [&]() {
        if (tok.empty()) return;
        if (tok == "??" || tok == "?") {
            out.push_back({0, true});
        } else {
            out.push_back({ (uint8_t)strtoul(tok.c_str(), nullptr, 16), false });
        }
        tok.clear();
    };
    for (const char* p = pattern; *p; ++p) {
        if (*p == ' ') flush();
        else tok += *p;
    }
    flush();
    return out;
}

// Scans [base, base+size) for the first occurrence of pattern.
// Returns pointer to the match start, or nullptr if not found.
inline uint8_t* FindPattern(uint8_t* base, size_t size, const std::vector<PatternByte>& pattern) {
    if (pattern.empty() || size < pattern.size()) return nullptr;
    const size_t last = size - pattern.size();
    for (size_t i = 0; i <= last; ++i) {
        bool ok = true;
        for (size_t j = 0; j < pattern.size(); ++j) {
            if (!pattern[j].wildcard && base[i + j] != pattern[j].value) { ok = false; break; }
        }
        if (ok) return base + i;
    }
    return nullptr;
}

// Scans the whole mapped image of hMod (like aobscanmodule) using the PE
// header's SizeOfImage, which covers .text/.rdata/etc. in one pass.
inline uint8_t* FindPatternInModule(HMODULE hMod, const char* pattern) {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hMod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>((uint8_t*)hMod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    size_t imageSize = nt->OptionalHeader.SizeOfImage;
    auto parsed = ParsePattern(pattern);
    return FindPattern(reinterpret_cast<uint8_t*>(hMod), imageSize, parsed);
}

// Verifies that `expected` bytes match at `addr` (mirrors CT's assert()).
inline bool VerifyBytes(uint8_t* addr, std::initializer_list<uint8_t> expected) {
    size_t i = 0;
    for (uint8_t b : expected) {
        if (addr[i] != b) return false;
        ++i;
    }
    return true;
}

// Writes `bytes` at `addr`, temporarily flipping the page to PAGE_EXECUTE_READWRITE
// and restoring the original protection afterward.
inline bool PatchBytes(uint8_t* addr, std::initializer_list<uint8_t> bytes) {
    DWORD oldProt;
    if (!VirtualProtect(addr, bytes.size(), PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    size_t i = 0;
    for (uint8_t b : bytes) { addr[i++] = b; }
    DWORD tmp;
    VirtualProtect(addr, bytes.size(), oldProt, &tmp);
    FlushInstructionCache(GetCurrentProcess(), addr, bytes.size());
    return true;
}
