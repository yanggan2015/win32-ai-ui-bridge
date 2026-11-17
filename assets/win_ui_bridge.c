/* win_ui_bridge.c — Win32 AI 可读可操作桥接层（协议 1.1，P0）
 * 依赖：mongoose 7.x、cJSON 1.7.15+
 */
#include "win_ui_bridge.h"

#if !defined(UI_BRIDGE_ENABLE)
#error "win_ui_bridge.c 只能在定义了 UI_BRIDGE_ENABLE 时编译"
#endif

#include "cJSON.h"
#include "mongoose.h"

#include <commctrl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UB_ID_LEN       96
#define UB_LOG_MAX      128
#define UB_LOG_LEN      512
#define UB_VIRTUAL_MAX  64
#define UB_ATTACH_MAX   8
#define UB_ITEMS_TREE   50
#define UB_ITEMS_FULL   500
#define UB_WAIT_MS      8000
#define UB_RESULT_LEN   1024

#pragma comment(lib, "comctl32.lib")

#ifndef BS_TYPEMASK
#define BS_TYPEMASK 0x0000000FL
#endif

typedef struct {
    unsigned seq;
    unsigned long ms;
    char text[UB_LOG_LEN];
} UB_LOG_ENTRY;

typedef struct {
    char id[UB_ID_LEN];
    char role[32];
    char name[256];
    char text[512];
    RECT rect;
    BOOL enabled;
    UI_VIRTUAL_CLICK on_click;
    UI_VIRTUAL_ACTION on_action;
    void *user;
} UB_VIRTUAL;

typedef struct {
    char id[UB_ID_LEN];
    char action[32];
    char text[512];
    int  value;
} UB_REQUEST;

static HWND              g_main;
static HWND              g_attach[UB_ATTACH_MAX];
static int               g_attach_count;
static volatile LONG     g_running;
static volatile LONG     g_inited;
static volatile LONG     g_action_count;
static CRITICAL_SECTION  g_lock;
static HANDLE            g_done_event;
static int               g_port;

static UB_REQUEST        g_req;
static volatile LONG     g_req_pending;
static int               g_res_code;
static char              g_res_msg[512];
static char              g_res_result[UB_RESULT_LEN];
static char              g_last_action[64];

static UB_VIRTUAL        g_virtual[UB_VIRTUAL_MAX];
static int               g_virtual_count;
static UB_LOG_ENTRY      g_logs[UB_LOG_MAX];
static unsigned          g_log_seq;

static UI_STATE_CB       g_state_cb;
static void             *g_state_user;
static char              g_token[64];

/* ------------------------------------------------------------------ 工具 */

static void ub_copy(char *dst, int cap, const char *src)
{
    if (cap <= 0) return;
    if (!src) { dst[0] = '\0'; return; }
    int n = (int) strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, (size_t) n);
    dst[n] = '\0';
}

static void ub_wide_to_utf8(const WCHAR *w, char *out, int cap)
{
    out[0] = '\0';
    if (!w || !w[0] || cap <= 1) return;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
}

static void ub_utf8_to_wide(const char *s, WCHAR *out, int cap_w)
{
    out[0] = L'\0';
    if (!s) return;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap_w);
}

static void ub_class_name(HWND h, char *out, int cap)
{
    WCHAR w[128];
    out[0] = '\0';
    if (!h) return;
    GetClassNameW(h, w, (int) (sizeof(w) / sizeof(w[0])));
    ub_wide_to_utf8(w, out, cap);
}

static void ub_window_text(HWND h, char *out, int cap)
{
    WCHAR w[512];
    out[0] = '\0';
    if (!h) return;
    GetWindowTextW(h, w, (int) (sizeof(w) / sizeof(w[0])));
    ub_wide_to_utf8(w, out, cap);
}

static int ub_is(const char *cls, const char *sub)
{
    return cls && sub && strstr(cls, sub) != NULL;
}

/* Button 细分：checkbox / radio / button */
static const char *ub_role_of(HWND h, const char *cls)
{
    if (!cls || !cls[0]) return "unknown";
    if (ub_is(cls, "Button")) {
        LONG sty = GetWindowLongW(h, GWL_STYLE);
        int t = (int) (sty & BS_TYPEMASK);
        if (t == BS_AUTOCHECKBOX || t == BS_CHECKBOX || t == BS_3STATE || t == BS_AUTO3STATE)
            return "checkbox";
        if (t == BS_AUTORADIOBUTTON || t == BS_RADIOBUTTON)
            return "radio";
        return "button";
    }
    if (ub_is(cls, "Edit") || ub_is(cls, "RICHEDIT")) return "textbox";
    if (ub_is(cls, "ComboBox")) return "combobox";
    if (ub_is(cls, "ListBox")) return "listbox";
    if (ub_is(cls, "SysListView")) return "list";
    if (ub_is(cls, "SysTreeView")) return "tree";
    if (ub_is(cls, "Static")) return "label";
    if (ub_is(cls, "msctls_trackbar") || ub_is(cls, "TrackBar")) return "slider";
    if (ub_is(cls, "msctls_updown") || ub_is(cls, "UpDown")) return "spinner";
    if (ub_is(cls, "msctls_progress") || ub_is(cls, "Progress")) return "progress";
    if (ub_is(cls, "msctls_statusbar") || ub_is(cls, "StatusBar")) return "statusbar";
    if (ub_is(cls, "SysTabControl")) return "tabview";
    return "unknown";
}

static void ub_effective_id(HWND h, char *out, int cap)
{
    const char *pid = (const char *) GetPropA(h, "ui.id");
    if (pid && pid[0]) {
        ub_copy(out, cap, pid);
        return;
    }
    char cls[128];
    ub_class_name(h, cls, sizeof(cls));
    snprintf(out, (size_t) cap, "auto/%s#%ld", cls[0] ? cls : "window",
             (long) GetDlgCtrlID(h));
}

