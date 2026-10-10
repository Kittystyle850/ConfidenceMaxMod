// CT2 Unique Technique Limit Mod (standalone)
// Single-purpose DLL: raises the in-game Unique Technique selection limit
// from 2 to 4 (all four normal slots). No Chain Max, no hotkeys, no sound,
// default-active on load. Ported from CT2CheatDLL "ChainForge Max" v2.0,
// keeping only CheatEntry 521 and switching its fixed RVAs to AOB scans
// so it survives game updates.
//
// Inject with Cheat Engine (or any DLL injector) into
// CaptainTsubasa2WF-Win64-Shipping.exe after the game has fully loaded.

#include <windows.h>
#include <string>
#include <cstdio>
#include "unique_limit_patch.h"

static void WriteLog(const std::string& text) {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string dir(path);
    size_t pos = dir.find_last_of("\\/");
    if (pos != std::string::npos) dir = dir.substr(0, pos + 1);
    std::string logPath = dir + "CT2_UniqueLimit4_log.txt";

    FILE* f = fopen(logPath.c_str(), "a");
    if (!f) return;

    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] %s",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            text.c_str());
    fclose(f);
}

static DWORD WINAPI MainThread(LPVOID) {
    WriteLog("[i] CT2 Unique Technique Limit Mod started.\n");

    HMODULE mod = nullptr;
    const int maxTries = 40;
    for (int i = 0; i < maxTries && !mod; ++i) {
        mod = GetModuleHandleA("CaptainTsubasa2WF-Win64-Shipping.exe");
        if (!mod) Sleep(500);
    }

    if (!mod) {
        WriteLog("[-] Could not find main module after retries. Aborting.\n");
        return 0;
    }

    char buf[128];
    wsprintfA(buf, "[i] module base=0x%p\n", (void*)mod);
    WriteLog(buf);

    UniqueLimitPatchResult result;
    for (int i = 0; i < maxTries; ++i) {
        result = ApplyUniqueTechniqueLimitPatch(mod);
        if (result.countPatched && result.maxPatched &&
            result.selectGuardPatched && result.confirmGuardPatched) {
            break;
        }
        Sleep(500);
    }

    WriteLog(result.log);

    if (result.countPatched && result.maxPatched &&
        result.selectGuardPatched && result.confirmGuardPatched) {
        WriteLog("[+] Unique Technique Limit set to 4. All four sites patched. No hotkeys, no sound.\n");
    } else {
        WriteLog("[-] One or more patch sites failed. Limit may be partially or not applied - see log above.\n");
    }

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
