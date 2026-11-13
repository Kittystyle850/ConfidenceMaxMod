// Onimusha: Way of the Sword - standalone mod DLL.
// Combines animation_cancel.lua + instant_parry.lua.
// Target: Steam 24769601 / 01.000.001, TimeDateStamp 0x6A827CEA, SizeOfImage 0x249F0000.

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_map>
#include "MinHook.h"

// ─── Build identity ────────────────────────────────────────
static constexpr DWORD     kTimestamp   = 0x6A827CEA;
static constexpr DWORD     kSizeOfImage = 0x249F0000;
static constexpr uintptr_t kBase        = 0x140000000;

// ─── RVAs (from REFramework dump: absolute - 0x140000000) ──
static constexpr uintptr_t RVA_PACC_LATEUPDATE      = 0x49A55A0;
static constexpr uintptr_t RVA_PCC_UPDATE           = 0x2015F0;
static constexpr uintptr_t RVA_PAS_UPDATE           = 0x455340;
static constexpr uintptr_t RVA_PCE_CHECKCANCEL_T    = 0x4B23C10;
static constexpr uintptr_t RVA_PCE_CHECKPRECANCEL_T = 0x4B23C50;
static constexpr uintptr_t RVA_PCE_CHECKIGNORE      = 0x50040E0;
static constexpr uintptr_t RVA_PU_CHECKCANCEL_T     = 0x4B93580;
static constexpr uintptr_t RVA_PU_CHECKPRECANCEL_T  = 0x7938030;
static constexpr uintptr_t RVA_JFU_UPDATE           = 0x40D950;
static constexpr uintptr_t RVA_JFU_GETJUSTFRAME     = 0x5101170;
static constexpr uintptr_t RVA_CB_GETACTIONSTATE    = 0x49A1D10;
static constexpr uintptr_t RVA_JDS_ISACCEPTING      = 0x760A450;

// ─── Field offsets ────────────────────────────────────────
static constexpr uintptr_t OFF_CCCMD_PHASE   = 0x10;   // cCancelCommand._Phase
static constexpr uintptr_t OFF_PACC_PCE      = 0x60;   // PlayerActionCancelChecker._PlayerCharacterEntity
static constexpr uintptr_t OFF_PACC_PC       = 0x58;   // PlayerActionCancelChecker._PlayerCharacter
static constexpr uintptr_t OFF_PCM_MASKS     = 0x10;   // cPlayerCommandMask._CommandMasks
static constexpr uintptr_t OFF_BITSET_VALUE  = 0x10;   // ace.Bitset._Value
static constexpr uintptr_t OFF_PCE_SEQ       = 0x5B8;  // PCE._PlayerCommandCancelMotionSeq
static constexpr uintptr_t OFF_PCE_JDS       = 0x3C8;  // PCE.JustDodgeSupporter
static constexpr uintptr_t OFF_JFU_PARAM     = 0x38;   // JFU._JustActionParam
static constexpr uintptr_t OFF_PARAM_GUARD   = 0xC8;
static constexpr uintptr_t OFF_PARAM_ISSEN   = 0xCC;
static constexpr uintptr_t OFF_PARAM_GRAB    = 0xD8;
static constexpr uintptr_t OFF_PARAM_DODGE   = 0xDC;
static constexpr uintptr_t OFF_ARR_COUNT     = 0x1C;   // managed array: element count
static constexpr uintptr_t OFF_ARR_DATA      = 0x20;   // managed array: first element

// ─── Constants ────────────────────────────────────────────
static constexpr int      PHASE_ACTUAL     = 2;
static constexpr uint64_t GRAPPLE_BIT      = 0x80000ull;
static constexpr uint64_t ATTACK_BIT       = 0x40000ull;
static constexpr uint64_t DAMAGE_BIT       = 0x100000ull;
static constexpr uint64_t SMASH_BIT        = 0x200000ull;
static constexpr uint64_t BLOCKED_BIT      = 0x400000ull;
static constexpr uint64_t GUARD_BREAK_BIT  = 0x4000000ull;
static constexpr uint64_t BLOCK_BIT        = 0x8000000ull;
static constexpr uint64_t PARRY_BIT        = 0x10000000ull;
static constexpr uint64_t EVADE_BIT        = 0x20000000ull;

