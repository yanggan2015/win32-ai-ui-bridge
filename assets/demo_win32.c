/* demo_win32.c — AI 可控 Win32 示例（P0 控件覆盖）
 * Edit / Button / CheckBox / ComboBox / ListBox / Tab / TrackBar / 虚拟控件
 * 运行后：http://127.0.0.1:8086/capabilities
 */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "win_ui_bridge.h"

#pragma comment(lib, "comctl32.lib")

#define ID_INPUT     1001
#define ID_BTN_ADD   1002
#define ID_BTN_CLEAR 1003
#define ID_LBL_SUM   1004
#define ID_CHK_MODE  1005
#define ID_COMBO     1006
#define ID_LIST      1007
#define ID_TAB       1008
#define ID_SLIDER    1009
#define ID_LBL_SLIDER 1010

static HWND g_hInput, g_hSum, g_hChk, g_hCombo, g_hList, g_hTab, g_hSlider, g_hLblSlider;
static int  g_sum = 0;
static int  g_adds = 0;
static int  g_bridge_port = 8086;

static void on_state(char *out, int cap, void *user)
{
    (void) user;
    int checked = (int) SendMessageW(g_hChk, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int sel = (int) SendMessageW(g_hCombo, CB_GETCURSEL, 0, 0);
    int slide = (int) SendMessageW(g_hSlider, TBM_GETPOS, 0, 0);
    snprintf(out, (size_t) cap,
             "{\"sum\":%d,\"adds\":%d,\"mode_on\":%s,\"combo_sel\":%d,\"slider\":%d,"
             "\"empty_input\":%s,\"bridge_port\":%d}",
             g_sum, g_adds, checked ? "true" : "false", sel, slide,
             (GetWindowTextLengthW(g_hInput) == 0) ? "true" : "false",
             g_bridge_port);
}

static void refresh_sum(void)
{
    WCHAR buf[128];
    wsprintfW(buf, L"合计: %d", g_sum);
    SetWindowTextW(g_hSum, buf);
}

static void refresh_slider_label(void)
{
    WCHAR buf[64];
    wsprintfW(buf, L"滑条: %d", (int) SendMessageW(g_hSlider, TBM_GETPOS, 0, 0));
    SetWindowTextW(g_hLblSlider, buf);
}

static void on_badge_click(void *user)
{
    (void) user;
    g_sum = 0;
    g_adds = 0;
    refresh_sum();
    ui_bridge_log("badge clicked -> reset");
}

static void do_add(void)
{
    WCHAR w[64];
    GetWindowTextW(g_hInput, w, (int) (sizeof(w) / sizeof(w[0])));
    int v = (int) wcstol(w, NULL, 10);
    g_sum += v;
    g_adds++;
    SetWindowTextW(g_hInput, L"");
    refresh_sum();
    ui_bridge_log("add value=%d sum=%d", v, g_sum);
}

static void do_clear(void)
{
    g_sum = 0;
    g_adds = 0;
    SetWindowTextW(g_hInput, L"");
    refresh_sum();
    ui_bridge_log("clear sum=%d", g_sum);
}

static void create_controls(HWND hwnd)
{
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    HFONT font = (HFONT) GetStockObject(DEFAULT_GUI_FONT);

    HWND c1 = CreateWindowW(L"STATIC", L"输入：", WS_CHILD | WS_VISIBLE,
                            20, 20, 50, 24, hwnd, (HMENU) 1, NULL, NULL);
    g_hInput = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                             70, 18, 100, 26, hwnd, (HMENU) ID_INPUT, NULL, NULL);
    HWND b1 = CreateWindowW(L"BUTTON", L"累加", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            180, 16, 70, 30, hwnd, (HMENU) ID_BTN_ADD, NULL, NULL);
    HWND b2 = CreateWindowW(L"BUTTON", L"清空", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            260, 16, 70, 30, hwnd, (HMENU) ID_BTN_CLEAR, NULL, NULL);
    g_hSum = CreateWindowW(L"STATIC", L"合计: 0", WS_CHILD | WS_VISIBLE,
                           350, 20, 120, 24, hwnd, (HMENU) ID_LBL_SUM, NULL, NULL);

    g_hChk = CreateWindowW(L"BUTTON", L"增强模式", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                           20, 60, 120, 24, hwnd, (HMENU) ID_CHK_MODE, NULL, NULL);

    HWND c2 = CreateWindowW(L"STATIC", L"下拉：", WS_CHILD | WS_VISIBLE,
                            160, 60, 50, 24, hwnd, (HMENU) 2, NULL, NULL);
    g_hCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                             210, 58, 140, 200, hwnd, (HMENU) ID_COMBO, NULL, NULL);
    SendMessageW(g_hCombo, CB_ADDSTRING, 0, (LPARAM) L"苹果");
    SendMessageW(g_hCombo, CB_ADDSTRING, 0, (LPARAM) L"香蕉");
    SendMessageW(g_hCombo, CB_ADDSTRING, 0, (LPARAM) L"橙子");
    SendMessageW(g_hCombo, CB_SETCURSEL, 0, 0);

    g_hList = CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY,
                            20, 100, 160, 100, hwnd, (HMENU) ID_LIST, NULL, NULL);
    SendMessageW(g_hList, LB_ADDSTRING, 0, (LPARAM) L"一行");
    SendMessageW(g_hList, LB_ADDSTRING, 0, (LPARAM) L"二行");
    SendMessageW(g_hList, LB_ADDSTRING, 0, (LPARAM) L"三行");

    g_hTab = CreateWindowW(WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                           200, 100, 220, 100, hwnd, (HMENU) ID_TAB, NULL, NULL);
    {
        TCITEMW ti;
        memset(&ti, 0, sizeof(ti));
        ti.mask = TCIF_TEXT;
        ti.pszText = L"页A";
        TabCtrl_InsertItem(g_hTab, 0, &ti);
        ti.pszText = L"页B";
        TabCtrl_InsertItem(g_hTab, 1, &ti);
    }

    g_hLblSlider = CreateWindowW(L"STATIC", L"滑条: 50", WS_CHILD | WS_VISIBLE,
                                 20, 220, 100, 24, hwnd, (HMENU) ID_LBL_SLIDER, NULL, NULL);
    g_hSlider = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                              120, 215, 300, 30, hwnd, (HMENU) ID_SLIDER, NULL, NULL);
    SendMessageW(g_hSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(g_hSlider, TBM_SETPOS, TRUE, 50);

    ui_bridge_set_id(c1, "label.input");
    ui_bridge_set_id(g_hInput, "edit.input");
    ui_bridge_set_id(b1, "btn.add");
    ui_bridge_set_id(b2, "btn.clear");
    ui_bridge_set_id(g_hSum, "label.sum");
    ui_bridge_set_id(g_hChk, "chk.mode");
    ui_bridge_set_id(c2, "label.combo");
    ui_bridge_set_id(g_hCombo, "combo.fruit");
    ui_bridge_set_id(g_hList, "list.rows");
    ui_bridge_set_id(g_hTab, "tab.main");
    ui_bridge_set_id(g_hSlider, "slider.level");
    ui_bridge_set_id(g_hLblSlider, "label.slider");

    HWND all[] = { c1, g_hInput, b1, b2, g_hSum, g_hChk, c2, g_hCombo,
                   g_hList, g_hTab, g_hSlider, g_hLblSlider };
    for (int i = 0; i < (int) (sizeof(all) / sizeof(all[0])); i++)
        SendMessageW(all[i], WM_SETFONT, (WPARAM) font, TRUE);
}

