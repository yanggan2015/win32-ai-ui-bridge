---
name: win32-ai-ui-bridge
display_name: "AI 可控 Win32 窗口工具"
description: >-
  给纯 C / Win32 原生 Windows 桌面程序加「AI 可读、可操作」桥接层：本机 HTTP 暴露
  /capabilities /ui-tree /control /state /action /logs，让 Agent 像操作 DOM 一样
  操控桌面窗口工具。用户要：AI 控制 Win32 工具、点按钮/填表/勾选/选列表、给现有 C
  程序加可操作接口、新建天生可测可控的 Win32 程序、UI 回归、MCP 操控本机应用时使用。
  反触发：Qt/WPF/Electron 已有 UIA；完全不能改源码的纯黑盒；非 Windows。
---

# AI 可控 Win32 窗口工具

结构化控件树 + 业务状态 + HTTP 白名单动作。默认开桥接，窗口即可被 AI 当工具用。

设计与边界见 `references/design.md`（先读）。契约：`protocol.md`。

## 何时启用

- AI / Agent 控制 Win32 窗口、HTTP 点按钮填表
- `CreateWindow` / `WndProc` / 托盘 + 要可操作
- UI 回归、MCP 挂桌面工具

## 三条铁律

1. 控件操作必须在 **UI 线程**（HTTP 只投递 `UI_BRIDGE_WM_ACTION`）
2. 每个控件 **稳定字符串 id**（`btn.start`）
3. 必须有 **`/state`**（业务真值）

## 交付物

| 路径 | 用途 |
| --- | --- |
| `references/design.md` | 设计：目标、P0/P1、验收 |
| `references/protocol.md` | HTTP 契约 |
| `references/win32-bridge-impl.md` | 线程/消息/坑 |
| `references/agent-loop.md` | Agent 闭环 |
| `assets/win_ui_bridge.h` `.c` | 桥接层 |
| `assets/demo_win32.c` | P0 全控件示例 |
| `assets/CMakeLists.txt` | 默认开桥接 / 可关精简包 |
| `assets/mcp_server.py` | MCP |
| `scripts/ui_probe.py` | CLI |
| `scripts/lint_win32.py` | 源码合规 |

依赖：`third_party/` 放 mongoose 7.x、cJSON 1.7.15+。

## 工作流

### 1. 摸清现状

产出控件表 / 命令表 / 状态表（无表不写断言）。

### 2. 拷贝接入

拷 `win_ui_bridge.{h,c}`；mongoose + cJSON。  
默认 `UI_BRIDGE_ENABLE`；精简包 `-DUI_BRIDGE_ENABLE=OFF`。

### 3. 最小改动

```c
#include "win_ui_bridge.h"
ui_bridge_set_id(hBtn, "btn.start");
ui_bridge_set_id(hEdit, "edit.port");
ui_bridge_set_id(hChk, "chk.mode");
if (ui_bridge_handle_message(hwnd, msg, wp, lp)) return 0;
case WM_CREATE:  ui_bridge_start(hwnd, 8086); break;
case WM_DESTROY: ui_bridge_stop(); break;
ui_bridge_set_state_provider(on_state, NULL);
ui_bridge_add_virtual("tray.x", "menu", "...", rc, TRUE, "...", cb, hwnd);
```

### 4. 自检

```bash
python scripts/ui_probe.py capabilities
python scripts/ui_probe.py tree
python scripts/ui_probe.py set-text edit.input 12
python scripts/ui_probe.py click btn.add
python scripts/ui_probe.py wait state.sum 12
python scripts/lint_win32.py path/to/src
```

### 5. Agent 闭环

`capabilities → tree → action → state/control`；失败读 logs。  
**`ok:true` ≠ 业务正确。**

## P0 控件完备表

| 前缀/控件 | 可读 | 可写 |
| --- | --- | --- |
| `btn.` 按钮 | text | click |
| `chk.` 复选 | checked | check/uncheck/toggle |
| `edit.` 编辑 | text* | set_text/get_text/clear |
| `combo.` 下拉 | selected,items | select |
| `list.` 列表 | selected,items | select |
| `tab.` 页签 | selected,items | select |
| `slider.` 滑条 | value,min,max | set_value |
| `lbl.` 标签 | text | get_text |
| `tray.`/`custom.` | text | click |

\* 密码框 tree 脱敏 `***`。

## 硬约束与验收

- 只听 127.0.0.1；建议 `--ui-token=`
- 交互控件无 `auto/`；有 state_provider
- 手测：输入→点击→state；勾选；select；set_value 各至少一次
- 精简包无 `ui_bridge_start` 符号

详阅顺序：`design.md` → `protocol.md` → `win32-bridge-impl.md` → `agent-loop.md`。