// ─── Command enum (from dump) ─────────────────────────────
enum Cmd : uint32_t {
    CMD_ONI_CHANGE=0, CMD_ONI_EYE=1, CMD_PARRY=2, CMD_BLOCK=3, CMD_JUST_DODGE=4,
    CMD_GRAB_COUNTER=5, CMD_ISSEN_COUNTER=6, CMD_ISSEN_BREAK=7, CMD_ISSEN_BLOCK=8,
    CMD_ISSEN_CHAIN=9, CMD_GRAPPLE_BLOW=10, CMD_SKILL=11, CMD_SKILL_KEEP=12,
    CMD_ULTIMATE=13, CMD_ABSORB=14, CMD_BOW=15, CMD_GUARD=16, CMD_ATTACK_BLOW=17,
    CMD_DODGE=18, CMD_ATTACK_JUST_DODGE=19, CMD_ATTACK_SNEAK=20,
    CMD_ATTACK_LIGHT_CHARGE=21, CMD_ATTACK_LIGHT_COMBO=22, CMD_ATTACK_LIGHT=23,
    CMD_ATTACK_HARD_CHARGE=24, CMD_ATTACK_HARD_COMBO=25, CMD_ATTACK_HARD=26,
    CMD_MEDICINE=27, CMD_ITEM=28, CMD_INTERACT=29, CMD_INTERACT_KEEP=30,
    CMD_DASH=31, CMD_DASH_KEEP=32, CMD_RUN=33, CMD_RUN_TURN=34, CMD_WALK=35,
    CMD_WALK_KEEP=36, CMD_WALK_STICK=37, CMD_QUICK_STOP=38, CMD_MENU_ITEM=39,
    CMD_MENU_SKILL=40, CMD_LOCK_ON=41,
};

enum Cat { CAT_ATTACK, CAT_DODGE, CAT_GUARD, CAT_MOVE, CAT_ISSEN, CAT_OTHER, CAT_COUNT };

struct KeyDef { uint32_t cmd; uintptr_t offset; Cat cat; };
static const KeyDef kKeys[] = {
    { CMD_ONI_CHANGE,         0x50,  CAT_OTHER  },
    { CMD_ONI_EYE,            0x58,  CAT_OTHER  },
    { CMD_PARRY,              0x60,  CAT_GUARD  },
    { CMD_BLOCK,              0x68,  CAT_GUARD  },
    { CMD_JUST_DODGE,         0x70,  CAT_DODGE  },
    { CMD_GRAB_COUNTER,       0x78,  CAT_ISSEN  },
    { CMD_ISSEN_COUNTER,      0x80,  CAT_ISSEN  },
    { CMD_ISSEN_BREAK,        0x88,  CAT_ISSEN  },
    { CMD_ISSEN_BLOCK,        0x90,  CAT_ISSEN  },
    { CMD_ISSEN_CHAIN,        0x98,  CAT_ISSEN  },
    { CMD_GRAPPLE_BLOW,       0xA0,  CAT_OTHER  },
    { CMD_SKILL,              0xA8,  CAT_ATTACK },
    { CMD_SKILL_KEEP,         0xB0,  CAT_ATTACK },
    { CMD_ULTIMATE,           0xB8,  CAT_ATTACK },
    { CMD_ABSORB,             0xC0,  CAT_OTHER  },
    { CMD_BOW,                0xC8,  CAT_ATTACK },
    { CMD_GUARD,              0xD0,  CAT_GUARD  },
    { CMD_ATTACK_BLOW,        0xD8,  CAT_ATTACK },
    { CMD_DODGE,              0xE0,  CAT_DODGE  },
    { CMD_ATTACK_JUST_DODGE,  0xE8,  CAT_DODGE  },  // dodge per Lua classify order
    { CMD_ATTACK_SNEAK,       0xF0,  CAT_ATTACK },
    { CMD_ATTACK_LIGHT_CHARGE,0xF8,  CAT_ATTACK },
    { CMD_ATTACK_LIGHT_COMBO, 0x100, CAT_ATTACK },
    { CMD_ATTACK_LIGHT,       0x108, CAT_ATTACK },
    { CMD_ATTACK_HARD_CHARGE, 0x110, CAT_ATTACK },
    { CMD_ATTACK_HARD_COMBO,  0x118, CAT_ATTACK },
    { CMD_ATTACK_HARD,        0x120, CAT_ATTACK },
    { CMD_MEDICINE,           0x128, CAT_OTHER  },
    { CMD_ITEM,               0x130, CAT_OTHER  },
    { CMD_INTERACT,           0x138, CAT_OTHER  },
    { CMD_INTERACT_KEEP,      0x140, CAT_OTHER  },
    { CMD_DASH,               0x148, CAT_MOVE   },
    { CMD_DASH_KEEP,          0x150, CAT_MOVE   },
    { CMD_RUN,                0x158, CAT_MOVE   },
    { CMD_RUN_TURN,           0x160, CAT_MOVE   },
    { CMD_WALK,               0x168, CAT_MOVE   },
    { CMD_WALK_KEEP,          0x170, CAT_MOVE   },
    { CMD_WALK_STICK,         0x178, CAT_MOVE   },
    { CMD_QUICK_STOP,         0x180, CAT_MOVE   },
    { CMD_MENU_ITEM,          0x188, CAT_OTHER  },
    { CMD_MENU_SKILL,         0x190, CAT_OTHER  },
    { CMD_LOCK_ON,            0x198, CAT_MOVE   },
};
static constexpr size_t kKeysCount = sizeof(kKeys) / sizeof(kKeys[0]);

