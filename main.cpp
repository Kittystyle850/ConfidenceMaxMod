// Onimusha: Way of the Sword - standalone mod DLL.
// Defensive build: SEH around all game calls + crash-once-then-disable.
// Target: Steam 24769601 / 01.000.001, TimeDateStamp 0x6A827CEA,
// SizeOfImage 0x249F0000, image base 0x140000000.

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

// ─────────────────────────────────────────────────────────────
// Build identity
// ─────────────────────────────────────────────────────────────
static constexpr DWORD     kTimestamp   = 0x6A827CEA;
static constexpr DWORD     kSizeOfImage = 0x249F0000;
static constexpr uintptr_t kBase        = 0x140000000;

// ─────────────────────────────────────────────────────────────
// RVAs
// ─────────────────────────────────────────────────────────────
static constexpr uintptr_t RVA_PACC_LATEUPDATE      = 0x49A55A0;
static constexpr uintptr_t RVA_PCC_UPDATE           = 0x2015F0;
static constexpr uintptr_t RVA_PAS_UPDATE           = 0x455340;
static constexpr uintptr_t RVA_JFU_GETJUSTFRAME     = 0x9D7BB0;
static constexpr uintptr_t RVA_JFU_UPDATE           = 0x40D950;
static constexpr uintptr_t RVA_PCE_CHECKCANCEL_T    = 0x4B23C10;
static constexpr uintptr_t RVA_PCE_CHECKPRECANCEL_T = 0x4B23C50;
static constexpr uintptr_t RVA_CB_GETACTIONSTATE    = 0x49A1D10;
static constexpr uintptr_t RVA_JDS_ISACCEPTING      = 0x760A450;

// ─────────────────────────────────────────────────────────────
// Field offsets
// ─────────────────────────────────────────────────────────────
static constexpr uintptr_t OFF_CCCMD_PHASE  = 0x10;
static constexpr uintptr_t OFF_PACC_PCE     = 0x60;
static constexpr uintptr_t OFF_PACC_PC      = 0x58;
static constexpr uintptr_t OFF_PCM_MASKS    = 0x10;
static constexpr uintptr_t OFF_BITSET_VALUE = 0x10;
static constexpr uintptr_t OFF_PCE_SEQ      = 0x5B8;
static constexpr uintptr_t OFF_PCE_JDS      = 0x3C8;
static constexpr uintptr_t OFF_JFU_PARAM    = 0x38;
static constexpr uintptr_t OFF_PARAM_GUARD  = 0xC8;
static constexpr uintptr_t OFF_PARAM_ISSEN  = 0xCC;
static constexpr uintptr_t OFF_PARAM_GRAB   = 0xD8;
static constexpr uintptr_t OFF_PARAM_DODGE  = 0xDC;
static constexpr uintptr_t OFF_ARR_COUNT    = 0x1C;
static constexpr uintptr_t OFF_ARR_DATA     = 0x20;

// ─────────────────────────────────────────────────────────────
// Command enum
// ─────────────────────────────────────────────────────────────
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

static constexpr int      PHASE_ACTUAL    = 2;
static constexpr uint64_t GRAPPLE_BIT     = 0x80000ull;
static constexpr uint64_t DAMAGE_BIT      = 0x100000ull;
static constexpr uint64_t SMASH_BIT       = 0x200000ull;
static constexpr uint64_t BLOCKED_BIT     = 0x400000ull;
static constexpr uint64_t ATTACK_BIT      = 0x40000ull;
static constexpr uint64_t GUARD_BREAK_BIT = 0x4000000ull;
static constexpr uint64_t BLOCK_BIT       = 0x8000000ull;
static constexpr uint64_t PARRY_BIT       = 0x10000000ull;
static constexpr uint64_t EVADE_BIT       = 0x20000000ull;

static constexpr uint64_t HELD_MASK = GRAPPLE_BIT | DAMAGE_BIT | SMASH_BIT |
    BLOCKED_BIT | GUARD_BREAK_BIT | BLOCK_BIT | PARRY_BIT;

