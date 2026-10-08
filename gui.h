// ============================================================
//  gui.h  -  native Win32 window, no overlay/renderer hooking
//  ------------------------------------------------------------
//  A plain separate window (not drawn over the game) so we don't
//  need to touch the game's DirectX swapchain - far less risk than
//  a true in-game overlay. Runs its own message loop on its own
//  thread.
//
//  Two actions:
//   - "Apply to save file" - writes the 6 fixed-size move fields
//     (+ SuperID) for the selected player directly into the .sav
//     file via save_file_editor.h. Takes effect after a restart
//     (confirmed: this game has no in-session reload).
//   - "Apply to Custom Player (live)" - writes the same fields to
//     the live UGameSaveData object (save+0x1B4), visible
//     immediately, same mechanism the old custom-player editor used.
// ============================================================
#pragma once
#include <windows.h>
#include <windowsx.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <string>
#include "player_names.h"
#include "movedb.h"
#include "save_pointer.h"
#include "save_file_editor.h"

extern void WriteLog(const char* fmt, ...);

namespace Gui {

// ---- live Custom Player moveset (mirrors the old custom_player_editor.h) ----
struct Moveset {
    int32_t dribble, tackle, shot, airshot, saving;
    uint8_t superType; uint8_t _pad[3];
    int32_t superID, miracle;
};
static_assert(sizeof(Moveset) == 0x20, "must match live struct");

inline bool WriteLiveCustomPlayer(const SaveFileEditor::Edits& e) {
    auto chain = SavePointer::Resolve();
    if (!chain.ok) return false;
    uintptr_t addr = chain.save + 0x1B4;
    if (IsBadReadPtr(reinterpret_cast<void*>(addr), sizeof(Moveset))) return false;
    Moveset m{};
    memcpy(&m, reinterpret_cast<void*>(addr), sizeof(m));
    if (e.dribble >= 0) m.dribble = e.dribble;
    if (e.tackle >= 0) m.tackle = e.tackle;
    if (e.shot >= 0) m.shot = e.shot;
    if (e.airshot >= 0) m.airshot = e.airshot;
    if (e.saving >= 0) m.saving = e.saving;
    if (e.miracle >= 0) m.miracle = e.miracle;
    if (e.superId >= 0) m.superID = e.superId;
    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(addr), sizeof(m), PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    memcpy(reinterpret_cast<void*>(addr), &m, sizeof(m));
    VirtualProtect(reinterpret_cast<LPVOID>(addr), sizeof(m), oldProt, &oldProt);
    return true;
}

// ---- control IDs ----
enum {
    ID_SEARCH = 100, ID_PLAYERLIST,
    ID_CB_DRIBBLE, ID_CB_TACKLE, ID_CB_SHOT, ID_CB_AIRSHOT, ID_CB_SAVING, ID_CB_MIRACLE,
    ID_ED_SUPERID, ID_ED_PATH,
    ID_BTN_FILE, ID_BTN_LIVE,
    ID_STATUS
};

inline HWND g_hPlayerList, g_hSearch, g_hPath, g_hStatus;
inline HWND g_hCbDribble, g_hCbTackle, g_hCbShot, g_hCbAirshot, g_hCbSaving, g_hCbMiracle, g_hEdSuper;

inline void SetStatus(const char* fmt, ...) {
    char buf[512];
    va_list args; va_start(args, fmt); vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
    SetWindowTextA(g_hStatus, buf);
    WriteLog("[GUI] %s", buf);
}

inline void FillMoveCombo(HWND cb, const MoveEntry* entries, int count) {
    SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)"(non modificare)");
    SendMessageA(cb, CB_SETITEMDATA, 0, (LPARAM)-1);
    for (int i = 0; i < count; ++i) {
        int idx = (int)SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)entries[i].name);
        SendMessageA(cb, CB_SETITEMDATA, idx, (LPARAM)entries[i].id);
    }
    SendMessageA(cb, CB_SETCURSEL, 0, 0);
}

inline int ComboValue(HWND cb) {
    int sel = (int)SendMessageA(cb, CB_GETCURSEL, 0, 0);
    if (sel < 0) return -1;
    return (int)SendMessageA(cb, CB_GETITEMDATA, sel, 0);
}

