# 桥接层接口契约（AI 可读可操作）

第 0 节是总则，其余按接口分节。改动 C 侧实现时，必须保持本文件描述的字段语义不变，
否则已经写好的 Agent 探针与回归基线全部失效。

协议版本：`1.1`（见 `/capabilities`）。

## 0. 总则

- 传输：HTTP/1.1，默认 `http://127.0.0.1:8086`，仅本机。P0：`port<=0` 回落 8086；自动选口为 P1。
- 编码：请求与响应均为 UTF-8 JSON；控件文字已从 Win32 宽字符转成 UTF-8。
- 鉴权：若程序调用过 `ui_bridge_set_token`，所有请求要带 `X-Ui-Token`，否则 401。
- 一次只允许一个 `/action` 在途（并发第二个返回 429）。客户端应串行调用。
- 任何接口都不应阻塞超过 8 秒（内部超时 504）。

## 1. GET /capabilities

Agent 启动时先调一次，确认协议与支持的动作。

```json
{
  "type": "capabilities",
  "protocol": "1.1",
  "service": "win_ui_bridge",
  "port": 8086,
  "token_required": false,
  "main_title": "AI 可控 Demo",
  "actions": [
    "click", "set_text", "get_text", "clear",
    "check", "uncheck", "toggle", "select", "set_value",
    "focus", "command"
  ]
}
```

> **可执行约定**：`actions` 只列出**当前二进制已实现**的动作。P1 预留名未实现时返回 `400 not implemented`，且不得出现在本列表中。

## 2. GET /ui-tree

返回当前全部控件（子控件 + 附属窗口 + 虚拟控件），扁平数组，不嵌套。

```json
{
  "type": "ui-tree",
  "ts": 123456,
  "main_rect": [100, 100, 720, 340],
  "count": 5,
  "controls": [
    {
      "id": "btn.add",
      "role": "button",
      "class": "Button",
      "text": "累加",
      "rect": [350, 118, 440, 148],
      "enabled": true,
      "visible": true,
      "focused": false,
      "order": 2,
      "parent": "root",
      "actions": ["click", "focus", "dblclick"]
    }
  ]
}
```

### 2.1 通用字段

| 字段 | 说明 |
| --- | --- |
| `id` | 动作接口使用的唯一标识。缺失登记时为 `auto/<类名>#<控件ID>` |
| `role` | button / checkbox / radio / textbox / label / combobox / listbox / list / tree / slider / spinner / progress / tabview / statusbar / custom / unknown |
| `class` | 原始 Win32 窗口类名 |
| `text` | 控件当前文字（密码框在树中脱敏为 `***`） |
| `rect` | 屏幕坐标 `[left, top, right, bottom]`，物理像素 |
| `enabled` / `visible` / `focused` | 可用性 / 可见性 / 是否焦点 |
| `order` | 枚举顺序，勿跨版本比对 |
| `parent` | 父控件 id，顶层为 `"root"`，附属对话框为 `"dialog:<标题>"` |
| `actions` | 当前可执行动作白名单 |
| `virtual` | 仅虚拟控件为 `1` |
| `hwnd` | 可选，HWND 十进制字符串（排障） |

### 2.2 按 role 附加字段（有则填）

| 字段 | 适用 | 说明 |
| --- | --- | --- |
| `checked` | checkbox / radio | 是否勾选 |
| `value` / `min` / `max` | slider / spinner / progress | 当前值与范围 |
| `selected` | combobox / listbox / list / tabview | 当前选中索引，无选中为 `-1` |
| `items` | 列表类 | 项文字摘要，默认最多 50 项 |
| `item_count` | 列表类 | 总项数 |
| `items_truncated` | 列表类 | 为 true 时表示 items 被截断 |
| `password` / `readonly` | textbox | 密码框 / 只读 |

## 3. GET /control?id=xxx

单控件完整快照；`items` 不截断（仍受内部硬上限保护）。404 表示 id 不存在。

```json
{ "type": "control", "control": { "...同 ui-tree 节点，items 更全..." } }
```

## 4. GET /state

返回程序内部业务状态。

