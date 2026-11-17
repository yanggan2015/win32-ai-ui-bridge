# Win32 AI UI Bridge 设计文档

## 1. 问题与目标

纯 C / Win32 窗口对 AI 是黑盒：截图猜文字不可靠，UIA 对自绘/托盘覆盖差。

**目标（可验收）**：接入本桥接后，Agent 仅靠本机 HTTP 即可：

1. 列出全部交互控件（稳定 id + role + 关键状态）
2. 对控件执行白名单动作（点、输入、勾选、选中、设值）
3. 读业务 `/state` 判断结果（不只看界面文字）

**非目标**：远程访问、任意消息投递、无源码注入、OCR 主路径、完整模拟用户鼠标轨迹。

## 2. 设计原则（保证可执行）

| 原则 | 含义 | 落地约束 |
| --- | --- | --- |
| 白名单动作 | 只暴露常用、语义清晰的动作 | 禁止 `SendMessage(任意码)` |
| UI 线程执行 | 一切控件操作在 UI 线程 | HTTP 只 `PostMessage`，超时 8s |
| 稳定 id | 动作主键是字面量 id | 禁止用显示文字/运行时序号 |
| 状态分两层 | 控件态进 tree；业务真值进 `/state` | 生成工具必须注册 state_provider |
| 本机信任域 | 仅 127.0.0.1 | 建议 token；不做公网 |
| 增量可跑 | 先 P0 闭环，再扩 P1 | 每阶段有 demo 可手测 |

## 3. 架构（最小可运行）

```
Agent/MCP/CLI  --HTTP-->  mongoose(HTTP线程)  --PostMessage-->  WndProc(UI线程)
                              |                                    |
                         填 g_req / WaitEvent                   ub_execute
                              |                                    |
                         回 JSON <--------------------------- SetEvent(done)
```

- **单槽**：同时只跑一个 `/action`，忙则 429。不做多队列（乱序难复现）。
- **ID**：`SetPropA("ui.id")`；tree 与 find 共用 `ub_effective_id`。
- **虚拟控件**：无 HWND 区域登记进数组，click（及可选 on_action）走回调。

## 4. 协议职责切分

| 接口 | 做什么 | 不做什么 |
| --- | --- | --- |
| `/capabilities` | 协议版本、端口、支持的 action 列表 | 不返回整棵树 |
| `/ui-tree` | 全控件快照；列表 `items` 最多 50 | 不做深树全量展开 |
| `/control?id=` | 单控件详情；items 放宽 | 不替代 `/state` |
| `/state` | 业务变量 JSON | 不重复抄一遍 tree |
| `/action` | 执行白名单动作；可带 `result` | `ok` ≠ 业务成功 |
| `/logs` | 环形事件 | 不当持久日志 |

契约细节：`protocol.md`。实现坑：`win32-bridge-impl.md`。Agent 闭环：`agent-loop.md`。

## 5. 控件矩阵（P0 必做 / P1 可延后）

### P0（本版必须可演示）

| 控件 | role | 可读 | 可写 |
| --- | --- | --- | --- |
| Button | button | text, enabled, focused, actions | click, focus |
| CheckBox | checkbox | + checked | check, uncheck, toggle, click |
| Radio | radio | + checked | click, check |
| Edit | textbox | text(密码脱敏), password, readonly | set_text, get_text, clear, focus |
| Static | label | text | get_text, set_text |
| ComboBox | combobox | selected, items, item_count | select, set_text |
| ListBox | listbox | selected, items | select |
| ListView | list | selected, items(主列) | select（正确 LVITEM） |
| Tab | tabview | selected, items | select |
| TrackBar | slider | value, min, max | set_value |
| Progress | progress | value, min, max | set_value |
| UpDown | spinner | value, min, max | set_value |
| 虚拟/托盘 | custom | text, enabled | click |
| 菜单命令 | — | — | command |

### P1（接口预留，实现可随后）

`key` / `scroll` / `dblclick` / Tree `expand|collapse` / `close` / `attach_window` 多窗 / `port=0` 自动端口与 port 文件 / `add_virtual_ex`。

> 可执行性约定：P1 动作若尚未实现，`/action` 返回 400 `not implemented`，且 `/capabilities.actions` **只列出已实现项**，避免 Agent 调到空动作。

## 6. 构建与安全（默认可当工具）

| 构建 | 宏 | 结果 |
| --- | --- | --- |
| 默认 | `UI_BRIDGE_ENABLE` | 有桥接，AI 可控 |
| 精简 | 不定义 | header 空操作，无符号 |

1. 只绑 `127.0.0.1`
2. 可选 token（`ui_bridge_set_token` / `--ui-token`）
3. 默认端口 8086；`port<=0` P0 回落 8086；冲突则启动失败并提示；自动选口为 P1

## 7. 接入清单（生成工具的验收）

每个用本 skill 生成的窗口工具必须：

1. 交互控件全部 `ui_bridge_set_id`（无 `auto/`）
2. 自绘可点区域 `add_virtual`
3. `set_state_provider` 暴露至少 1 个业务字段
4. WndProc 调 `handle_message`；启停 `start`/`stop`
5. 启动日志或标题栏能看到端口
6. 手测脚本：`set_text` → `click` → `/state` 变化；`check`/`select`/`set_value` 至少各一次

## 8. Agent 闭环（可执行）

```
GET /capabilities
GET /ui-tree          # 选 id，看 actions 与状态
POST /action          # 只发 actions 里有的
GET /state 或 /control
失败 → GET /logs
```

断言优先级：`state` 字段 > `logs` > tree 的 text/checked/value > 截图。

## 9. 风险与对策

| 风险 | 对策 |
| --- | --- |
| 模态框卡住 UI → 全 504 | 文档要求对话框也 `handle_message`；Test 尽量非模态 |
| ListView select 参数错 | P0 用标准 LVITEM；demo 覆盖 |
| 密码进 tree 泄密 | tree 脱敏 `***`；本机 get_text 仍可用 |
| 范围膨胀做不完 | capabilities 只报已实现；P1 显式延后 |
| Release 误开端口 | CMake 精简选项关闭宏；验收 grep 无 `ui_bridge_start` |

## 10. 本版交付物

- C：`win_ui_bridge.h/.c`（P0 读+写 + capabilities/control）
- 契约：`protocol.md` + 本文
- demo：覆盖 Edit/Button/Check/Combo/List/Tab/Slider + virtual
- `ui_probe.py` / `mcp_server.py` 对齐已实现动作
- `SKILL.md`：生成即可控的工作流与验收