static inline Cat CatOfCmd(uint32_t cmd) {
    for (size_t i = 0; i < kKeysCount; i++) if (kKeys[i].cmd == cmd) return kKeys[i].cat;
    return CAT_OTHER;
}

// ─── Config ───────────────────────────────────────────────
struct Config {
    bool enabled = false;
    bool cat[CAT_COUNT] = { false, false, false, false, false, false };
    int  lock_guard = 0, lock_issen = -1, lock_grab = -1, lock_dodge = -1;
    float wm_guard = 1.0f, wm_issen = 1.0f, wm_grab = 1.0f, wm_dodge = 1.0f;
    bool log_on = true;
} g_cfg;

// ─── Runtime state ────────────────────────────────────────
struct LayerKeys { void* key_obj; uintptr_t offset; Cat cat; };
struct RuntimeState {
    void*    entity = nullptr;
    void*    player = nullptr;
    void*    seq = nullptr;
    uint32_t seq_count = 0;
    std::unordered_map<uintptr_t, std::vector<LayerKeys>> layer_cache;
    void*    jfu = nullptr;
    void*    jparam = nullptr;
    int      orig_guard = -1, orig_issen = -1, orig_grab = -1, orig_dodge = -1;
    bool     jd_open = false;
    bool     held = false;
    uint64_t frame = 0;
    uint64_t state_frame = 0;
    uint64_t frame_seen = 0;
} g_rt;