```json
{
  "type": "state",
  "ts": 123457,
  "action_count": 3,
  "last_action": "click:btn.add",
  "focus_id": "edit.input",
  "state": { "sum": 7, "adds": 1, "empty_input": false }
}
```

- `state` 由 `ui_bridge_set_state_provider` 提供；非法 JSON 时退化为 `state_raw`。
- 断言优先用 `state` 数字/布尔，文字只用包含匹配。

## 5. POST /action

请求：

```json
{ "id": "btn.add", "action": "click", "text": "12", "value": 0 }
```

| action | 阶段 | 适用 | 参数 | 程序内实际执行 |
| --- | --- | --- | --- | --- |
| `click` | P0 | Button / Check / Radio / 虚拟 / 其它 | — | `BM_CLICK`；非 Button 则 `SetFocus` + 父窗 `BN_CLICKED` |
| `set_text` | P0 | Edit / Combo / Static / 虚拟 | `text` | `WM_SETTEXT`（Edit 勿再补 `EN_CHANGE`） |
| `get_text` | P0 | 任意 | — | 读文字 → `result.text` |
| `clear` | P0 | Edit | — | `WM_SETTEXT ""` |
| `check` / `uncheck` / `toggle` | P0 | CheckBox（radio 支持 check） | — | `BM_SETCHECK` + `BN_CLICKED` |
| `select` | P0 | Combo / ListBox / ListView / Tab | `value` 或 `text` | 设选中并通知父窗 |
| `set_value` | P0 | TrackBar / UpDown / Progress | `value` | 设位置并通知 |
| `focus` | P0 | 任意 | — | `SetFocus` |
| `command` | P0 | 菜单 | `value`=命令 ID | 主窗 `WM_COMMAND` |
| `dblclick` / `key` / `scroll` / `expand` / `collapse` / `close` | P1 | — | — | 未实现则 400；不得列入 `/capabilities` |

成功响应：

```json
{
  "ok": true,
  "code": 0,
  "id": "edit.input",
  "action": "get_text",
  "message": "ok",
  "result": { "text": "12" }
}
```

`result` 常见字段：`text`（get_text）、`selected`（select 后索引）、`value`（set_value 后）、`checked`（check/toggle 后）。

**约定**：`ok:true` 只代表动作已送达并执行，不代表业务结果正确。判断正确性必须紧接 `GET /state` 或 `GET /control`。

## 6. GET /logs?since=N

返回 `seq >= N` 的日志（环形缓冲，最多 128 条）。

```json
{ "type": "logs", "next_since": 4,
  "entries": [{ "seq": 3, "ms": 123457, "text": "click btn.add (Button)" }] }
```

## 7. GET /ping

`{"ok":true,"service":"win_ui_bridge","protocol":"1.1"}`

## 8. 状态码

| code | 含义 | Agent 应对 |
| --- | --- | --- |
| 200 | 成功 | 继续，紧接读 `/state` 或 `/control` |
| 400 | 请求体非法 / 未知 action / 控件不支持该动作 | 修正参数，不要原样重试 |
| 401 | token 不匹配 | 检查 token |
| 404 | 控件 id 不存在 | 重读 `/ui-tree` |
| 405 | 方法不对 | 改用 POST |
| 409 | 控件 disabled / 虚拟不可用 | 先满足前置条件 |
| 413 | 请求体过大 | 缩短 text |
| 429 | 有动作在途 | 串行化，退避后重试 |
| 500 | PostMessage 失败 | 程序可能已退出 |
| 504 | UI 线程 8 秒未处理 | 模态对话框或死循环，见 impl |

## 9. ID 稳定性约定

- 格式：`<域>.<动作或字段>`，如 `btn.add`、`edit.input`、`chk.rdp`、`menu.file.open`。
- **不要用控件文字当 id**。
- 不要用自增序号当 id。
- 未登记只有 `auto/<类名>#<控件ID>`，**只能兜底**，不能写进基线。

## 10. 坐标系

`rect` 为屏幕物理像素。程序须 DPI aware（`SetProcessDpiAwarenessContext` 或 manifest）。
