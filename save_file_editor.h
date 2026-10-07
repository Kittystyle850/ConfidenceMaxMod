// ============================================================
//  save_file_editor.h  -  direct .sav file move patcher
//  ------------------------------------------------------------
//  C++ port of the known-working Python save editor's safe path:
//  patch the fixed-size IntProperty fields (Tackle/Dribble/Shot/
//  AirShot/Saving/MiracleSaving/SuperID) in place, byte for byte,
//  no resizing. SuperType (a variable-length enum string) is
//  intentionally NOT supported here - it requires cascading size
//  adjustments through nested containers, too risky to port
//  without the original tool's own test coverage.
//
//  Effect is NOT live: the game only reads this file at startup,
//  so changes need a restart to show up (confirmed: this game has
//  no in-session "reload save" option).
// ============================================================
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

extern void WriteLog(const char* fmt, ...);

namespace SaveFileEditor {

// Exact byte layout of a UE tagged IntProperty, lifted verbatim from the
// Python tool's INT_SUFFIX constant.
inline const std::vector<uint8_t>& IntSuffix() {
    static const std::vector<uint8_t> s = {
        0x00, 0x0C,0x00,0x00,0x00,
        'I','n','t','P','r','o','p','e','r','t','y', 0x00,
        0x00,0x00,0x00,0x00,
        0x04,0x00,0x00,0x00,
        0x00
    };
    return s;
}

struct Edits {
    int tackle = -1;   // -1 = leave untouched
    int dribble = -1;
    int shot = -1;
    int airshot = -1;
    int saving = -1;
    int miracle = -1;
    int superId = -1;
};

// Finds `needle` in `hay` starting at `from`, returns -1 if absent.
inline int64_t Find(const std::vector<uint8_t>& hay, const std::vector<uint8_t>& needle, int64_t from = 0) {
    if (needle.empty() || from < 0) return -1;
    int64_t hn = static_cast<int64_t>(hay.size()), nn = static_cast<int64_t>(needle.size());
    for (int64_t i = from; i + nn <= hn; ++i) {
        if (memcmp(&hay[i], &needle[0], static_cast<size_t>(nn)) == 0) return i;
    }
    return -1;
}
inline int64_t Find(const std::vector<uint8_t>& hay, const char* asciiNeedle, int64_t from = 0) {
    std::vector<uint8_t> n(asciiNeedle, asciiNeedle + strlen(asciiNeedle));
    return Find(hay, n, from);
}

inline std::vector<uint8_t> FieldMarker(const char* key) {
    std::vector<uint8_t> m(key, key + strlen(key));
    const auto& suf = IntSuffix();
    m.insert(m.end(), suf.begin(), suf.end());
    return m;
}

// Patches one int32 field within [blockStart,blockEnd) of `data`, in place.
// No-op if the field or the sentinel value (-1, "don't touch") isn't present.
inline bool PatchInt(std::vector<uint8_t>& data, int64_t blockStart, int64_t blockEnd,
                      const char* key, int value) {
    if (value < 0) return false;
    auto marker = FieldMarker(key);
    // search only within the block
    std::vector<uint8_t> sub(data.begin() + blockStart, data.begin() + blockEnd);
    int64_t i = Find(sub, marker);
    if (i < 0) return false;
    int64_t pos = blockStart + i + static_cast<int64_t>(marker.size());
    if (pos + 4 > static_cast<int64_t>(data.size())) return false;
    int32_t v = value;
    memcpy(&data[pos], &v, 4);
    return true;
}

struct PatchResult {
    bool ok = false;
    int blocksPatched = 0;
    int fieldsPatched = 0;
    std::string error;
};

// Applies `edits` to every valid CharaID==targetChara block in the file at
// `path` (there can be more than one - different teams/slots), skips the
// Story Mode live-snapshot region exactly like the Python tool does, backs
// up the original first.
inline PatchResult ApplyToFile(const std::string& path, int targetChara, const Edits& edits) {
    PatchResult r;

    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) { r.error = "could not open save file"; return r; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    size_t readN = fread(data.data(), 1, data.size(), f);
    fclose(f);
    if (readN != data.size()) { r.error = "short read on save file"; return r; }

    if (Find(data, "normalTeamInfoList") < 0) { r.error = "normalTeamInfoList not found - wrong/corrupt file"; return r; }

    int64_t storyStart = Find(data, "StoryModeSaveData\x00");
    int64_t normalRosterStart = Find(data, "normalTeamInfoList\x00");
    int64_t storySnapshotEnd = (storyStart >= 0 && normalRosterStart > storyStart) ? normalRosterStart : -1;

    auto charaMarker = FieldMarker("CharaID");
    std::vector<int64_t> allChara;
    int64_t from = 0;
    for (;;) {
        int64_t p = Find(data, charaMarker, from);
        if (p < 0) break;
        allChara.push_back(p);
        from = p + 1;
    }
    if (allChara.empty()) { r.error = "no CharaID records found"; return r; }

    // backup before touching anything
    std::string bak = path + ".bak";
    FILE* bf = nullptr;
    if (fopen_s(&bf, bak.c_str(), "wb") == 0 && bf) {
        fwrite(data.data(), 1, data.size(), bf);
        fclose(bf);
    }

    for (size_t mi = 0; mi < allChara.size(); ++mi) {
        int64_t start = allChara[mi];
        int64_t end = (mi + 1 < allChara.size()) ? allChara[mi + 1] : static_cast<int64_t>(data.size());

        if (storySnapshotEnd >= 0 && start >= storyStart && start < storySnapshotEnd) continue; // live snapshot, skip

        // must look like a real per-player move record: backNum before EquipMove
        int64_t back = Find(data, "backNum\x00", start); // search globally then bound-check, cheap enough
        int64_t equip = Find(data, "EquipMove\x00", start);
        if (back < 0 || equip < 0 || back >= end || equip >= end || !(back < equip)) continue;

        int64_t cidPos = start + static_cast<int64_t>(charaMarker.size());
        if (cidPos + 4 > end) continue;
        int32_t cid = 0;
        memcpy(&cid, &data[cidPos], 4);
        if (cid != targetChara) continue;

        int before = r.fieldsPatched;
        if (PatchInt(data, start, end, "TackleMoveID", edits.tackle)) ++r.fieldsPatched;
        if (PatchInt(data, start, end, "dribbelMoveID", edits.dribble)) ++r.fieldsPatched;
        if (PatchInt(data, start, end, "NormalShootMoveID", edits.shot)) ++r.fieldsPatched;
        if (PatchInt(data, start, end, "AirShootMoveID", edits.airshot)) ++r.fieldsPatched;
        if (PatchInt(data, start, end, "SavingID", edits.saving)) ++r.fieldsPatched;
        if (PatchInt(data, start, end, "MiracleSavingID", edits.miracle)) ++r.fieldsPatched;

        if (edits.superId >= 0) {
            int64_t sm = Find(data, "superMoveID\x00", start);
            if (sm >= 0 && sm < end) {
                if (PatchInt(data, sm, end, "ID", edits.superId)) ++r.fieldsPatched;
            }
        }
        if (r.fieldsPatched > before) ++r.blocksPatched;
    }

    if (r.blocksPatched == 0) { r.error = "CharaID not found in any editable roster block (custom player 9901 isn't supported here - use the live tab)"; return r; }

    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) { r.error = "could not open save file for writing"; return r; }
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);

    r.ok = true;
    return r;
}

} // namespace SaveFileEditor