static void ub_add_actions(cJSON *o, const char *role, BOOL enabled, BOOL readonly)
{
    cJSON *arr = cJSON_CreateArray();
    if (!enabled) {
        cJSON_AddItemToObject(o, "actions", arr);
        return;
    }
    if (strcmp(role, "button") == 0 || strcmp(role, "radio") == 0) {
        cJSON_AddItemToArray(arr, cJSON_CreateString("click"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("focus"));
    } else if (strcmp(role, "checkbox") == 0) {
        cJSON_AddItemToArray(arr, cJSON_CreateString("click"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("check"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("uncheck"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("toggle"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("focus"));
    } else if (strcmp(role, "textbox") == 0) {
        cJSON_AddItemToArray(arr, cJSON_CreateString("get_text"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("focus"));
        if (!readonly) {
            cJSON_AddItemToArray(arr, cJSON_CreateString("set_text"));
            cJSON_AddItemToArray(arr, cJSON_CreateString("clear"));
        }
    } else if (strcmp(role, "label") == 0) {
        cJSON_AddItemToArray(arr, cJSON_CreateString("get_text"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("set_text"));
    } else if (strcmp(role, "combobox") == 0) {
        cJSON_AddItemToArray(arr, cJSON_CreateString("select"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("set_text"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("get_text"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("click"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("focus"));
    } else if (strcmp(role, "listbox") == 0 || strcmp(role, "list") == 0 ||
               strcmp(role, "tabview") == 0) {
        cJSON_AddItemToArray(arr, cJSON_CreateString("select"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("focus"));
    } else if (strcmp(role, "slider") == 0 || strcmp(role, "spinner") == 0 ||
               strcmp(role, "progress") == 0) {
        cJSON_AddItemToArray(arr, cJSON_CreateString("set_value"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("focus"));
    } else if (strcmp(role, "custom") == 0) {
        cJSON_AddItemToArray(arr, cJSON_CreateString("click"));
    } else {
        cJSON_AddItemToArray(arr, cJSON_CreateString("click"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("focus"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("get_text"));
    }
    cJSON_AddItemToObject(o, "actions", arr);
}

/* ------------------------------------------------------------ enrichment */

static void ub_fill_button(HWND h, cJSON *o, const char *role)
{
    if (strcmp(role, "checkbox") == 0 || strcmp(role, "radio") == 0) {
        LRESULT st = SendMessageW(h, BM_GETCHECK, 0, 0);
        cJSON_AddBoolToObject(o, "checked", st == BST_CHECKED ? 1 : 0);
    }
}

static void ub_add_items_combo(HWND h, cJSON *o, int max_items)
{
    int n = (int) SendMessageW(h, CB_GETCOUNT, 0, 0);
    int sel = (int) SendMessageW(h, CB_GETCURSEL, 0, 0);
    cJSON_AddNumberToObject(o, "item_count", (double) n);
    cJSON_AddNumberToObject(o, "selected", (double) sel);
    cJSON *items = cJSON_CreateArray();
    int lim = n > max_items ? max_items : n;
    for (int i = 0; i < lim; i++) {
        WCHAR w[256];
        SendMessageW(h, CB_GETLBTEXT, (WPARAM) i, (LPARAM) w);
        char u[512];
        ub_wide_to_utf8(w, u, sizeof(u));
        cJSON_AddItemToArray(items, cJSON_CreateString(u));
    }
    cJSON_AddItemToObject(o, "items", items);
    if (n > max_items) cJSON_AddBoolToObject(o, "items_truncated", 1);
}

static void ub_add_items_listbox(HWND h, cJSON *o, int max_items)
{
    int n = (int) SendMessageW(h, LB_GETCOUNT, 0, 0);
    int sel = (int) SendMessageW(h, LB_GETCURSEL, 0, 0);
    cJSON_AddNumberToObject(o, "item_count", (double) n);
    cJSON_AddNumberToObject(o, "selected", (double) sel);
    cJSON *items = cJSON_CreateArray();
    int lim = n > max_items ? max_items : n;
    for (int i = 0; i < lim; i++) {
        WCHAR w[256];
        SendMessageW(h, LB_GETTEXT, (WPARAM) i, (LPARAM) w);
        char u[512];
        ub_wide_to_utf8(w, u, sizeof(u));
        cJSON_AddItemToArray(items, cJSON_CreateString(u));
    }
    cJSON_AddItemToObject(o, "items", items);
    if (n > max_items) cJSON_AddBoolToObject(o, "items_truncated", 1);
}

static void ub_add_items_listview(HWND h, cJSON *o, int max_items)
{
    int n = (int) SendMessageW(h, LVM_GETITEMCOUNT, 0, 0);
    int sel = (int) SendMessageW(h, LVM_GETNEXTITEM, (WPARAM) -1, LVNI_SELECTED);
    cJSON_AddNumberToObject(o, "item_count", (double) n);
    cJSON_AddNumberToObject(o, "selected", (double) sel);
    cJSON *items = cJSON_CreateArray();
    int lim = n > max_items ? max_items : n;
    for (int i = 0; i < lim; i++) {
        WCHAR w[256];
        LVITEMW it;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.iSubItem = 0;
        it.pszText = w;
        it.cchTextMax = (int) (sizeof(w) / sizeof(w[0]));
        SendMessageW(h, LVM_GETITEMW, 0, (LPARAM) &it);
        char u[512];
        ub_wide_to_utf8(w, u, sizeof(u));
        cJSON_AddItemToArray(items, cJSON_CreateString(u));
    }
    cJSON_AddItemToObject(o, "items", items);
    if (n > max_items) cJSON_AddBoolToObject(o, "items_truncated", 1);
}

static void ub_add_items_tab(HWND h, cJSON *o, int max_items)
{
    int n = (int) SendMessageW(h, TCM_GETITEMCOUNT, 0, 0);
    int sel = (int) SendMessageW(h, TCM_GETCURSEL, 0, 0);
    cJSON_AddNumberToObject(o, "item_count", (double) n);
    cJSON_AddNumberToObject(o, "selected", (double) sel);
    cJSON *items = cJSON_CreateArray();
    int lim = n > max_items ? max_items : n;
    for (int i = 0; i < lim; i++) {
        WCHAR w[128];
        TCITEMW ti;
        memset(&ti, 0, sizeof(ti));
        ti.mask = TCIF_TEXT;
        ti.pszText = w;
        ti.cchTextMax = (int) (sizeof(w) / sizeof(w[0]));
        SendMessageW(h, TCM_GETITEMW, (WPARAM) i, (LPARAM) &ti);
        char u[256];
        ub_wide_to_utf8(w, u, sizeof(u));
        cJSON_AddItemToArray(items, cJSON_CreateString(u));
    }
    cJSON_AddItemToObject(o, "items", items);
    if (n > max_items) cJSON_AddBoolToObject(o, "items_truncated", 1);
}

static void ub_fill_range(HWND h, cJSON *o, const char *role)
{
    if (strcmp(role, "slider") == 0) {
        int mn = (int) SendMessageW(h, TBM_GETRANGEMIN, 0, 0);
        int mx = (int) SendMessageW(h, TBM_GETRANGEMAX, 0, 0);
        int pos = (int) SendMessageW(h, TBM_GETPOS, 0, 0);
        cJSON_AddNumberToObject(o, "min", (double) mn);
        cJSON_AddNumberToObject(o, "max", (double) mx);
        cJSON_AddNumberToObject(o, "value", (double) pos);
    } else if (strcmp(role, "progress") == 0) {
        int range = (int) SendMessageW(h, PBM_GETRANGE, 0, 0);
        int pos = (int) SendMessageW(h, PBM_GETPOS, 0, 0);
        /* PBM_GETRANGE low 在 LOWORD 不可靠；用默认 0..100 兜底若为 0 */
        cJSON_AddNumberToObject(o, "min", 0);
        cJSON_AddNumberToObject(o, "max", range > 0 ? (double) range : 100);
        cJSON_AddNumberToObject(o, "value", (double) pos);
    } else if (strcmp(role, "spinner") == 0) {
        int pos = (int) SendMessageW(h, UDM_GETPOS32, 0, 0);
        cJSON_AddNumberToObject(o, "value", (double) pos);
        cJSON_AddNumberToObject(o, "min", 0);
        cJSON_AddNumberToObject(o, "max", 100);
    }
}

static cJSON *ub_node_json(HWND h, HWND root_for_parent, int order, int max_items)
{
    char id[UB_ID_LEN], cls[128], text[512];
    ub_effective_id(h, id, sizeof(id));
    ub_class_name(h, cls, sizeof(cls));
    ub_window_text(h, text, sizeof(text));
    const char *role = ub_role_of(h, cls);

    int readonly = 0;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", id);
    cJSON_AddStringToObject(o, "role", role);
    cJSON_AddStringToObject(o, "class", cls);

    if (strcmp(role, "textbox") == 0) {
        LONG sty = GetWindowLongW(h, GWL_STYLE);
        if (sty & ES_PASSWORD) ub_copy(text, sizeof(text), "***");
        cJSON_AddBoolToObject(o, "password", (sty & ES_PASSWORD) ? 1 : 0);
        readonly = (sty & ES_READONLY) ? 1 : 0;
        cJSON_AddBoolToObject(o, "readonly", readonly);
    }
    cJSON_AddStringToObject(o, "text", text);

    RECT r;
    GetWindowRect(h, &r);
    cJSON *rect = cJSON_CreateArray();
    cJSON_AddItemToArray(rect, cJSON_CreateNumber((double) r.left));
    cJSON_AddItemToArray(rect, cJSON_CreateNumber((double) r.top));
    cJSON_AddItemToArray(rect, cJSON_CreateNumber((double) r.right));
    cJSON_AddItemToArray(rect, cJSON_CreateNumber((double) r.bottom));
    cJSON_AddItemToObject(o, "rect", rect);

    BOOL en = IsWindowEnabled(h);
    cJSON_AddBoolToObject(o, "enabled", en ? 1 : 0);
    cJSON_AddBoolToObject(o, "visible", IsWindowVisible(h) ? 1 : 0);
    cJSON_AddBoolToObject(o, "focused", GetFocus() == h ? 1 : 0);
    cJSON_AddNumberToObject(o, "order", (double) order);

    {
        char hwnd_s[32];
        snprintf(hwnd_s, sizeof(hwnd_s), "%llu", (unsigned long long) (uintptr_t) h);
        cJSON_AddStringToObject(o, "hwnd", hwnd_s);
    }

    HWND parent = GetParent(h);
    if (!parent || parent == root_for_parent) {
        cJSON_AddStringToObject(o, "parent", "root");
    } else {
        char pid[UB_ID_LEN];
        ub_effective_id(parent, pid, sizeof(pid));
        cJSON_AddStringToObject(o, "parent", pid);
    }

    if (strcmp(role, "checkbox") == 0 || strcmp(role, "radio") == 0)
        ub_fill_button(h, o, role);
    if (strcmp(role, "combobox") == 0) ub_add_items_combo(h, o, max_items);
    if (strcmp(role, "listbox") == 0) ub_add_items_listbox(h, o, max_items);
    if (strcmp(role, "list") == 0) ub_add_items_listview(h, o, max_items);
    if (strcmp(role, "tabview") == 0) ub_add_items_tab(h, o, max_items);
    if (strcmp(role, "slider") == 0 || strcmp(role, "progress") == 0 ||
        strcmp(role, "spinner") == 0)
        ub_fill_range(h, o, role);

    ub_add_actions(o, role, en, readonly);
    return o;
}

typedef struct {
    cJSON *arr;
    HWND   root;
    int    order;
    int    max_items;
} UB_WALK;

static BOOL CALLBACK ub_enum_cb(HWND h, LPARAM lp)
{
    UB_WALK *w = (UB_WALK *) lp;
    cJSON_AddItemToArray(w->arr, ub_node_json(h, w->root, w->order++, w->max_items));
    return TRUE;
}

static void ub_enum_window(HWND root, cJSON *arr, int *order, int max_items)
{
    if (!root || !IsWindow(root)) return;
    UB_WALK w;
    w.arr = arr;
    w.root = root;
    w.order = *order;
    w.max_items = max_items;
    EnumChildWindows(root, ub_enum_cb, (LPARAM) &w);
    *order = w.order;
}

static cJSON *ub_virtual_json(UB_VIRTUAL *v, int order)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", v->id);
    cJSON_AddStringToObject(o, "role", v->role[0] ? v->role : "custom");
    cJSON_AddStringToObject(o, "class", "virtual");
    cJSON_AddStringToObject(o, "text", v->text);
    cJSON_AddStringToObject(o, "name", v->name);
    cJSON *rect = cJSON_CreateArray();
    cJSON_AddItemToArray(rect, cJSON_CreateNumber((double) v->rect.left));
    cJSON_AddItemToArray(rect, cJSON_CreateNumber((double) v->rect.top));
    cJSON_AddItemToArray(rect, cJSON_CreateNumber((double) v->rect.right));
    cJSON_AddItemToArray(rect, cJSON_CreateNumber((double) v->rect.bottom));
    cJSON_AddItemToObject(o, "rect", rect);
    cJSON_AddBoolToObject(o, "enabled", v->enabled);
    cJSON_AddBoolToObject(o, "visible", 1);
    cJSON_AddBoolToObject(o, "virtual", 1);
    cJSON_AddNumberToObject(o, "order", (double) order);
    cJSON_AddStringToObject(o, "parent", "root");
    ub_add_actions(o, "custom", v->enabled, 0);
    return o;
}

static char *ub_tree_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ui-tree");
    cJSON_AddNumberToObject(root, "ts", (double) GetTickCount());

    RECT mr;
    GetWindowRect(g_main, &mr);
    cJSON *mrect = cJSON_CreateArray();
    cJSON_AddItemToArray(mrect, cJSON_CreateNumber((double) mr.left));
    cJSON_AddItemToArray(mrect, cJSON_CreateNumber((double) mr.top));
    cJSON_AddItemToArray(mrect, cJSON_CreateNumber((double) mr.right));
    cJSON_AddItemToArray(mrect, cJSON_CreateNumber((double) mr.bottom));
    cJSON_AddItemToObject(root, "main_rect", mrect);

    cJSON *arr = cJSON_CreateArray();
    int order = 0;
    ub_enum_window(g_main, arr, &order, UB_ITEMS_TREE);
    for (int i = 0; i < g_attach_count; i++) {
        if (g_attach[i] && IsWindow(g_attach[i]))
            ub_enum_window(g_attach[i], arr, &order, UB_ITEMS_TREE);
    }
    for (int i = 0; i < g_virtual_count; i++)
        cJSON_AddItemToArray(arr, ub_virtual_json(&g_virtual[i], order++));

    cJSON_AddNumberToObject(root, "count", (double) cJSON_GetArraySize(arr));
    cJSON_AddItemToObject(root, "controls", arr);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* ---------------------------------------------------------- 定位 */

typedef struct {
    const char *want;
    HWND        found;
} UB_FIND;

static BOOL CALLBACK ub_find_cb(HWND h, LPARAM lp)
{
    UB_FIND *f = (UB_FIND *) lp;
    char id[UB_ID_LEN];
    ub_effective_id(h, id, sizeof(id));
    if (strcmp(id, f->want) == 0) {
        f->found = h;
        return FALSE;
    }
    return TRUE;
}

static HWND ub_find_hwnd(const char *id)
{
    UB_FIND f;
    f.want = id;
    f.found = NULL;
    EnumChildWindows(g_main, ub_find_cb, (LPARAM) &f);
    if (f.found) return f.found;
    for (int i = 0; i < g_attach_count; i++) {
        if (!g_attach[i] || !IsWindow(g_attach[i])) continue;
        EnumChildWindows(g_attach[i], ub_find_cb, (LPARAM) &f);
        if (f.found) return f.found;
        char mid[UB_ID_LEN];
        ub_effective_id(g_attach[i], mid, sizeof(mid));
        if (strcmp(mid, id) == 0) return g_attach[i];
    }
    if (g_main) {
        char mid[UB_ID_LEN];
        ub_effective_id(g_main, mid, sizeof(mid));
        if (strcmp(mid, id) == 0) return g_main;
    }
    return NULL;
}

static UB_VIRTUAL *ub_find_virtual(const char *id)
{
    for (int i = 0; i < g_virtual_count; i++) {
        if (strcmp(g_virtual[i].id, id) == 0) return &g_virtual[i];
    }
    return NULL;
}

static cJSON *ub_find_control_json(const char *id, int max_items)
{
    UB_VIRTUAL *v = ub_find_virtual(id);
    if (v) return ub_virtual_json(v, 0);
    HWND h = ub_find_hwnd(id);
    if (!h) return NULL;
    HWND root = g_main;
    HWND p = GetParent(h);
    for (int i = 0; i < g_attach_count; i++) {
        if (g_attach[i] && (h == g_attach[i] || IsChild(g_attach[i], h))) {
            root = g_attach[i];
            break;
        }
    }
    (void) p;
    return ub_node_json(h, root, 0, max_items);
}

static void ub_notify_parent(HWND h, WORD code)
{
    HWND parent = GetParent(h);
    if (!parent) return;
    SendMessageW(parent, WM_COMMAND,
                 MAKEWPARAM((WORD) GetDlgCtrlID(h), code), (LPARAM) h);
}

static void ub_set_result_text(const char *text)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "text", text ? text : "");
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (s) {
        ub_copy(g_res_result, UB_RESULT_LEN, s);
        cJSON_free(s);
    }
}

static void ub_set_result_num(const char *key, int v)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, key, (double) v);
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (s) {
        ub_copy(g_res_result, UB_RESULT_LEN, s);
        cJSON_free(s);
    }
}

static void ub_set_result_bool(const char *key, int v)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, key, v ? 1 : 0);
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (s) {
        ub_copy(g_res_result, UB_RESULT_LEN, s);
        cJSON_free(s);
    }
}

static int ub_resolve_select_index(HWND h, const char *cls, const char *text, int value)
{
    if (!text || !text[0]) return value;
    WCHAR want[256];
    ub_utf8_to_wide(text, want, (int) (sizeof(want) / sizeof(want[0])));
    if (ub_is(cls, "ComboBox")) {
        int n = (int) SendMessageW(h, CB_GETCOUNT, 0, 0);
        for (int i = 0; i < n; i++) {
            WCHAR w[256];
            SendMessageW(h, CB_GETLBTEXT, (WPARAM) i, (LPARAM) w);
            if (wcscmp(w, want) == 0) return i;
        }
    } else if (ub_is(cls, "ListBox")) {
        int n = (int) SendMessageW(h, LB_GETCOUNT, 0, 0);
        for (int i = 0; i < n; i++) {
            WCHAR w[256];
            SendMessageW(h, LB_GETTEXT, (WPARAM) i, (LPARAM) w);
            if (wcscmp(w, want) == 0) return i;
        }
    } else if (ub_is(cls, "SysTabControl")) {
        int n = (int) SendMessageW(h, TCM_GETITEMCOUNT, 0, 0);
        for (int i = 0; i < n; i++) {
            WCHAR w[128];
            TCITEMW ti;
            memset(&ti, 0, sizeof(ti));
            ti.mask = TCIF_TEXT;
            ti.pszText = w;
            ti.cchTextMax = (int) (sizeof(w) / sizeof(w[0]));
            SendMessageW(h, TCM_GETITEMW, (WPARAM) i, (LPARAM) &ti);
            if (wcscmp(w, want) == 0) return i;
        }
    }
    return value;
}

static void ub_execute(const char *id, const char *action, const char *text,
                       int value, int *code, char *msg, int cap)
{
    *code = 0;
    snprintf(msg, (size_t) cap, "ok");
    g_res_result[0] = '\0';

    UB_VIRTUAL *v = ub_find_virtual(id);
    if (v) {
        if (!v->enabled) {
            *code = 409;
            snprintf(msg, (size_t) cap, "virtual control disabled");
            return;
        }
        if (strcmp(action, "click") == 0) {
            if (v->on_click) v->on_click(v->user);
            ui_bridge_log("virtual click %s", id);
            return;
        }
        if (strcmp(action, "set_text") == 0) {
            ub_copy(v->text, (int) sizeof(v->text), text ? text : "");
            if (v->on_action)
                v->on_action(action, text, value, g_res_result, UB_RESULT_LEN, v->user);
            ui_bridge_log("virtual set_text %s", id);
            return;
        }
        if (v->on_action) {
            v->on_action(action, text, value, g_res_result, UB_RESULT_LEN, v->user);
            ui_bridge_log("virtual %s %s", action, id);
            return;
        }
        *code = 400;
        snprintf(msg, (size_t) cap, "virtual control supports only click/set_text");
        return;
    }

    HWND h = ub_find_hwnd(id);
    if (!h) {
        *code = 404;
        snprintf(msg, (size_t) cap, "control not found: %s", id);
        return;
    }

    char cls[128];
    ub_class_name(h, cls, sizeof(cls));
    const char *role = ub_role_of(h, cls);

    if (strcmp(action, "click") == 0) {
        if (!IsWindowEnabled(h)) {
            *code = 409;
            snprintf(msg, (size_t) cap, "control disabled");
            return;
        }
        if (ub_is(cls, "Button"))
            SendMessageW(h, BM_CLICK, 0, 0);
        else {
            SetFocus(h);
            ub_notify_parent(h, BN_CLICKED);
        }
        ui_bridge_log("click %s (%s)", id, cls);
        return;
    }

    if (strcmp(action, "get_text") == 0) {
        char t[512];
        ub_window_text(h, t, sizeof(t));
        ub_set_result_text(t);
        ui_bridge_log("get_text %s", id);
        return;
    }

    if (strcmp(action, "set_text") == 0) {
        WCHAR w[512];
        ub_utf8_to_wide(text ? text : "", w, (int) (sizeof(w) / sizeof(w[0])));
        SendMessageW(h, WM_SETTEXT, 0, (LPARAM) w);
        ui_bridge_log("set_text %s", id);
        return;
    }

    if (strcmp(action, "clear") == 0) {
        SendMessageW(h, WM_SETTEXT, 0, (LPARAM) L"");
        ui_bridge_log("clear %s", id);
        return;
    }

    if (strcmp(action, "focus") == 0) {
        SetFocus(h);
        return;
    }

    if (strcmp(action, "check") == 0 || strcmp(action, "uncheck") == 0 ||
        strcmp(action, "toggle") == 0) {
        if (strcmp(role, "checkbox") != 0 && strcmp(role, "radio") != 0) {
            *code = 400;
            snprintf(msg, (size_t) cap, "check unsupported for %s", role);
            return;
        }
        int want;
        if (strcmp(action, "toggle") == 0) {
            LRESULT st = SendMessageW(h, BM_GETCHECK, 0, 0);
            want = (st == BST_CHECKED) ? BST_UNCHECKED : BST_CHECKED;
        } else {
            want = (strcmp(action, "check") == 0) ? BST_CHECKED : BST_UNCHECKED;
        }
        SendMessageW(h, BM_SETCHECK, (WPARAM) want, 0);
        ub_notify_parent(h, BN_CLICKED);
        ub_set_result_bool("checked", want == BST_CHECKED);
        ui_bridge_log("%s %s -> %d", action, id, want == BST_CHECKED);
        return;
    }

    if (strcmp(action, "select") == 0) {
        int idx = ub_resolve_select_index(h, cls, text, value);
        if (ub_is(cls, "ComboBox")) {
            SendMessageW(h, CB_SETCURSEL, (WPARAM) idx, 0);
            ub_notify_parent(h, CBN_SELCHANGE);
        } else if (ub_is(cls, "ListBox")) {
            SendMessageW(h, LB_SETCURSEL, (WPARAM) idx, 0);
            ub_notify_parent(h, LBN_SELCHANGE);
        } else if (ub_is(cls, "SysListView")) {
            int n = (int) SendMessageW(h, LVM_GETITEMCOUNT, 0, 0);
            for (int i = 0; i < n; i++) {
                LVITEMW it;
                memset(&it, 0, sizeof(it));
                it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                it.state = (i == idx) ? (LVIS_SELECTED | LVIS_FOCUSED) : 0;
                SendMessageW(h, LVM_SETITEMSTATE, (WPARAM) i, (LPARAM) &it);
            }
            SetFocus(h);
        } else if (ub_is(cls, "SysTabControl")) {
            SendMessageW(h, TCM_SETCURSEL, (WPARAM) idx, 0);
            /* 通知父窗 NM_ 较复杂；发 WM_NOTIFY 可选，P0 先设选中 */
            HWND parent = GetParent(h);
            if (parent) {
                NMHDR hdr;
                hdr.hwndFrom = h;
                hdr.idFrom = (UINT_PTR) GetDlgCtrlID(h);
                hdr.code = TCN_SELCHANGE;
                SendMessageW(parent, WM_NOTIFY, hdr.idFrom, (LPARAM) &hdr);
            }
        } else {
            *code = 400;
            snprintf(msg, (size_t) cap, "select unsupported for %s", cls);
            return;
        }
        ub_set_result_num("selected", idx);
        ui_bridge_log("select %s index=%d", id, idx);
        return;
    }

    if (strcmp(action, "set_value") == 0) {
        if (ub_is(cls, "msctls_trackbar") || ub_is(cls, "TrackBar")) {
            SendMessageW(h, TBM_SETPOS, TRUE, (LPARAM) value);
            HWND parent = GetParent(h);
            if (parent)
                SendMessageW(parent, WM_HSCROLL, MAKEWPARAM(TB_THUMBPOSITION, value), (LPARAM) h);
        } else if (ub_is(cls, "msctls_progress") || ub_is(cls, "Progress")) {
            SendMessageW(h, PBM_SETPOS, (WPARAM) value, 0);
        } else if (ub_is(cls, "msctls_updown") || ub_is(cls, "UpDown")) {
            SendMessageW(h, UDM_SETPOS32, 0, (LPARAM) value);
        } else {
            *code = 400;
            snprintf(msg, (size_t) cap, "set_value unsupported for %s", cls);
            return;
        }
        ub_set_result_num("value", value);
        ui_bridge_log("set_value %s = %d", id, value);
        return;
    }

    if (strcmp(action, "command") == 0) {
        SendMessageW(g_main, WM_COMMAND, MAKEWPARAM((WORD) value, 0), 0);
        ui_bridge_log("command %d", value);
        return;
    }

    /* P1 占位：明确未实现，capabilities 不列出 */
    if (strcmp(action, "key") == 0 || strcmp(action, "scroll") == 0 ||
        strcmp(action, "dblclick") == 0 || strcmp(action, "expand") == 0 ||
        strcmp(action, "collapse") == 0 || strcmp(action, "close") == 0) {
        *code = 400;
        snprintf(msg, (size_t) cap, "not implemented in P0: %s", action);
        return;
    }

    *code = 400;
    snprintf(msg, (size_t) cap, "unknown action: %s", action);
}

/* -------------------------------------------------------------- 日志/API */

void ui_bridge_log(const char *fmt, ...)
{
    char msg[UB_LOG_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (!g_inited) return;
    EnterCriticalSection(&g_lock);
    UB_LOG_ENTRY *e = &g_logs[g_log_seq % UB_LOG_MAX];
    e->seq = g_log_seq;
    e->ms = (unsigned long) GetTickCount();
    ub_copy(e->text, UB_LOG_LEN, msg);
    g_log_seq++;
    LeaveCriticalSection(&g_lock);
}

BOOL ui_bridge_set_id(HWND ctl, const char *id)
{
    if (!ctl || !id) return FALSE;
    return SetPropA(ctl, "ui.id", (HANDLE) id);
}

BOOL ui_bridge_add_virtual(const char *id, const char *role, const char *name,
                           RECT rc, BOOL enabled, const char *text,
                           UI_VIRTUAL_CLICK on_click, void *user)
{
    return ui_bridge_add_virtual_ex(id, role, name, rc, enabled, text, on_click, NULL, user);
}

BOOL ui_bridge_add_virtual_ex(const char *id, const char *role, const char *name,
                              RECT rc, BOOL enabled, const char *text,
                              UI_VIRTUAL_CLICK on_click,
                              UI_VIRTUAL_ACTION on_action, void *user)
{
    if (!id || g_virtual_count >= UB_VIRTUAL_MAX) return FALSE;
    UB_VIRTUAL *v = &g_virtual[g_virtual_count++];
    memset(v, 0, sizeof(*v));
    ub_copy(v->id, UB_ID_LEN, id);
    ub_copy(v->role, 32, role && role[0] ? role : "custom");
    ub_copy(v->name, 256, name);
    ub_copy(v->text, 512, text);
    v->rect = rc;
    v->enabled = enabled;
    v->on_click = on_click;
    v->on_action = on_action;
    v->user = user;
    return TRUE;
}

BOOL ui_bridge_attach_window(HWND hwnd)
{
    if (!hwnd) return FALSE;
    for (int i = 0; i < g_attach_count; i++)
        if (g_attach[i] == hwnd) return TRUE;
    if (g_attach_count >= UB_ATTACH_MAX) return FALSE;
    g_attach[g_attach_count++] = hwnd;
    return TRUE;
}

BOOL ui_bridge_handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg != UI_BRIDGE_WM_ACTION) return FALSE;
    (void) hwnd; (void) wp; (void) lp;

    char id[UB_ID_LEN] = {0}, action[32] = {0}, text[512] = {0};
    int value = 0, have = 0, code = 0;
    char reply[512] = {0};

    EnterCriticalSection(&g_lock);
    if (g_req_pending) {
        ub_copy(id, UB_ID_LEN, g_req.id);
        ub_copy(action, 32, g_req.action);
        ub_copy(text, 512, g_req.text);
        value = g_req.value;
        have = 1;
    }
    LeaveCriticalSection(&g_lock);

    if (have) {
        ub_execute(id, action[0] ? action : "click", text, value, &code, reply, sizeof(reply));
        if (code == 0) {
            InterlockedIncrement(&g_action_count);
            EnterCriticalSection(&g_lock);
            snprintf(g_last_action, sizeof(g_last_action), "%s:%s", action, id);
            LeaveCriticalSection(&g_lock);
        }
    } else {
        code = 400;
        ub_copy(reply, sizeof(reply), "no pending request");
    }

    EnterCriticalSection(&g_lock);
    g_res_code = code;
    ub_copy(g_res_msg, sizeof(g_res_msg), reply);
    LeaveCriticalSection(&g_lock);
    SetEvent(g_done_event);
    return TRUE;
}

static char *ub_state_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "state");
    cJSON_AddNumberToObject(o, "ts", (double) GetTickCount());
    cJSON_AddNumberToObject(o, "action_count", (double) g_action_count);
    EnterCriticalSection(&g_lock);
    cJSON_AddStringToObject(o, "last_action", g_last_action);
    LeaveCriticalSection(&g_lock);

    HWND focus = GetFocus();
    if (focus) {
        char fid[UB_ID_LEN];
        ub_effective_id(focus, fid, sizeof(fid));
        cJSON_AddStringToObject(o, "focus_id", fid);
    } else {
        cJSON_AddStringToObject(o, "focus_id", "");
    }

    if (g_state_cb) {
        char buf[4096];
        buf[0] = '\0';
        g_state_cb(buf, (int) sizeof(buf), g_state_user);
        if (buf[0]) {
            cJSON *state = cJSON_Parse(buf);
            if (state) cJSON_AddItemToObject(o, "state", state);
            else cJSON_AddStringToObject(o, "state_raw", buf);
        }
    }

    char *out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return out;
}

static char *ub_logs_json(unsigned since)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "logs");
    cJSON *arr = cJSON_CreateArray();
    EnterCriticalSection(&g_lock);
    unsigned first = (g_log_seq > UB_LOG_MAX) ? g_log_seq - UB_LOG_MAX : 0;
    for (unsigned s = first; s < g_log_seq; s++) {
        if (s < since) continue;
        UB_LOG_ENTRY *e = &g_logs[s % UB_LOG_MAX];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "seq", (double) e->seq);
        cJSON_AddNumberToObject(o, "ms", (double) e->ms);
        cJSON_AddStringToObject(o, "text", e->text);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddNumberToObject(root, "next_since", (double) g_log_seq);
    LeaveCriticalSection(&g_lock);
    cJSON_AddItemToObject(root, "entries", arr);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

static char *ub_capabilities_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "capabilities");
    cJSON_AddStringToObject(o, "protocol", UI_BRIDGE_PROTOCOL);
    cJSON_AddStringToObject(o, "service", "win_ui_bridge");
    cJSON_AddNumberToObject(o, "port", (double) g_port);
    cJSON_AddBoolToObject(o, "token_required", g_token[0] ? 1 : 0);
    char title[256] = "";
    if (g_main) ub_window_text(g_main, title, sizeof(title));
    cJSON_AddStringToObject(o, "main_title", title);

    cJSON *acts = cJSON_CreateArray();
    const char *a[] = {
        "click", "set_text", "get_text", "clear",
        "check", "uncheck", "toggle", "select", "set_value",
        "focus", "command"
    };
    for (int i = 0; i < (int) (sizeof(a) / sizeof(a[0])); i++)
        cJSON_AddItemToArray(acts, cJSON_CreateString(a[i]));
    cJSON_AddItemToObject(o, "actions", acts);

    char *out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return out;
}

/* -------------------------------------------------------------------- HTTP */

static int ub_uri_is(const struct mg_str *uri, const char *s)
{
    size_t n = strlen(s);
    return uri && (size_t) uri->len == n && memcmp(uri->buf, s, n) == 0;
}

static int ub_uri_prefix(const struct mg_str *uri, const char *s)
{
    size_t n = strlen(s);
    return uri && (size_t) uri->len >= n && memcmp(uri->buf, s, n) == 0;
}

static int ub_token_ok(struct mg_http_message *hm)
{
    if (!g_token[0]) return 1;
    struct mg_str *h = mg_http_get_header(hm, "X-Ui-Token");
    if (!h) return 0;
    return (size_t) h->len == strlen(g_token) &&
           memcmp(h->buf, g_token, (size_t) h->len) == 0;
}

static void ub_send_json(struct mg_connection *c, int status, char *json)
{
    mg_http_reply(c, status,
                  "Content-Type: application/json\r\n"
                  "Access-Control-Allow-Origin: *\r\n",
                  "%s", json ? json : "{}");
    if (json) cJSON_free(json);
}

static int ub_invoke(const char *id, const char *action, const char *text, int value,
                     char *msg, int cap)
{
    EnterCriticalSection(&g_lock);
    if (g_req_pending) {
        LeaveCriticalSection(&g_lock);
        snprintf(msg, (size_t) cap, "busy: another action is in flight");
        return 429;
    }
    ub_copy(g_req.id, UB_ID_LEN, id);
    ub_copy(g_req.action, 32, action);
    ub_copy(g_req.text, 512, text);
    g_req.value = value;
    g_req_pending = 1;
    g_res_code = 0;
    g_res_msg[0] = '\0';
    g_res_result[0] = '\0';
    LeaveCriticalSection(&g_lock);

    ResetEvent(g_done_event);
    if (!PostMessageW(g_main, UI_BRIDGE_WM_ACTION, 0, 0)) {
        EnterCriticalSection(&g_lock);
        g_req_pending = 0;
        LeaveCriticalSection(&g_lock);
        snprintf(msg, (size_t) cap, "post message failed");
        return 500;
    }

    if (WaitForSingleObject(g_done_event, UB_WAIT_MS) != WAIT_OBJECT_0) {
        EnterCriticalSection(&g_lock);
        g_req_pending = 0;
        LeaveCriticalSection(&g_lock);
        snprintf(msg, (size_t) cap, "timeout: UI thread did not run the action");
        return 504;
    }

    int code;
    EnterCriticalSection(&g_lock);
    g_req_pending = 0;
    code = g_res_code;
    ub_copy(msg, cap, g_res_msg);
    LeaveCriticalSection(&g_lock);
    return code;
}

static void ub_handle_action(struct mg_connection *c, struct mg_http_message *hm)
{
    char body[2048];
    size_t n = (size_t) hm->body.len;
    if (n >= sizeof(body)) { ub_send_json(c, 413, NULL); return; }
    memcpy(body, hm->body.buf, n);
    body[n] = '\0';

    cJSON *req = cJSON_Parse(body);
    if (!req) { ub_send_json(c, 400, NULL); return; }

    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(req, "id"));
    const char *act = cJSON_GetStringValue(cJSON_GetObjectItem(req, "action"));
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(req, "text"));
    cJSON *val = cJSON_GetObjectItem(req, "value");
    if (!id || !id[0]) { cJSON_Delete(req); ub_send_json(c, 400, NULL); return; }

    char msg[512] = {0};
    int code = ub_invoke(id, act && act[0] ? act : "click", text ? text : "",
                         val ? (int) val->valuedouble : 0, msg, (int) sizeof(msg));

    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "ok", code == 0 ? 1 : 0);
    cJSON_AddNumberToObject(res, "code", (double) code);
    cJSON_AddStringToObject(res, "id", id);
    cJSON_AddStringToObject(res, "action", act && act[0] ? act : "click");
    cJSON_AddStringToObject(res, "message", msg);
    if (code == 0 && g_res_result[0]) {
        cJSON *rj = cJSON_Parse(g_res_result);
        if (rj) cJSON_AddItemToObject(res, "result", rj);
    }
    char *json = cJSON_PrintUnformatted(res);
    cJSON_Delete(res);
    cJSON_Delete(req);
    ub_send_json(c, code == 0 ? 200 : (code >= 400 ? code : 400), json);
}