inline void PopulatePlayerList(const char* filter) {
    SendMessageA(g_hPlayerList, LB_RESETCONTENT, 0, 0);
    std::string f = filter ? filter : "";
    for (auto& c : f) c = (char)tolower((unsigned char)c);
    for (int i = 0; i < g_playerNames_count; ++i) {
        std::string name = g_playerNames[i].name;
        std::string lower = name;
        for (auto& c : lower) c = (char)tolower((unsigned char)c);
        if (!f.empty() && lower.find(f) == std::string::npos) continue;
        char label[128];
        snprintf(label, sizeof(label), "%s (ID %d)", g_playerNames[i].name, g_playerNames[i].chara_id);
        int idx = (int)SendMessageA(g_hPlayerList, LB_ADDSTRING, 0, (LPARAM)label);
        SendMessageA(g_hPlayerList, LB_SETITEMDATA, idx, (LPARAM)g_playerNames[i].chara_id);
    }
}

inline int SelectedCharaId() {
    int sel = (int)SendMessageA(g_hPlayerList, LB_GETCURSEL, 0, 0);
    if (sel < 0) return -1;
    return (int)SendMessageA(g_hPlayerList, LB_GETITEMDATA, sel, 0);
}

inline SaveFileEditor::Edits CollectEdits() {
    SaveFileEditor::Edits e;
    e.dribble = ComboValue(g_hCbDribble);
    e.tackle = ComboValue(g_hCbTackle);
    e.shot = ComboValue(g_hCbShot);
    e.airshot = ComboValue(g_hCbAirshot);
    e.saving = ComboValue(g_hCbSaving);
    e.miracle = ComboValue(g_hCbMiracle);
    char buf[32];
    GetWindowTextA(g_hEdSuper, buf, sizeof(buf));
    e.superId = (buf[0] != '\0') ? atoi(buf) : -1;
    return e;
}

inline LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        WORD code = HIWORD(wParam);
        if (id == ID_SEARCH && code == EN_CHANGE) {
            char buf[128];
            GetWindowTextA(g_hSearch, buf, sizeof(buf));
            PopulatePlayerList(buf);
        } else if (id == ID_BTN_FILE) {
            int cid = SelectedCharaId();
            if (cid < 0) { SetStatus("Seleziona prima un giocatore."); break; }
            if (cid == 9901) { SetStatus("9901 e' il Custom Player - usa il pulsante 'live', non il file."); break; }
            char path[MAX_PATH];
            GetWindowTextA(g_hPath, path, sizeof(path));
            auto edits = CollectEdits();
            auto r = SaveFileEditor::ApplyToFile(path, cid, edits);
            if (r.ok) SetStatus("OK: %d campo/i scritti in %d blocco/i. Riavvia il gioco per vederlo.", r.fieldsPatched, r.blocksPatched);
            else SetStatus("Errore: %s", r.error.c_str());
        } else if (id == ID_BTN_LIVE) {
            auto edits = CollectEdits();
            bool ok = WriteLiveCustomPlayer(edits);
            SetStatus(ok ? "OK: applicato live al Custom Player." : "Errore: salvataggio live non raggiungibile (apri un menu in game e riprova).");
        }
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE); // keep it alive, just hide - DLL stays loaded
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

inline HWND Label(HWND parent, const char* text, int x, int y, int w, int h) {
    return CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent, nullptr, nullptr, nullptr);
}