// ─────────────────────────────────────────────────────────────
// Categories
// ─────────────────────────────────────────────────────────────
enum Cat { CAT_ATTACK, CAT_DODGE, CAT_GUARD, CAT_MOVE, CAT_ISSEN, CAT_OTHER, CAT_COUNT };
static constexpr int CAT_NONE = -1;

struct KeyDef { uint32_t cmd; uintptr_t offset; Cat cat; };
static const KeyDef kKeys[] = {
    { CMD_ONI_CHANGE, 0x50, CAT_OTHER },  { CMD_ONI_EYE, 0x58, CAT_OTHER },
    { CMD_PARRY, 0x60, CAT_GUARD },       { CMD_BLOCK, 0x68, CAT_GUARD },
    { CMD_JUST_DODGE, 0x70, CAT_DODGE },  { CMD_GRAB_COUNTER, 0x78, CAT_ISSEN },
    { CMD_ISSEN_COUNTER, 0x80, CAT_ISSEN }, { CMD_ISSEN_BREAK, 0x88, CAT_ISSEN },
    { CMD_ISSEN_BLOCK, 0x90, CAT_ISSEN }, { CMD_ISSEN_CHAIN, 0x98, CAT_ISSEN },
    { CMD_GRAPPLE_BLOW, 0xA0, CAT_OTHER },{ CMD_SKILL, 0xA8, CAT_ATTACK },
    { CMD_SKILL_KEEP, 0xB0, CAT_ATTACK }, { CMD_ULTIMATE, 0xB8, CAT_ATTACK },
    { CMD_ABSORB, 0xC0, CAT_OTHER },      { CMD_BOW, 0xC8, CAT_ATTACK },
    { CMD_GUARD, 0xD0, CAT_GUARD },       { CMD_ATTACK_BLOW, 0xD8, CAT_ATTACK },
    { CMD_DODGE, 0xE0, CAT_DODGE },       { CMD_ATTACK_JUST_DODGE, 0xE8, CAT_DODGE },
    { CMD_ATTACK_SNEAK, 0xF0, CAT_ATTACK },{ CMD_ATTACK_LIGHT_CHARGE, 0xF8, CAT_ATTACK },
    { CMD_ATTACK_LIGHT_COMBO, 0x100, CAT_ATTACK },{ CMD_ATTACK_LIGHT, 0x108, CAT_ATTACK },
    { CMD_ATTACK_HARD_CHARGE, 0x110, CAT_ATTACK },{ CMD_ATTACK_HARD_COMBO, 0x118, CAT_ATTACK },
    { CMD_ATTACK_HARD, 0x120, CAT_ATTACK },{ CMD_MEDICINE, 0x128, CAT_OTHER },
    { CMD_ITEM, 0x130, CAT_OTHER },       { CMD_INTERACT, 0x138, CAT_OTHER },
    { CMD_INTERACT_KEEP, 0x140, CAT_OTHER },{ CMD_DASH, 0x148, CAT_MOVE },
    { CMD_DASH_KEEP, 0x150, CAT_MOVE },   { CMD_RUN, 0x158, CAT_MOVE },
    { CMD_RUN_TURN, 0x160, CAT_MOVE },    { CMD_WALK, 0x168, CAT_MOVE },
    { CMD_WALK_KEEP, 0x170, CAT_MOVE },   { CMD_WALK_STICK, 0x178, CAT_MOVE },
    { CMD_QUICK_STOP, 0x180, CAT_MOVE },  { CMD_MENU_ITEM, 0x188, CAT_OTHER },
    { CMD_MENU_SKILL, 0x190, CAT_OTHER }, { CMD_LOCK_ON, 0x198, CAT_MOVE },
};
static constexpr size_t kKeysCount = sizeof(kKeys) / sizeof(kKeys[0]);

static inline Cat CatOfCmd(uint32_t cmd) {
    for (size_t i = 0; i < kKeysCount; i++) if (kKeys[i].cmd == cmd) return kKeys[i].cat;
    return CAT_OTHER;
}

