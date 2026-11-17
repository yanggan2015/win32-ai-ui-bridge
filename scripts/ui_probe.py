#!/usr/bin/env python3
"""ui_probe.py — win_ui_bridge HTTP 探针（P0 动作对齐）。"""
import argparse
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

DEFAULT_BASE = "http://127.0.0.1:8086"


class BridgeError(RuntimeError):
    pass


class Bridge:
    def __init__(self, base=DEFAULT_BASE, token=None, timeout=10):
        self.base = base.rstrip("/")
        self.token = token
        self.timeout = timeout

    def _call(self, method, path, payload=None):
        url = self.base + path
        data = None
        headers = {"Accept": "application/json"}
        if payload is not None:
            data = json.dumps(payload).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if self.token:
            headers["X-Ui-Token"] = self.token
        req = urllib.request.Request(url, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                body = resp.read().decode("utf-8")
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", "replace")
            try:
                return e.code, json.loads(body) if body.strip() else {}
            except json.JSONDecodeError:
                return e.code, {"raw": body}
        except urllib.error.URLError as e:
            raise BridgeError(f"连不上桥接层 {url}：{e.reason}")
        return 200, json.loads(body) if body.strip() else {}

    def ping(self):
        code, data = self._call("GET", "/ping")
        return code == 200 and data.get("ok") is True

    def capabilities(self):
        code, data = self._call("GET", "/capabilities")
        if code != 200:
            raise BridgeError(f"GET /capabilities -> {code} {data}")
        return data

    def tree(self):
        code, data = self._call("GET", "/ui-tree")
        if code != 200:
            raise BridgeError(f"GET /ui-tree -> {code} {data}")
        return data

    def control(self, cid):
        q = urllib.parse.urlencode({"id": cid})
        code, data = self._call("GET", f"/control?{q}")
        if code != 200:
            raise BridgeError(f"GET /control -> {code} {data}")
        return data

    def state(self):
        code, data = self._call("GET", "/state")
        if code != 200:
            raise BridgeError(f"GET /state -> {code} {data}")
        return data

    def logs(self, since=0):
        code, data = self._call("GET", f"/logs?since={int(since)}")
        if code != 200:
            raise BridgeError(f"GET /logs -> {code} {data}")
        return data

    def act(self, cid, action="click", text=None, value=None):
        payload = {"id": cid, "action": action}
        if text is not None:
            payload["text"] = text
        if value is not None:
            payload["value"] = value
        code, data = self._call("POST", "/action", payload)
        if code != 200 or not data.get("ok"):
            raise BridgeError(f"动作失败 {cid} {action}: {code} {data.get('message')}")
        return data

    def click(self, cid):
        return self.act(cid, "click")

    def set_text(self, cid, text):
        return self.act(cid, "set_text", text=text)

    def get_text(self, cid):
        return self.act(cid, "get_text")

    def clear(self, cid):
        return self.act(cid, "clear")

    def check(self, cid):
        return self.act(cid, "check")

    def uncheck(self, cid):
        return self.act(cid, "uncheck")

    def toggle(self, cid):
        return self.act(cid, "toggle")

    def select(self, cid, index=None, text=None):
        return self.act(cid, "select", text=text, value=index)

    def set_value(self, cid, value):
        return self.act(cid, "set_value", value=value)

    def command(self, cmd_id):
        return self.act("root", "command", value=cmd_id)

    def find(self, cid):
        for c in self.tree().get("controls", []):
            if c.get("id") == cid:
                return c
        return None

    @staticmethod
    def dig(obj, path, default=None):
        cur = obj
        for part in path.split("."):
            if isinstance(cur, list):
                try:
                    cur = cur[int(part)]
                except (ValueError, IndexError):
                    return default
            elif isinstance(cur, dict):
                if part not in cur:
                    return default
                cur = cur[part]
            else:
                return default
        return cur

    def wait(self, path, expect, timeout=3.0, interval=0.1):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = self.state()
            got = self.dig(last, path)
            if str(got) == str(expect) or got == expect:
                return True, last
            time.sleep(interval)
        return False, last


def normalize(tree, with_rect=False):
    out = {}
    for c in tree.get("controls", []):
        item = {
            "role": c.get("role"),
            "text": c.get("text"),
            "enabled": c.get("enabled"),
            "visible": c.get("visible"),
            "checked": c.get("checked"),
            "selected": c.get("selected"),
            "value": c.get("value"),
        }
        if with_rect:
            item["rect"] = c.get("rect")
        out[c.get("id")] = item
    return out


def diff_trees(old, new, with_rect=False):
    a, b = normalize(old, with_rect), normalize(new, with_rect)
    added = sorted(set(b) - set(a))
    removed = sorted(set(a) - set(b))
    changed = []
    for cid in sorted(set(a) & set(b)):
        delta = {k: (a[cid][k], b[cid][k]) for k in a[cid] if a[cid][k] != b[cid][k]}
        if delta:
            changed.append({"id": cid, "diff": delta})
    return {"added": added, "removed": removed, "changed": changed}


def print_tree(tree, full=False):
    controls = tree.get("controls", [])
    print(f"{len(controls)} 个控件（main_rect={tree.get('main_rect')}）")
    for c in controls:
        if not full and not c.get("visible", True):
            continue
        flag = "" if c.get("enabled", True) else " [disabled]"
        extra = []
        if "checked" in c:
            extra.append(f"checked={c['checked']}")
        if "selected" in c:
            extra.append(f"sel={c['selected']}")
        if "value" in c:
            extra.append(f"val={c['value']}")
        if c.get("actions"):
            extra.append("[" + ",".join(c["actions"]) + "]")
        ex = ("  " + " ".join(extra)) if extra else ""
        print(f"  {c.get('id'):<28} {c.get('role'):<10} \"{c.get('text','')}\"{flag}{ex}")


def main(argv=None):
    ap = argparse.ArgumentParser(description="win_ui_bridge 探针")
    ap.add_argument("--base", default=DEFAULT_BASE)
    ap.add_argument("--token", default=None)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("ping")
    sub.add_parser("capabilities")
    sub.add_parser("tree").add_argument("--full", action="store_true")
    sub.add_parser("state")
    p_ctl = sub.add_parser("control")
    p_ctl.add_argument("id")
    p_logs = sub.add_parser("logs")
    p_logs.add_argument("--since", type=int, default=0)

    p_click = sub.add_parser("click")
    p_click.add_argument("id")
    p_text = sub.add_parser("set-text")
    p_text.add_argument("id")
    p_text.add_argument("text")
    p_gt = sub.add_parser("get-text")
    p_gt.add_argument("id")
    p_clear = sub.add_parser("clear")
    p_clear.add_argument("id")
    p_chk = sub.add_parser("check")
    p_chk.add_argument("id")
    p_uchk = sub.add_parser("uncheck")
    p_uchk.add_argument("id")
    p_tog = sub.add_parser("toggle")
    p_tog.add_argument("id")
    p_sel = sub.add_parser("select")
    p_sel.add_argument("id")
    p_sel.add_argument("index", type=int, nargs="?", default=None)
    p_sel.add_argument("--text", default=None)
    p_val = sub.add_parser("set-value")
    p_val.add_argument("id")
    p_val.add_argument("value", type=int)
    p_cmd = sub.add_parser("command")
    p_cmd.add_argument("cmd_id", type=int)

    p_wait = sub.add_parser("wait")
    p_wait.add_argument("path")
    p_wait.add_argument("expect")
    p_wait.add_argument("--timeout", type=float, default=3.0)

    p_save = sub.add_parser("save")
    p_save.add_argument("path")
    p_diff = sub.add_parser("diff")
    p_diff.add_argument("path")

    args = ap.parse_args(argv)
    br = Bridge(args.base, args.token)

    def dump(obj):
        print(json.dumps(obj, ensure_ascii=False, indent=2 if isinstance(obj, dict) and len(json.dumps(obj)) > 120 else None))

    if args.cmd == "ping":
        ok = br.ping()
        print("bridge ok" if ok else "bridge 未响应")
        return 0 if ok else 2
    if args.cmd == "capabilities":
        print(json.dumps(br.capabilities(), ensure_ascii=False, indent=2))
        return 0
    if args.cmd == "tree":
        t = br.tree()
        if args.full:
            print(json.dumps(t, ensure_ascii=False, indent=2))
        else:
            print_tree(t)
        return 0
    if args.cmd == "control":
        print(json.dumps(br.control(args.id), ensure_ascii=False, indent=2))
        return 0
    if args.cmd == "state":
        print(json.dumps(br.state(), ensure_ascii=False, indent=2))
        return 0
    if args.cmd == "logs":
        d = br.logs(args.since)
        for e in d.get("entries", []):
            print(f"[{e['seq']:>4}] {e['text']}")
        print(f"(next_since={d.get('next_since')})")
        return 0
    if args.cmd == "click":
        print(json.dumps(br.click(args.id), ensure_ascii=False))
        return 0
    if args.cmd == "set-text":
        print(json.dumps(br.set_text(args.id, args.text), ensure_ascii=False))
        return 0
    if args.cmd == "get-text":
        print(json.dumps(br.get_text(args.id), ensure_ascii=False))
        return 0
    if args.cmd == "clear":
        print(json.dumps(br.clear(args.id), ensure_ascii=False))
        return 0
    if args.cmd == "check":
        print(json.dumps(br.check(args.id), ensure_ascii=False))
        return 0
    if args.cmd == "uncheck":
        print(json.dumps(br.uncheck(args.id), ensure_ascii=False))
        return 0
    if args.cmd == "toggle":
        print(json.dumps(br.toggle(args.id), ensure_ascii=False))
        return 0
    if args.cmd == "select":
        print(json.dumps(br.select(args.id, index=args.index, text=args.text), ensure_ascii=False))
        return 0
    if args.cmd == "set-value":
        print(json.dumps(br.set_value(args.id, args.value), ensure_ascii=False))
        return 0
    if args.cmd == "command":
        print(json.dumps(br.command(args.cmd_id), ensure_ascii=False))
        return 0
    if args.cmd == "wait":
        ok, last = br.wait(args.path, args.expect, args.timeout)
        print(("满足" if ok else "超时未满足") + f"：{args.path} = {Bridge.dig(last, args.path)}")
        return 0 if ok else 1
    if args.cmd == "save":
        snap = br.tree()
        with open(args.path, "w", encoding="utf-8") as f:
            json.dump(snap, f, ensure_ascii=False, indent=2)
        print(f"已保存基线 {args.path}（{len(snap.get('controls', []))} 个控件）")
        return 0
    if args.cmd == "diff":
        with open(args.path, encoding="utf-8") as f:
            baseline = json.load(f)
        d = diff_trees(baseline, br.tree())
        if not (d["added"] or d["removed"] or d["changed"]):
            print("UI 与基线一致")
            return 0
        print(json.dumps(d, ensure_ascii=False, indent=2))
        return 1
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BridgeError as e:
        print(f"错误：{e}", file=sys.stderr)
        sys.exit(2)