inline DWORD WINAPI GuiThread(LPVOID) {
    WNDCLASSA wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "CT2AllMovesGuiClass";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorA(nullptr, MAKEINTRESOURCEA(32512)); // IDC_ARROW, forced ANSI (UNICODE define would otherwise widen it)
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(WS_EX_TOPMOST, wc.lpszClassName, "CT2 AllMoves",
        WS_OVERLAPPEDWINDOW, 100, 100, 520, 560, nullptr, nullptr, wc.hInstance, nullptr);

    Label(hwnd, "Cerca giocatore:", 12, 12, 140, 18);
    g_hSearch = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE,
        12, 32, 220, 22, hwnd, (HMENU)ID_SEARCH, wc.hInstance, nullptr);
    g_hPlayerList = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
        12, 60, 220, 420, hwnd, (HMENU)ID_PLAYERLIST, wc.hInstance, nullptr);

    int rx = 250, ry = 20, rw = 250;
    Label(hwnd, "Dribbling:", rx, ry, rw, 16); ry += 18;
    g_hCbDribble = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, rx, ry, rw, 300, hwnd, (HMENU)ID_CB_DRIBBLE, wc.hInstance, nullptr); ry += 28;
    Label(hwnd, "Tackle:", rx, ry, rw, 16); ry += 18;
    g_hCbTackle = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, rx, ry, rw, 300, hwnd, (HMENU)ID_CB_TACKLE, wc.hInstance, nullptr); ry += 28;
    Label(hwnd, "Tiro:", rx, ry, rw, 16); ry += 18;
    g_hCbShot = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, rx, ry, rw, 300, hwnd, (HMENU)ID_CB_SHOT, wc.hInstance, nullptr); ry += 28;
    Label(hwnd, "Tiro Aereo:", rx, ry, rw, 16); ry += 18;
    g_hCbAirshot = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, rx, ry, rw, 300, hwnd, (HMENU)ID_CB_AIRSHOT, wc.hInstance, nullptr); ry += 28;
    Label(hwnd, "Saving (portieri):", rx, ry, rw, 16); ry += 18;
    g_hCbSaving = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, rx, ry, rw, 300, hwnd, (HMENU)ID_CB_SAVING, wc.hInstance, nullptr); ry += 28;
    Label(hwnd, "Miracle Saving (portieri):", rx, ry, rw, 16); ry += 18;
    g_hCbMiracle = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, rx, ry, rw, 300, hwnd, (HMENU)ID_CB_MIRACLE, wc.hInstance, nullptr); ry += 28;
    Label(hwnd, "Super ID (numero, tipo non confermato):", rx, ry, rw, 16); ry += 18;
    g_hEdSuper = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE, rx, ry, 100, 22, hwnd, (HMENU)ID_ED_SUPERID, wc.hInstance, nullptr); ry += 34;

    FillMoveCombo(g_hCbDribble, g_dribbleMoves, g_dribbleMoves_count);
    FillMoveCombo(g_hCbTackle, g_tackleMoves, g_tackleMoves_count);
    FillMoveCombo(g_hCbShot, g_shotMoves, g_shotMoves_count);
    FillMoveCombo(g_hCbAirshot, g_airshotMoves, g_airshotMoves_count);
    FillMoveCombo(g_hCbSaving, g_savingMoves, g_savingMoves_count);
    FillMoveCombo(g_hCbMiracle, g_miracleMoves, g_miracleMoves_count);

    Label(hwnd, "Percorso file salvataggio:", 12, ry, 480, 16); ry += 18;
    char defPath[MAX_PATH] = {};
    char localAppData[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
        snprintf(defPath, sizeof(defPath), "%s\\CaptainTsubasa2WF\\Saved\\SaveGames\\6144", localAppData);
    g_hPath = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", defPath, WS_CHILD | WS_VISIBLE, 12, ry, 480, 22, hwnd, (HMENU)ID_ED_PATH, wc.hInstance, nullptr);
    ry += 34;

    CreateWindowExA(0, "BUTTON", "Applica al file (riavvio richiesto)", WS_CHILD | WS_VISIBLE,
        250, ry, 240, 30, hwnd, (HMENU)ID_BTN_FILE, wc.hInstance, nullptr);
    ry += 36;
    CreateWindowExA(0, "BUTTON", "Applica al Custom Player (live, subito)", WS_CHILD | WS_VISIBLE,
        250, ry, 240, 30, hwnd, (HMENU)ID_BTN_LIVE, wc.hInstance, nullptr);

    g_hStatus = CreateWindowExA(0, "STATIC", "Pronto.", WS_CHILD | WS_VISIBLE | SS_LEFT,
        12, 490, 490, 40, hwnd, (HMENU)ID_STATUS, wc.hInstance, nullptr);

    PopulatePlayerList(nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

inline void Launch() {
    CreateThread(nullptr, 0, GuiThread, nullptr, 0, nullptr);
}

} // namespace Gui