static void ub_http(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev != MG_EV_HTTP_MSG) return;
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;

    if (ub_uri_is(&hm->method, "OPTIONS")) {
        mg_http_reply(c, 204,
                      "Access-Control-Allow-Origin: *\r\n"
                      "Access-Control-Allow-Methods: GET,POST,OPTIONS\r\n"
                      "Access-Control-Allow-Headers: Content-Type,X-Ui-Token\r\n",
                      "");
        return;
    }
    if (!ub_token_ok(hm)) { ub_send_json(c, 401, NULL); return; }

    if (ub_uri_is(&hm->uri, "/ui-tree")) {
        ub_send_json(c, 200, ub_tree_json());
        return;
    }
    if (ub_uri_is(&hm->uri, "/state")) {
        ub_send_json(c, 200, ub_state_json());
        return;
    }
    if (ub_uri_is(&hm->uri, "/capabilities")) {
        ub_send_json(c, 200, ub_capabilities_json());
        return;
    }
    if (ub_uri_prefix(&hm->uri, "/control")) {
        char cid[UB_ID_LEN] = {0};
        if (mg_http_get_var(&hm->query, "id", cid, sizeof(cid)) <= 0) {
            ub_send_json(c, 400, NULL);
            return;
        }
        cJSON *ctl = ub_find_control_json(cid, UB_ITEMS_FULL);
        if (!ctl) { ub_send_json(c, 404, NULL); return; }
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "control");
        cJSON_AddItemToObject(root, "control", ctl);
        char *json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        ub_send_json(c, 200, json);
        return;
    }
    if (ub_uri_is(&hm->uri, "/logs")) {
        char q[32] = {0};
        long since = 0;
        if (mg_http_get_var(&hm->query, "since", q, sizeof(q)) > 0) since = atol(q);
        ub_send_json(c, 200, ub_logs_json((unsigned) (since < 0 ? 0 : since)));
        return;
    }
    if (ub_uri_is(&hm->uri, "/action")) {
        if (!ub_uri_is(&hm->method, "POST")) { ub_send_json(c, 405, NULL); return; }
        ub_handle_action(c, hm);
        return;
    }
    if (ub_uri_is(&hm->uri, "/ping")) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", 1);
        cJSON_AddStringToObject(o, "service", "win_ui_bridge");
        cJSON_AddStringToObject(o, "protocol", UI_BRIDGE_PROTOCOL);
        char *json = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        ub_send_json(c, 200, json);
        return;
    }
    ub_send_json(c, 404, NULL);
}