// ─────────────────────────────────────────────────────────────
// Config
// ─────────────────────────────────────────────────────────────
struct Config {
    bool enabled = true;
    bool cat[CAT_COUNT] = { true, true, true, false, false, false };
    int  lock_guard = 0, lock_issen = -1, lock_grab = -1, lock_dodge = -1;
    float wm_guard = 1.0f, wm_issen = 1.0f, wm_grab = 1.0f, wm_dodge = 1.0f;
    bool feat_phase = true, feat_mask = true, feat_lockout = true,
         feat_window = true, feat_reflex = true, feat_wrappers = false;
    bool hook_late = true, hook_ctrl = true, hook_sel = true, hook_gjf = true;
    bool safe_mode = true;     // DEFAULT ON: no game function calls
    bool log_on = true;
    bool hotkey_f1 = true;
} g_cfg;

// ─────────────────────────────────────────────────────────────
// Runtime state
// ─────────────────────────────────────────────────────────────
struct LayerKeys { void* key_obj; Cat cat; };

struct RuntimeState {
    void*    entity = nullptr;
    void*    player = nullptr;
    void*    seq = nullptr;
    uint32_t seq_count = 0;
    std::unordered_map<uintptr_t, std::vector<LayerKeys>> layer_cache;
    void*    jfu = nullptr;
    void*    jparam = nullptr;
    int      orig_guard = -1, orig_issen = -1, orig_grab = -1, orig_dodge = -1;
    int64_t  state = 0;
    bool     jd_open = false;
    bool     held = false;
    int      skip_cat = CAT_NONE;
    uint64_t frame = 0;
    uint64_t state_frame = (uint64_t)-1;
    uint64_t hotkey_prev = 0;
    int      gjf_calls = 0;
    uint64_t last_entity_resolve = 0;

    // Crash guards
    bool player_invalid = false;
    bool gjc_disabled = false;
    bool o_late_crashed = false, o_ctrl_crashed = false,
         o_sel_crashed = false, o_gjf_crashed = false;
    int  hook_entries[4] = {0,0,0,0};   // late, ctrl, sel, gjf
} g_rt;

