#!/usr/bin/env python3
"""mcp_server.py — 把 win_ui_bridge HTTP 包成 MCP 工具（stdio）。

    UI_BRIDGE_BASE=http://127.0.0.1:8086 UI_BRIDGE_TOKEN=xxx python3 mcp_server.py
"""
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

BASE = os.environ.get("UI_BRIDGE_BASE", "http://127.0.0.1:8086").rstrip("/")
TOKEN = os.environ.get("UI_BRIDGE_TOKEN", "")

P0_ACTIONS = [
    "click", "set_text", "get_text", "clear",
    "check", "uncheck", "toggle", "select", "set_value",
    "focus", "command",
]

TOOLS = [
    {
        "name": "ui_capabilities",
        "description": "读取桥接协议版本、端口、已实现动作列表。操控窗口前先调用。",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "ui_tree",
        "description": "读取界面控件树：稳定 id、role、文字、checked/selected/value、可执行 actions。",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "ui_control",
        "description": "读取单个控件完整快照（含更全 items）。",
        "inputSchema": {
            "type": "object",
            "properties": {"id": {"type": "string"}},
            "required": ["id"],
            "additionalProperties": False,
        },
    },
    {
        "name": "ui_state",
        "description": "读取程序业务状态（真值来源，优先于界面文字断言）。",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "ui_logs",
        "description": "读取桥接/业务日志，确认动作是否执行到业务分支。",
        "inputSchema": {
            "type": "object",
            "properties": {"since": {"type": "integer"}},
            "additionalProperties": False,
        },
    },
    {
        "name": "ui_action",
        "description": "对控件执行动作（UI 线程）。P0：click/set_text/get_text/clear/check/uncheck/toggle/select/set_value/focus/command。ok 不等于业务成功，需再读 ui_state。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "id": {"type": "string"},
                "action": {"type": "string", "enum": P0_ACTIONS},
                "text": {"type": "string"},
                "value": {"type": "integer"},
            },
            "required": ["id", "action"],
            "additionalProperties": False,
        },
    },
]


def http_get(path):
    req = urllib.request.Request(BASE + path, headers=_headers(), method="GET")
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read().decode("utf-8"))


def http_post(path, payload):
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(BASE + path, data=data, headers=_headers(post=True), method="POST")
    with urllib.request.urlopen(req, timeout=15) as r:
        return json.loads(r.read().decode("utf-8"))


def _headers(post=False):
    h = {"Accept": "application/json"}
    if post:
        h["Content-Type"] = "application/json"
    if TOKEN:
        h["X-Ui-Token"] = TOKEN
    return h


def call_tool(name, args):
    if name == "ui_capabilities":
        return http_get("/capabilities")
    if name == "ui_tree":
        return http_get("/ui-tree")
    if name == "ui_control":
        q = urllib.parse.urlencode({"id": args["id"]})
        return http_get(f"/control?{q}")
    if name == "ui_state":
        return http_get("/state")
    if name == "ui_logs":
        return http_get(f"/logs?since={int(args.get('since', 0))}")
    if name == "ui_action":
        payload = {"id": args["id"], "action": args["action"]}
        if "text" in args:
            payload["text"] = args["text"]
        if "value" in args:
            payload["value"] = args["value"]
        return http_post("/action", payload)
    raise ValueError(f"unknown tool {name}")


def reply(rid, result=None, error=None):
    msg = {"jsonrpc": "2.0", "id": rid}
    if error is not None:
        msg["error"] = error
    else:
        msg["result"] = result
    sys.stdout.write(json.dumps(msg, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def handle(req):
    rid = req.get("id")
    method = req.get("method", "")
    params = req.get("params") or {}

    if method == "initialize":
        return reply(rid, {
            "protocolVersion": params.get("protocolVersion", "2024-11-05"),
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "win-ui-bridge", "version": "1.1.0"},
        })
    if method in ("notifications/initialized", "notifications/cancelled"):
        return None
    if method == "ping":
        return reply(rid, {})
    if method == "tools/list":
        return reply(rid, {"tools": TOOLS})
    if method == "tools/call":
        name = params.get("name")
        args = params.get("arguments") or {}
        try:
            out = call_tool(name, args)
            return reply(rid, {
                "content": [{"type": "text", "text": json.dumps(out, ensure_ascii=False)}],
                "isError": False,
            })
        except urllib.error.URLError as e:
            return reply(rid, {"content": [{"type": "text", "text": f"桥接层不可达：{e}"}], "isError": True})
        except Exception as e:  # noqa: BLE001
            return reply(rid, {"content": [{"type": "text", "text": f"工具执行失败：{e}"}], "isError": True})
    if rid is not None:
        return reply(rid, error={"code": -32601, "message": f"method not found: {method}"})
    return None


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except json.JSONDecodeError:
            reply(None, error={"code": -32700, "message": "parse error"})
            continue
        handle(req)


if __name__ == "__main__":
    main()