static int parse_port(LPSTR cmd)
{
    if (!cmd) return 8086;
    const char *p = strstr(cmd, "--ui-bridge-port=");
    if (p) return atoi(p + 16);
    p = strstr(cmd, "--ui-bridge-port ");
    if (p) return atoi(p + 16);
    return 8086;
}

static void apply_token_fix(LPSTR cmd)
{
    if (!cmd) return;
    const char *p = strstr(cmd, "--ui-token=");
    if (!p) return;
    p += 11;
    char tok[64];
    int i = 0;
    while (p[i] && p[i] != ' ' && i < 63) {
        tok[i] = p[i];
        i++;
    }
    tok[i] = '\0';
    if (tok[0]) ui_bridge_set_token(tok);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ui_bridge_handle_message(hwnd, msg, wp, lp)) return 0;

    switch (msg) {
    case WM_CREATE:
        create_controls(hwnd);
        ui_bridge_set_state_provider(on_state, NULL);
        apply_token_fix(GetCommandLineA());
        if (!ui_bridge_start(hwnd, g_bridge_port)) {
            MessageBoxW(hwnd, L"桥接层启动失败（端口被占用？）", L"提示", MB_OK);
        } else {
            WCHAR t[128];
            wsprintfW(t, L"AI 可控 Demo — http://127.0.0.1:%d", g_bridge_port);
            SetWindowTextW(hwnd, t);
        }
        {
            RECT rc = { 480, 16, 580, 46 };
            ui_bridge_add_virtual("custom.reset_badge", "custom", "重置徽标", rc,
                                  TRUE, "reset", on_badge_click, hwnd);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BTN_ADD:   do_add();   return 0;
        case ID_BTN_CLEAR: do_clear(); return 0;
        case ID_CHK_MODE:
            ui_bridge_log("chk.mode=%d",
                          (int) (SendMessageW(g_hChk, BM_GETCHECK, 0, 0) == BST_CHECKED));
            return 0;
        }
        return 0;

    case WM_HSCROLL:
        if ((HWND) lp == g_hSlider) {
            refresh_slider_label();
            ui_bridge_log("slider=%d", (int) SendMessageW(g_hSlider, TBM_GETPOS, 0, 0));
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Rectangle(dc, 480, 16, 580, 46);
        SetBkMode(dc, TRANSPARENT);
        TextOutW(dc, 498, 24, L"重置(自绘)", 6);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        ui_bridge_stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show)
{
    (void) hPrev;
    g_bridge_port = parse_port(cmd);

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
    wc.lpszClassName = L"DemoAiControllable";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR) IDC_ARROW);
    if (!RegisterClassW(&wc)) return 1;

    HWND hwnd = CreateWindowW(L"DemoAiControllable", L"AI 可控 Demo",
                              WS_OVERLAPPEDWINDOW, 100, 100, 640, 320,
                              NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