// ─────────────────────────────────────────────────────────────
// Logging
// ─────────────────────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────
// Safe memory access
// ─────────────────────────────────────────────────────────────
template<typename T>
static bool SafeRead(void* base, uintptr_t off, T& out) {
    __try { out = *(T*)((uintptr_t)base + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<typename T>
static bool SafeWrite(void* base, uintptr_t off, T v) {
    __try { *(T*)((uintptr_t)base + off) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static inline void* RdPtr(void* base, uintptr_t off) {
    void* v = nullptr; return SafeRead(base, off, v) ? v : nullptr;
}
static inline int32_t RdI32(void* base, uintptr_t off, int32_t d = 0) {
    int32_t v = d; return SafeRead(base, off, v) ? v : d;
}

// ─────────────────────────────────────────────────────────────
// Config loader
// ─────────────────────────────────────────────────────────────
static void LoadConfig() {
    char path[MAX_PATH];
    HMODULE self = GetModuleHandleA("OnimushaMod.dll");
    if (!self) self = GetModuleHandleA(nullptr);
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string ini = path;
    auto dot = ini.find_last_of('.');
    ini = (dot == std::string::npos) ? ini + ".ini" : ini.substr(0, dot) + ".ini";

    auto I = [&](const char* s, const char* k, int d) {
        return (int)GetPrivateProfileIntA(s, k, d, ini.c_str());
    };
    auto F = [&](const char* s, const char* k, float d) {
        char buf[64]; GetPrivateProfileStringA(s, k, "", buf, sizeof(buf), ini.c_str());
        return buf[0] ? (float)atof(buf) : d;
    };
    g_cfg.enabled         = I("AnimationCancel", "Enabled", 1) != 0;
    g_cfg.cat[CAT_ATTACK] = I("AnimationCancel", "Attacks", 1) != 0;
    g_cfg.cat[CAT_DODGE]  = I("AnimationCancel", "Dodge",   1) != 0;
    g_cfg.cat[CAT_GUARD]  = I("AnimationCancel", "Guard",   1) != 0;
    g_cfg.cat[CAT_MOVE]   = I("AnimationCancel", "Move",    0) != 0;
    g_cfg.cat[CAT_ISSEN]  = I("AnimationCancel", "Issen",   0) != 0;
    g_cfg.cat[CAT_OTHER]  = I("AnimationCancel", "Other",   0) != 0;
    g_cfg.lock_guard      = I("InstantParry", "GuardLockout", 0);
    g_cfg.lock_issen      = I("InstantParry", "IssenLockout", -1);
    g_cfg.lock_grab       = I("InstantParry", "GrabLockout", -1);
    g_cfg.lock_dodge      = I("InstantParry", "DodgeLockout", -1);
    g_cfg.wm_guard        = F("InstantParry", "GuardWindow", 1.0f);
    g_cfg.wm_issen        = F("InstantParry", "IssenWindow", 1.0f);
    g_cfg.wm_grab         = F("InstantParry", "GrabWindow", 1.0f);
    g_cfg.wm_dodge        = F("InstantParry", "DodgeWindow", 1.0f);
    g_cfg.feat_phase      = I("Features", "PhaseWrites", 1) != 0;
    g_cfg.feat_mask       = I("Features", "MaskClearing", 1) != 0;
    g_cfg.feat_lockout    = I("Features", "LockoutOverride", 1) != 0;
    g_cfg.feat_window     = I("Features", "WindowMultiplier", 1) != 0;
    g_cfg.feat_reflex     = I("Features", "ReflexHold", 1) != 0;
    g_cfg.feat_wrappers   = I("Features", "WrapperOverrides", 0) != 0;
    g_cfg.hook_late       = I("Hooks", "LateUpdate", 1) != 0;
    g_cfg.hook_ctrl       = I("Hooks", "CtrlUpdate", 1) != 0;
    g_cfg.hook_sel        = I("Hooks", "SelUpdate", 1) != 0;
    g_cfg.hook_gjf        = I("Hooks", "GetJustFrame", 1) != 0;
    g_cfg.safe_mode       = I("Debug", "SafeMode", 1) != 0;   // DEFAULT 1
    g_cfg.log_on          = I("Debug", "Log", 1) != 0;
    g_cfg.hotkey_f1       = I("Debug", "HotkeyF1", 1) != 0;

    Log("[cfg] enabled=%d cats a=%d d=%d g=%d m=%d i=%d o=%d",
        g_cfg.enabled, g_cfg.cat[0], g_cfg.cat[1], g_cfg.cat[2],
        g_cfg.cat[3], g_cfg.cat[4], g_cfg.cat[5]);
    Log("[cfg] features phase=%d mask=%d lockout=%d window=%d reflex=%d wrap=%d",
        g_cfg.feat_phase, g_cfg.feat_mask, g_cfg.feat_lockout,
        g_cfg.feat_window, g_cfg.feat_reflex, g_cfg.feat_wrappers);
    Log("[cfg] hooks late=%d ctrl=%d sel=%d gjf=%d SAFE=%d",
        g_cfg.hook_late, g_cfg.hook_ctrl, g_cfg.hook_sel, g_cfg.hook_gjf,
        g_cfg.safe_mode);
}

// ─────────────────────────────────────────────────────────────
// Game calls (defensive)
// ─────────────────────────────────────────────────────────────
static int64_t CallGetActionState(void* player) {
    if (!player || g_rt.player_invalid) return 0;
    typedef int64_t(*Fn)(void*);
    auto fn = (Fn)(kBase + RVA_CB_GETACTIONSTATE);
    __try { return fn(player); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_rt.player_invalid = true;
        Log("[crash] getActionState(player=%p) crashed -> player disabled", player);
        return 0;
    }
}
static bool CallIsJDAccepting(void* jds) {
    if (!jds || g_rt.gjc_disabled) return false;
    typedef bool(*Fn)(void*);
    auto fn = (Fn)(kBase + RVA_JDS_ISACCEPTING);
    __try { return fn(jds); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_rt.gjc_disabled = true;
        Log("[crash] isJustDodgeAttackAccepting(%p) crashed -> disabled", jds);
        return false;
    }
}

// ─────────────────────────────────────────────────────────────
// State
// ─────────────────────────────────────────────────────────────
static void RefreshSeq() {
    if (!g_rt.entity) return;
    void* seq = RdPtr(g_rt.entity, OFF_PCE_SEQ);
    // Validate: pointer must be in plausible user-space and 8-aligned
    uintptr_t sa = (uintptr_t)seq;
    bool plausible = seq && sa >= 0x10000 && sa <= 0x7FFFFFFFFFFF && (sa & 7) == 0;
    if (!plausible) seq = nullptr;
    if (seq == g_rt.seq) return;
    g_rt.seq = seq;
    g_rt.seq_count = 0;
    g_rt.layer_cache.clear();
    if (seq) {
        int32_t cnt = RdI32(seq, OFF_ARR_COUNT);
        if (cnt > 0 && cnt < 128) g_rt.seq_count = (uint32_t)cnt;
    }
    Log("[state] seq=%p layers=%u", g_rt.seq, g_rt.seq_count);
}

static void UpdateState() {
    if (g_rt.state_frame == g_rt.frame) return;
    g_rt.state_frame = g_rt.frame;
    if (!g_cfg.enabled) return;

    if (g_cfg.feat_phase) RefreshSeq();
    if (g_rt.frame % 600 == 0) { g_rt.seq = nullptr; g_rt.layer_cache.clear(); }

    if (g_cfg.safe_mode || g_rt.player_invalid) {
        g_rt.state = 0;
        g_rt.jd_open = false;
    } else {
        g_rt.state = CallGetActionState(g_rt.player);
        g_rt.jd_open = g_cfg.feat_reflex && (g_rt.state & EVADE_BIT) != 0
            && CallIsJDAccepting(RdPtr(g_rt.entity, OFF_PCE_JDS));
    }

    bool held_new = (g_rt.state & HELD_MASK) != 0 || g_rt.jd_open;
    if (held_new != g_rt.held) {
        Log("[state] held=%d state=0x%llX jd=%d",
            held_new, (unsigned long long)g_rt.state, g_rt.jd_open);
        g_rt.held = held_new;
    }

    if      (g_rt.state & ATTACK_BIT) g_rt.skip_cat = CAT_ATTACK;
    else if (g_rt.state & EVADE_BIT)  g_rt.skip_cat = CAT_DODGE;
    else if (g_rt.state & (BLOCK_BIT | PARRY_BIT)) g_rt.skip_cat = CAT_GUARD;
    else                               g_rt.skip_cat = CAT_NONE;
}

// ─────────────────────────────────────────────────────────────
// Phase writes
// ─────────────────────────────────────────────────────────────
static void ForcePhases() {
    if (!g_cfg.feat_phase || !g_rt.seq || !g_cfg.enabled || g_rt.held) return;
    for (uint32_t i = 0; i < g_rt.seq_count; i++) {
        void* layer = nullptr;
        if (!SafeRead((void*)((uintptr_t)g_rt.seq + OFF_ARR_DATA + 8 * i), 0, layer)) break;
        if (!layer) continue;
        auto it = g_rt.layer_cache.find((uintptr_t)layer);
        if (it == g_rt.layer_cache.end()) {
            std::vector<LayerKeys> keys;
            keys.reserve(kKeysCount);
            for (const auto& kd : kKeys) {
                void* ko = RdPtr(layer, kd.offset);
                if (ko) keys.push_back({ ko, kd.cat });
            }
            it = g_rt.layer_cache.emplace((uintptr_t)layer, std::move(keys)).first;
            Log("[phases] cached layer %p with %zu keys", layer, it->second.size());
        }
        for (auto& k : it->second) {
            if (!g_cfg.cat[k.cat]) continue;
            if (g_rt.skip_cat == (int)k.cat) continue;
            SafeWrite<int32_t>(k.key_obj, OFF_CCCMD_PHASE, PHASE_ACTUAL);
        }
    }
}

// ─────────────────────────────────────────────────────────────
// Mask clearing
// ─────────────────────────────────────────────────────────────
static void Unmask(void* mask_obj) {
    if (!g_cfg.feat_mask || !mask_obj || !g_cfg.enabled || g_rt.held) return;
    bool in_combat = (g_rt.state & (ATTACK_BIT | EVADE_BIT | BLOCK_BIT | PARRY_BIT)) != 0;
    if (!in_combat && !g_cfg.safe_mode) return;
    if (g_cfg.safe_mode) return;   // require state confirmation to unmask
    void* bitset = RdPtr(mask_obj, OFF_PCM_MASKS);
    if (!bitset) return;
    void* arr = RdPtr(bitset, OFF_BITSET_VALUE);
    if (!arr) return;
    int32_t nwords = RdI32(arr, OFF_ARR_COUNT);
    if (nwords <= 0 || nwords > 8) return;
    uint32_t* words = (uint32_t*)((uintptr_t)arr + OFF_ARR_DATA);
    for (const auto& kd : kKeys) {
        if (!g_cfg.cat[kd.cat]) continue;
        if (g_rt.skip_cat == (int)kd.cat) continue;
        uint32_t w = kd.cmd >> 5, bit = 1u << (kd.cmd & 31);
        if (w < (uint32_t)nwords) {
            __try { words[w] &= ~bit; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
}

// ─────────────────────────────────────────────────────────────
// Lockout override
// ─────────────────────────────────────────────────────────────
struct LockF { uintptr_t off; int* target; };
static LockF kLockF[] = {
    { OFF_PARAM_GUARD, &g_cfg.lock_guard }, { OFF_PARAM_ISSEN, &g_cfg.lock_issen },
    { OFF_PARAM_GRAB,  &g_cfg.lock_grab  }, { OFF_PARAM_DODGE, &g_cfg.lock_dodge },
};

static void AdoptJFU(void* jfu) {
    if (!jfu) return;
    if (jfu == g_rt.jfu && g_rt.jparam) return;
    g_rt.jfu = jfu;
    void* param = RdPtr(jfu, OFF_JFU_PARAM);
    if (!param || param == g_rt.jparam) return;
    g_rt.jparam = param;
    g_rt.orig_guard = RdI32(param, OFF_PARAM_GUARD);
    g_rt.orig_issen = RdI32(param, OFF_PARAM_ISSEN);
    g_rt.orig_grab  = RdI32(param, OFF_PARAM_GRAB);
    g_rt.orig_dodge = RdI32(param, OFF_PARAM_DODGE);
    Log("[parry] adopted param %p: g=%d i=%d gr=%d d=%d", param,
        g_rt.orig_guard, g_rt.orig_issen, g_rt.orig_grab, g_rt.orig_dodge);
}

static void ApplyLockout() {
    if (!g_cfg.feat_lockout || !g_rt.jparam || !g_cfg.enabled) return;
    for (auto& lf : kLockF)
        if (*lf.target >= 0) SafeWrite<int32_t>(g_rt.jparam, lf.off, *lf.target);
}

// ─────────────────────────────────────────────────────────────
// Original function pointers
// ─────────────────────────────────────────────────────────────
typedef void  (*LateUpdateFn)(void*);
typedef void  (*CtrlUpdateFn)(void*, void*);
typedef void  (*SelUpdateFn)(void*);
typedef void  (*JFUUpdateFn)(void*);
typedef float (*GetJustFrameFn)(void*, uint32_t, uint32_t, void*, void*);
typedef bool  (*CheckMotion2Fn)(void*, uint32_t);

static LateUpdateFn   o_LateUpdate   = nullptr;
static CtrlUpdateFn   o_CtrlUpdate   = nullptr;
static SelUpdateFn    o_SelUpdate    = nullptr;
static JFUUpdateFn    o_JFUUpdate    = nullptr;
static GetJustFrameFn o_GetJustFrame = nullptr;
static CheckMotion2Fn o_PCCancel     = nullptr;
static CheckMotion2Fn o_PCPreCancel  = nullptr;

static bool ShouldOverride(uint32_t cmd) {
    if (!g_cfg.feat_wrappers || !g_cfg.enabled || g_rt.held) return false;
    return g_cfg.cat[CatOfCmd(cmd)];
}

// ─────────────────────────────────────────────────────────────
// Hook implementations (defensive)
// ─────────────────────────────────────────────────────────────
static void hk_LateUpdate(void* self) {
    g_rt.frame++;

    if (g_rt.hook_entries[0]++ < 3) Log("[hook] late #%d self=%p", g_rt.hook_entries[0], self);

    if (g_cfg.hotkey_f1) {
        bool down = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
        if (down && !g_rt.hotkey_prev) {
            g_cfg.enabled = !g_cfg.enabled;
            Log("[hotkey] F1 -> enabled=%d", g_cfg.enabled);
        }
        g_rt.hotkey_prev = down;
    }

    if (self) {
        void* pce = RdPtr(self, OFF_PACC_PCE);
        if (pce != g_rt.entity) {
            g_rt.entity = pce;
            g_rt.player = RdPtr(self, OFF_PACC_PC);
            g_rt.seq = nullptr;
            g_rt.layer_cache.clear();
            g_rt.player_invalid = false;   // re-validate on entity change
            if (pce) Log("[state] entity=%p player=%p", pce, g_rt.player);
        }
        if (!g_rt.player && g_rt.frame - g_rt.last_entity_resolve > 60) {
            g_rt.last_entity_resolve = g_rt.frame;
            g_rt.player = RdPtr(self, OFF_PACC_PC);
            if (g_rt.player) Log("[state] player late: %p", g_rt.player);
        }
    }

    UpdateState();
    ForcePhases();

    if (o_LateUpdate && !g_rt.o_late_crashed) {
        __try { o_LateUpdate(self); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_rt.o_late_crashed = true;
            Log("[crash] o_LateUpdate crashed -> disabled");
        }
    }
}

static void hk_CtrlUpdate(void* self, void* mask) {
    if (g_rt.hook_entries[1]++ < 3)
        Log("[hook] ctrl #%d self=%p mask=%p", g_rt.hook_entries[1], self, mask);
    UpdateState();
    ForcePhases();
    Unmask(mask);
    if (o_CtrlUpdate && !g_rt.o_ctrl_crashed) {
        __try { o_CtrlUpdate(self, mask); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_rt.o_ctrl_crashed = true;
            Log("[crash] o_CtrlUpdate crashed -> disabled");
        }
    }
}

static void hk_SelUpdate(void* self) {
    if (g_rt.hook_entries[2]++ < 3) Log("[hook] sel #%d self=%p", g_rt.hook_entries[2], self);
    UpdateState();
    ForcePhases();
    if (o_SelUpdate && !g_rt.o_sel_crashed) {
        __try { o_SelUpdate(self); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_rt.o_sel_crashed = true;
            Log("[crash] o_SelUpdate crashed -> disabled");
        }
    }
}

static void hk_JFUUpdate(void* self) {
    AdoptJFU(self);
    ApplyLockout();
    if (o_JFUUpdate) {
        __try { o_JFUUpdate(self); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[crash] o_JFUUpdate crashed");
        }
    }
}

static float hk_GetJustFrame(void* self, uint32_t type, uint32_t cat,
                              void* obj, void* rank) {
    if (g_rt.hook_entries[3]++ < 5)
        Log("[hook] gjf #%d type=%u cat=%u", g_rt.hook_entries[3], type, cat);
    if (!o_GetJustFrame || g_rt.o_gjf_crashed) return 0.0f;
    float orig = 0.0f;
    __try { orig = o_GetJustFrame(self, type, cat, obj, rank); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_rt.o_gjf_crashed = true;
        Log("[crash] o_GetJustFrame crashed -> disabled");
        return 0.0f;
    }
    if (!g_cfg.feat_window || !g_cfg.enabled || orig <= 0.0f) return orig;
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

static bool hk_PCCancel(void* self, uint32_t cmd) {
    if (!o_PCCancel) return false;
    bool orig = o_PCCancel(self, cmd);
    return ShouldOverride(cmd) ? true : orig;
}
static bool hk_PCPreCancel(void* self, uint32_t cmd) {
    if (!o_PCPreCancel) return false;
    bool orig = o_PCPreCancel(self, cmd);
    return ShouldOverride(cmd) ? true : orig;
}

// ─────────────────────────────────────────────────────────────
// Install
// ─────────────────────────────────────────────────────────────
struct HookInst {
    uintptr_t rva; void* fn; void** orig; const char* name;
    bool* flag; bool optional; bool ok;
};
static HookInst g_hooks[] = {
    { RVA_PACC_LATEUPDATE,  (void*)hk_LateUpdate,  (void**)&o_LateUpdate,  "PACC.lateUpdate",  &g_cfg.hook_late, false, false },
    { RVA_PCC_UPDATE,       (void*)hk_CtrlUpdate,  (void**)&o_CtrlUpdate,  "PCC.update",       &g_cfg.hook_ctrl, false, false },
    { RVA_PAS_UPDATE,       (void*)hk_SelUpdate,   (void**)&o_SelUpdate,   "PAS.update",       &g_cfg.hook_sel,  false, false },
    { RVA_JFU_GETJUSTFRAME, (void*)hk_GetJustFrame,(void**)&o_GetJustFrame,"JFU.getJustFrame", &g_cfg.hook_gjf,  false, false },
    { RVA_JFU_UPDATE,       (void*)hk_JFUUpdate,   (void**)&o_JFUUpdate,   "JFU.update",       nullptr,          false, false },
    { RVA_PCE_CHECKCANCEL_T,(void*)hk_PCCancel,    (void**)&o_PCCancel,    "PCE.checkCancelMotion",    nullptr, true, false },
    { RVA_PCE_CHECKPRECANCEL_T,(void*)hk_PCPreCancel,(void**)&o_PCPreCancel,"PCE.checkPreCancelMotion", nullptr, true, false },
};

static int InstallAll() {
    if (MH_Initialize() != MH_OK) { Log("[-] MH_Initialize failed"); return 0; }
    int ok = 0;
    for (auto& h : g_hooks) {
        if (h.optional && !g_cfg.feat_wrappers) {
            Log("[i] skip %s (WrapperOverrides=0)", h.name); continue;
        }
        if (h.flag && !*h.flag) {
            Log("[i] skip %s (hook flag off)", h.name); continue;
        }
        void* target = (void*)(kBase + h.rva);
        MH_STATUS s1 = MH_CreateHook(target, h.fn, h.orig);
        MH_STATUS s2 = (s1 == MH_OK) ? MH_EnableHook(target) : s1;
        if (s2 != MH_OK) {
            Log("[-] %s hook failed (%d)", h.name, (int)s2);
        } else {
            Log("[+] %s hooked @ 0x%llX", h.name, (unsigned long long)h.rva);
            h.ok = true; ok++;
        }
    }
    return ok;
}

static void UninstallAll() {
    for (auto& h : g_hooks) if (h.ok) MH_DisableHook((void*)(kBase + h.rva));
    MH_Uninitialize();
    Log("[i] hooks removed");
}

// ─────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────
static DWORD WINAPI MainThread(LPVOID) {
    LoadConfig();
    for (int i = 0; i < 240; i++) {
        HMODULE mod = GetModuleHandleA("OnimushaWotS.exe");
        if (!mod) { Sleep(500); continue; }
        auto* dos = (IMAGE_DOS_HEADER*)mod;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) { Log("[-] not PE"); return 0; }
        auto* nt = (IMAGE_NT_HEADERS*)((uintptr_t)mod + dos->e_lfanew);
        if (nt->FileHeader.TimeDateStamp != kTimestamp ||
            nt->OptionalHeader.SizeOfImage != kSizeOfImage) {
            Log("[-] unsupported build (ts=%08X so=%08X)",
                nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
            return 0;
        }
        Log("[+] build verified: %p", mod);
        int n = InstallAll();
        if (n == 0) { Log("[-] no hooks installed"); return 0; }
        Log("[+] ready (%d hooks). SAFE_MODE=%d. Press F1 in game to toggle.",
            n, g_cfg.safe_mode ? 1 : 0);
        return 0;
    }
    Log("[-] OnimushaWotS.exe not found");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        char p[MAX_PATH]; GetModuleFileNameA(h, p, MAX_PATH);
        g_log_path = p;
        size_t s = g_log_path.find_last_of("\\/");
        g_log_path = (s == std::string::npos ? "" : g_log_path.substr(0, s + 1)) + "OnimushaMod_log.txt";
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        UninstallAll();
    }
    return TRUE;
}