// ─── Logging ──────────────────────────────────────────────
static std::string g_log_path;
static void Log(const char* fmt, ...) {
    if (!g_cfg.log_on) return;
    char msg[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    FILE* f = fopen(g_log_path.c_str(), "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] %s\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    fclose(f);
}

// ─── Safe memory read ─────────────────────────────────────
template<typename T> static inline T Rd(void* base, uintptr_t off) {
    return *(T*)((uintptr_t)base + off);
}
template<typename T> static inline void Wr(void* base, uintptr_t off, T v) {
    *(T*)((uintptr_t)base + off) = v;
}

// ─── Config loader ────────────────────────────────────────
static void LoadConfig() {
    char path[MAX_PATH];
    HMODULE self = GetModuleHandleA("OnimushaMod.dll");
    if (!self) self = GetModuleHandleA(nullptr);
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string ini = path;
    auto dot = ini.find_last_of('.');
    ini = (dot == std::string::npos) ? ini + ".ini" : ini.substr(0, dot) + ".ini";

    auto I = [&](const char* s, const char* k, int def) {
        return (int)GetPrivateProfileIntA(s, k, def, ini.c_str());
    };
    auto F = [&](const char* s, const char* k, float def) {
        char buf[64]; GetPrivateProfileStringA(s, k, "", buf, sizeof(buf), ini.c_str());
        if (!buf[0]) return def;
        return (float)atof(buf);
    };
    g_cfg.enabled       = I("AnimationCancel", "Enabled", 0) != 0;
    g_cfg.cat[CAT_ATTACK] = I("AnimationCancel", "Attacks", 0) != 0;
    g_cfg.cat[CAT_DODGE]  = I("AnimationCancel", "Dodge",   0) != 0;
    g_cfg.cat[CAT_GUARD]  = I("AnimationCancel", "Guard",   0) != 0;
    g_cfg.cat[CAT_MOVE]   = I("AnimationCancel", "Move",    0) != 0;
    g_cfg.cat[CAT_ISSEN]  = I("AnimationCancel", "Issen",   0) != 0;
    g_cfg.cat[CAT_OTHER]  = I("AnimationCancel", "Other",   0) != 0;
    g_cfg.lock_guard = I("InstantParry", "GuardLockout", 0);
    g_cfg.lock_issen = I("InstantParry", "IssenLockout", -1);
    g_cfg.lock_grab  = I("InstantParry", "GrabLockout", -1);
    g_cfg.lock_dodge = I("InstantParry", "DodgeLockout", -1);
    g_cfg.wm_guard   = F("InstantParry", "GuardWindow", 1.0f);
    g_cfg.wm_issen   = F("InstantParry", "IssenWindow", 1.0f);
    g_cfg.wm_grab    = F("InstantParry", "GrabWindow", 1.0f);
    g_cfg.wm_dodge   = F("InstantParry", "DodgeWindow", 1.0f);
    g_cfg.log_on     = I("Debug", "Log", 1) != 0;

    Log("[cfg] enabled=%d atk=%d ddg=%d grd=%d mv=%d isn=%d oth=%d lock(g%d i%d gr%d d%d)",
        g_cfg.enabled, g_cfg.cat[0], g_cfg.cat[1], g_cfg.cat[2], g_cfg.cat[3], g_cfg.cat[4], g_cfg.cat[5],
        g_cfg.lock_guard, g_cfg.lock_issen, g_cfg.lock_grab, g_cfg.lock_dodge);
}

// ─── State update (once per frame) ────────────────────────
static int64_t GetActionState() {
    if (!g_rt.player) return 0;
    typedef int64_t(*Fn)(void*);
    auto fn = (Fn)(kBase + RVA_CB_GETACTIONSTATE);
    __try { return fn(g_rt.player); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static bool IsJDWindowOpen() {
    if (!g_rt.entity) return false;
    void* jds = Rd<void*>(g_rt.entity, OFF_PCE_JDS);
    if (!jds) return false;
    typedef bool(*Fn)(void*);
    auto fn = (Fn)(kBase + RVA_JDS_ISACCEPTING);
    __try { return fn(jds); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void RefreshSeq() {
    if (!g_rt.entity) return;
    void* seq = Rd<void*>(g_rt.entity, OFF_PCE_SEQ);
    if (seq == g_rt.seq) return;
    g_rt.seq = seq;
    g_rt.seq_count = 0;
    g_rt.layer_cache.clear();
    if (seq) {
        int32_t cnt = Rd<int32_t>(seq, OFF_ARR_COUNT);
        if (cnt > 0 && cnt < 128) g_rt.seq_count = (uint32_t)cnt;
    }
    Log("[state] seq=%p layers=%u", seq, g_rt.seq_count);
}

static void UpdateState() {
    if (g_rt.state_frame == g_rt.frame) return;
    g_rt.state_frame = g_rt.frame;
    if (!g_cfg.enabled) return;

    RefreshSeq();
    int64_t state = GetActionState();

    g_rt.jd_open = IsJDWindowOpen();
    bool synced = (state & GRAPPLE_BIT) != 0;
    bool parry  = (state & (PARRY_BIT | BLOCK_BIT)) != 0;
    bool damage = (state & (DAMAGE_BIT | SMASH_BIT | BLOCKED_BIT | GUARD_BREAK_BIT)) != 0;

    bool held_new = synced || parry || damage || g_rt.jd_open;
    if (held_new != g_rt.held) {
        Log("[state] held=%d (sync=%d parry=%d damage=%d jd=%d)",
            held_new, synced, parry, damage, g_rt.jd_open);
    }
    g_rt.held = held_new;
}

// ─── Force phases (animation_cancel core) ─────────────────
static void ForcePhases() {
    if (!g_rt.seq || !g_cfg.enabled) return;
    if (g_rt.held) return;
    uintptr_t* arr = (uintptr_t*)g_rt.seq;
    for (uint32_t i = 0; i < g_rt.seq_count; i++) {
        void* layer = (void*)arr[OFF_ARR_DATA / 8 + i];
        if (!layer) continue;
        auto it = g_rt.layer_cache.find((uintptr_t)layer);
        if (it == g_rt.layer_cache.end()) {
            std::vector<LayerKeys> keys;
            keys.reserve(kKeysCount);
            for (const auto& kd : kKeys) {
                void* ko = Rd<void*>(layer, kd.offset);
                if (ko) keys.push_back({ ko, kd.offset, kd.cat });
            }
            it = g_rt.layer_cache.emplace((uintptr_t)layer, std::move(keys)).first;
            Log("[phases] cached layer %p with %zu keys", layer, it->second.size());
        }
        for (auto& k : it->second) {
            if (!g_cfg.cat[k.cat]) continue;
            Wr<int32_t>(k.key_obj, OFF_CCCMD_PHASE, PHASE_ACTUAL);
        }
    }
}

// ─── Mask clearing ────────────────────────────────────────
static void Unmask(void* mask_obj) {
    if (!mask_obj || !g_cfg.enabled || g_rt.held) return;
    bool any = false;
    for (int i = 0; i < CAT_COUNT; i++) if (g_cfg.cat[i]) any = true;
    if (!any) return;
    void* bitset = Rd<void*>(mask_obj, OFF_PCM_MASKS);
    if (!bitset) return;
    void* arr = Rd<void*>(bitset, OFF_BITSET_VALUE);
    if (!arr) return;
    int32_t nwords = Rd<int32_t>(arr, OFF_ARR_COUNT);
    if (nwords <= 0 || nwords > 8) return;
    uint32_t* words = (uint32_t*)((uintptr_t)arr + OFF_ARR_DATA);
    for (const auto& kd : kKeys) {
        if (!g_cfg.cat[kd.cat]) continue;
        uint32_t w = kd.cmd >> 5;
        uint32_t bit = 1u << (kd.cmd & 31);
        if (w < (uint32_t)nwords) words[w] &= ~bit;
    }
}

// ─── Lockout override ─────────────────────────────────────
struct LockF { uintptr_t offset; int* target; };
static LockF kLockF[] = {
    { OFF_PARAM_GUARD, &g_cfg.lock_guard },
    { OFF_PARAM_ISSEN, &g_cfg.lock_issen },
    { OFF_PARAM_GRAB,  &g_cfg.lock_grab  },
    { OFF_PARAM_DODGE, &g_cfg.lock_dodge },
};

static void AdoptJFU(void* jfu) {
    if (jfu == g_rt.jfu) return;
    g_rt.jfu = jfu;
    void* param = Rd<void*>(jfu, OFF_JFU_PARAM);
    if (param == g_rt.jparam) return;
    g_rt.jparam = param;
    if (param) {
        g_rt.orig_guard = Rd<int32_t>(param, OFF_PARAM_GUARD);
        g_rt.orig_issen = Rd<int32_t>(param, OFF_PARAM_ISSEN);
        g_rt.orig_grab  = Rd<int32_t>(param, OFF_PARAM_GRAB);
        g_rt.orig_dodge = Rd<int32_t>(param, OFF_PARAM_DODGE);
        Log("[parry] adopted param %p: guard=%d issen=%d grab=%d dodge=%d",
            param, g_rt.orig_guard, g_rt.orig_issen, g_rt.orig_grab, g_rt.orig_dodge);
    }
}

static void ApplyLockout() {
    if (!g_rt.jparam) return;
    for (int i = 0; i < 4; i++) {
        if (*kLockF[i].target >= 0) Wr<int32_t>(g_rt.jparam, kLockF[i].offset, *kLockF[i].target);
    }
}

// ─── Original function pointers ───────────────────────────
typedef void    (*LateUpdateFn)(void*);
typedef void    (*CtrlUpdateFn)(void*, void*);
typedef void    (*SelUpdateFn)(void*);
typedef bool    (*CheckMotionFn)(void*, uint32_t);
typedef void    (*JFUUpdateFn)(void*);
typedef float   (*GetJustFrameFn)(void*, uint32_t, uint32_t, void*, void*);

static LateUpdateFn   o_LateUpdate = nullptr;
static CtrlUpdateFn   o_CtrlUpdate = nullptr;
static SelUpdateFn    o_SelUpdate  = nullptr;
static CheckMotionFn  o_PCCancel   = nullptr;
static CheckMotionFn  o_PCPreCancel= nullptr;
static CheckMotionFn  o_PCIgnore   = nullptr;
static CheckMotionFn  o_PUCancel   = nullptr;
static CheckMotionFn  o_PUPreCancel= nullptr;
static JFUUpdateFn    o_JFUUpdate  = nullptr;
static GetJustFrameFn o_GetJustFrame= nullptr;

// ─── Helpers ──────────────────────────────────────────────
static bool ShouldOverride(uint32_t cmd) {
    if (!g_cfg.enabled || g_rt.held) return false;
    Cat c = CatOfCmd(cmd);
    return g_cfg.cat[c];
}

// ─── Hook implementations ─────────────────────────────────
static void hk_LateUpdate(void* self) {
    g_rt.frame++;
    if (self) {
        void* pce = Rd<void*>(self, OFF_PACC_PCE);
        if (pce != g_rt.entity) {
            g_rt.entity = pce;
            g_rt.player = Rd<void*>(self, OFF_PACC_PC);
            Log("[state] entity=%p player=%p", pce, g_rt.player);
        }
    }
    UpdateState();
    ForcePhases();
    o_LateUpdate(self);
}

static void hk_CtrlUpdate(void* self, void* mask) {
    UpdateState();
    ForcePhases();
    Unmask(mask);
    o_CtrlUpdate(self, mask);
}

static void hk_SelUpdate(void* self) {
    UpdateState();
    ForcePhases();
    o_SelUpdate(self);
}

static bool hk_PCCancel(void* self, uint32_t cmd) {
    bool orig = o_PCCancel(self, cmd);
    return ShouldOverride(cmd) ? true : orig;
}
static bool hk_PCPreCancel(void* self, uint32_t cmd) {
    bool orig = o_PCPreCancel(self, cmd);
    return ShouldOverride(cmd) ? true : orig;
}
static bool hk_PCIgnore(void* self, uint32_t cmd) {
    bool orig = o_PCIgnore(self, cmd);
    return ShouldOverride(cmd) ? false : orig;
}
static bool hk_PUCancel(void* self, uint32_t cmd) {
    bool orig = o_PUCancel(self, cmd);
    return ShouldOverride(cmd) ? true : orig;
}
static bool hk_PUPreCancel(void* self, uint32_t cmd) {
    bool orig = o_PUPreCancel(self, cmd);
    return ShouldOverride(cmd) ? true : orig;
}

static void hk_JFUUpdate(void* self) {
    AdoptJFU(self);
    ApplyLockout();
    o_JFUUpdate(self);
}

static float hk_GetJustFrame(void* self, uint32_t type, uint32_t cat,
                             void* gameObj, void* rank) {
    float orig = o_GetJustFrame(self, type, cat, gameObj, rank);
    if (!g_cfg.enabled || orig <= 0.0f) return orig;
    float m = 1.0f;
    switch (type) {
        case 0: m = g_cfg.wm_issen; break;
        case 1: m = g_cfg.wm_grab;  break;
        case 2: m = g_cfg.wm_guard; break;
        case 3: m = g_cfg.wm_guard; break;
        case 4: m = g_cfg.wm_dodge; break;
        default: return orig;
    }
    return (m == 1.0f) ? orig : orig * m;
}

// ─── Install ──────────────────────────────────────────────
static bool InstallAll() {
    if (MH_Initialize() != MH_OK) { Log("[-] MH_Initialize failed"); return false; }
    auto H = [](uintptr_t rva, void* fn, void** orig, const char* name) {
        void* target = (void*)(kBase + rva);
        MH_STATUS s1 = MH_CreateHook(target, fn, orig);
        MH_STATUS s2 = (s1 == MH_OK) ? MH_EnableHook(target) : s1;
        if (s2 != MH_OK) { Log("[-] %s hook failed (%d)", name, (int)s2); return false; }
        Log("[+] %s hooked @ 0x%llX", name, (unsigned long long)rva);
        return true;
    };
    return
        H(RVA_PACC_LATEUPDATE,      (void*)hk_LateUpdate,   (void**)&o_LateUpdate,    "PACC.lateUpdate") &&
        H(RVA_PCC_UPDATE,           (void*)hk_CtrlUpdate,   (void**)&o_CtrlUpdate,    "PCC.update") &&
        H(RVA_PAS_UPDATE,           (void*)hk_SelUpdate,    (void**)&o_SelUpdate,     "PAS.update") &&
        H(RVA_PCE_CHECKCANCEL_T,    (void*)hk_PCCancel,     (void**)&o_PCCancel,      "PCE.checkCancelMotion") &&
        H(RVA_PCE_CHECKPRECANCEL_T, (void*)hk_PCPreCancel,  (void**)&o_PCPreCancel,   "PCE.checkPreCancelMotion") &&
        H(RVA_PCE_CHECKIGNORE,      (void*)hk_PCIgnore,     (void**)&o_PCIgnore,      "PCE.checkIgnoreMotion") &&
        H(RVA_PU_CHECKCANCEL_T,     (void*)hk_PUCancel,     (void**)&o_PUCancel,      "PU.checkCancelMotion") &&
        H(RVA_PU_CHECKPRECANCEL_T,  (void*)hk_PUPreCancel,  (void**)&o_PUPreCancel,   "PU.checkPreCancelMotion") &&
        H(RVA_JFU_UPDATE,           (void*)hk_JFUUpdate,    (void**)&o_JFUUpdate,     "JFU.update") &&
        H(RVA_JFU_GETJUSTFRAME,     (void*)hk_GetJustFrame, (void**)&o_GetJustFrame,  "JFU.getJustFrame");
}

// ─── Entry ────────────────────────────────────────────────
static DWORD WINAPI MainThread(LPVOID) {
    LoadConfig();
    for (int i = 0; i < 240; i++) {
        HMODULE mod = GetModuleHandleA("OnimushaWotS.exe");
        if (!mod) { Sleep(500); continue; }
        auto* dos = (IMAGE_DOS_HEADER*)mod;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        auto* nt = (IMAGE_NT_HEADERS*)((uintptr_t)mod + dos->e_lfanew);
        if (nt->FileHeader.TimeDateStamp != kTimestamp ||
            nt->OptionalHeader.SizeOfImage != kSizeOfImage) {
            Log("[-] unsupported build (ts=%08X so=%08X)",
                nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
            return 0;
        }
        Log("[+] build verified: %p", mod);
        if (!InstallAll()) { Log("[-] install failed"); return 0; }
        Log("[+] ready");
        return 0;
    }
    Log("[-] OnimushaWotS.exe not found");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        char p[MAX_PATH];
        GetModuleFileNameA(h, p, MAX_PATH);
        g_log_path = p;
        size_t s = g_log_path.find_last_of("\\/");
        g_log_path = (s == std::string::npos ? "" : g_log_path.substr(0, s + 1)) + "OnimushaMod_log.txt";
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    return TRUE;
}