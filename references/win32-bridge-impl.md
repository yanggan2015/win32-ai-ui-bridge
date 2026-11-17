# Win32 桥接层实现细节（P0 可执行版）

## 1. 依赖与构建

| 库 | 版本 | 用途 |
| --- | --- | --- |
| mongoose | 7.x | HTTP |
| cJSON | 1.7.15+ | JSON |

```bash
cmake -B build -DUI_BRIDGE_ENABLE=ON && cmake --build build
cmake -B build-slim -DUI_BRIDGE_ENABLE=OFF && cmake --build build-slim
```

MSVC：`cl /DUI_BRIDGE_ENABLE demo_win32.c win_ui_bridge.c third_party\mongoose.c third_party\cJSON.c comctl32.lib ws2_32.lib user32.lib gdi32.lib`

## 2. 线程模型

HTTP 线程填 `g_req` → `PostMessage(UI_BRIDGE_WM_ACTION)` → UI 线程 `ub_execute` → `SetEvent`。  
**禁止** HTTP 线程直接 `SendMessage` 控件。单槽；忙 429；超时 8s → 504。

## 3. P0 已实现动作

click / set_text / get_text / clear / check / uncheck / toggle / select / set_value / focus / command  

`/capabilities.actions` 只列出上述项。P1（key/scroll/dblclick/expand/close/自动端口）未实现时返回 `400 not implemented`。

## 4. 控件消息要点

| 控件 | 读 | 写 |
| --- | --- | --- |
| CheckBox | `BM_GETCHECK` | `BM_SETCHECK` + `BN_CLICKED` |
| Edit | `GetWindowText`；树中密码 `***` | `WM_SETTEXT`（勿重复 EN_CHANGE） |
| Combo | `CB_GETCURSEL` / `GETLBTEXT` | `CB_SETCURSEL` + 补 `CBN_SELCHANGE` |
| ListBox | `LB_*` | `LB_SETCURSEL` + `LBN_SELCHANGE` |
| ListView | 选中 + 主列文字 | `LVM_SETITEMSTATE` + `LVIS_SELECTED\|FOCUSED` |
| Tab | `TCM_GETCURSEL` | `TCM_SETCURSEL` + `TCN_SELCHANGE` |
| TrackBar | `TBM_GETPOS` | `TBM_SETPOS` + `WM_HSCROLL` |

## 5. 常见坑

1. 模态框 → 504：对话框也要 `handle_message`  
2. `SetPropA` 只能传字面量 id  
3. 端口占用：启动失败要提示；换 `--ui-bridge-port=`  
4. DPI：声明 Per-Monitor V2，否则 rect 错  
5. 精简包勿定义 `UI_BRIDGE_ENABLE`

## 6. 多窗口

`ui_bridge_attach_window(hwnd)` 登记后纳入 tree/find。P0 demo 未演示弹窗；API 已具备。
