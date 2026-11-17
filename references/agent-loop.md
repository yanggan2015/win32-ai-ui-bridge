# AI Agent 闭环（把窗口当工具）

## 1. 工具面

| 工具 | 接口 | 用途 |
| --- | --- | --- |
| `ui_capabilities` | `GET /capabilities` | 先看协议与**已实现**动作 |
| `ui_tree` | `GET /ui-tree` | id / role / 状态 / actions |
| `ui_control` | `GET /control?id=` | 单控件详情 |
| `ui_state` | `GET /state` | 业务真值 |
| `ui_action` | `POST /action` | 白名单动作 |
| `ui_logs` | `GET /logs` | 执行轨迹 |

```bash
python scripts/ui_probe.py capabilities
python scripts/ui_probe.py tree
python scripts/ui_probe.py set-text edit.input 12
python scripts/ui_probe.py click btn.add
python scripts/ui_probe.py wait state.sum 12
python scripts/ui_probe.py check chk.mode
python scripts/ui_probe.py select combo.fruit 1
python scripts/ui_probe.py set-value slider.level 80
```

MCP：`UI_BRIDGE_BASE=http://127.0.0.1:8086 python assets/mcp_server.py`

## 2. 闭环算法

```
1. capabilities()           # 只使用 listed actions
2. tree = ui_tree()
3. state0 = ui_state()
4. 选 id + action（必须在该控件 actions 内）
5. ui_action(...)           # ok:true ≠ 业务正确
6. 轮询 ui_state / ui_control 2–3s
7. 失败 → ui_logs；无日志=消息未达；有日志无状态变=业务 Bug
```

## 3. 断言优先级

1. `/state` 业务字段  
2. `/logs` 事件  
3. tree / control 的 `checked` / `selected` / `value` / `text`  
4. 截图（最后手段）

## 4. 窗口当工具示例

```
目标：用累加器算出 12+8
set_text edit.input 12 → click btn.add → wait state.sum 12
set_text edit.input 8  → click btn.add → wait state.sum 20
```

## 5. Bug 报告模板

```
【现象】…
【复现】action 序列
【期望】state.x = …
【实际】state / logs / tree 快照
【判定】消息未达 | 业务未进 | UI 未刷新
```