static DWORD WINAPI ub_http_thread(LPVOID param)
{
    struct mg_mgr mgr;
    char url[64];
    int port = (int) (intptr_t) param;
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", port);

    mg_mgr_init(&mgr);
    if (mg_http_listen(&mgr, url, ub_http, NULL) == NULL) {
        ui_bridge_log("bridge: listen failed on %s", url);
    } else {
        ui_bridge_log("bridge: listening on %s", url);
    }
    while (g_running) mg_mgr_poll(&mgr, 50);
    mg_mgr_free(&mgr);
    return 0;
}

void ui_bridge_set_token(const char *token)
{
    ub_copy(g_token, sizeof(g_token), token);
}

void ui_bridge_set_state_provider(UI_STATE_CB cb, void *user)
{
    g_state_cb = cb;
    g_state_user = user;
}

int ui_bridge_get_port(void)
{
    return g_port;
}

int ui_bridge_start(HWND hwnd_main, int port)
{
    if (g_inited) return 1;
    if (port <= 0) port = 8086; /* P0：0 暂回落默认口；P1 再做自动选口 */
    g_main = hwnd_main;
    g_port = port;
    InitializeCriticalSection(&g_lock);
    g_done_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_done_event) return 0;
    g_running = 1;
    g_inited = 1;
    ui_bridge_log("bridge: start on port %d", port);

    HANDLE th = CreateThread(NULL, 0, ub_http_thread, (LPVOID) (intptr_t) port, 0, NULL);
    if (!th) {
        g_inited = 0;
        g_running = 0;
        return 0;
    }
    CloseHandle(th);
    return 1;
}

void ui_bridge_stop(void)
{
    if (!g_inited) return;
    g_running = 0;
    Sleep(200);
    if (g_done_event) CloseHandle(g_done_event);
    g_done_event = NULL;
    DeleteCriticalSection(&g_lock);
    g_inited = 0;
    g_attach_count = 0;
    g_virtual_count = 0;
}
