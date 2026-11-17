#!/usr/bin/env python3
"""lint_win32.py — 静态检查是否满足「AI 可控」最低要求。

检查项：
    E001 缺少 ui_bridge_handle_message（动作全部超时）
    E002 0.0.0.0 / INADDR_ANY（禁止对外暴露）
    W003 未注册 ui_bridge_set_state_provider
    W004 CreateWindow 附近无 ui_bridge_set_id
    W005 有 start 无 stop
    W006 set_id 非字面量
    I007 自绘无 add_virtual
退出码：0 通过 / 1 有 ERROR
"""
import argparse
import re
import sys

CREATE_RE = re.compile(r"\bCreateWindow(?:Ex)?[AW]?\s*\(")
SETID_RE = re.compile(r"\bui_bridge_set_id\s*\(")
SETID_ARG_RE = re.compile(r"\bui_bridge_set_id\s*\(\s*[^,]+,\s*([^)]*)\)")


def _call_args(text, start):
    """从函数名后的 '(' 起，做括号配平并切分顶层参数。"""
    i = text.index("(", start)
    depth, args, cur, in_str = 0, [], "", False
    while i < len(text):
        ch = text[i]
        if in_str:
            if ch == "\\":
                cur += text[i : i + 2]
                i += 2
                continue
            if ch == '"':
                in_str = False
            cur += ch
        elif ch == '"':
            in_str = True
            cur += ch
        elif ch == "(":
            depth += 1
            if depth > 1:
                cur += ch
        elif ch == ")":
            depth -= 1
            if depth == 0:
                args.append(cur.strip())
                return args
            cur += ch
        elif ch == "," and depth == 1:
            args.append(cur.strip())
            cur = ""
        else:
            cur += ch
        i += 1
    return args


def control_creations(text):
    """返回需要稳定 ID 的子控件创建调用 [(行号, 类名, 参数列表)]，排除顶层主窗口。"""
    out = []
    for m in CREATE_RE.finditer(text):
        args = _call_args(text, m.end() - 1)
        line_no = text[: m.start()].count("\n") + 1
        parent = args[7] if len(args) > 7 else ""
        if parent.strip().lower() in ("null", "0", "nullptr"):
            continue
        out.append((line_no, args[0] if args else "?", args))
    return out


def find_line(lines, regex):
    return [i for i, ln in enumerate(lines) if regex.search(ln)]


def check_file(path, quiet=False):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError as e:
        print(f"{path}: ERROR 无法读取 {e}", file=sys.stderr)
        return 1
    lines = text.splitlines()
    errors = warns = 0

    def report(level, code, line_no, msg):
        nonlocal errors, warns
        if level == "ERROR":
            errors += 1
        else:
            warns += 1
        print(f"{path}:{line_no}: {level} {code} {msg}")

    # E001
    if "ui_bridge_start" in text and "ui_bridge_handle_message" not in text:
        report("ERROR", "E001", 0, "缺少 ui_bridge_handle_message：动作消息不会被 UI 线程消费，所有 /action 都会超时")

    # E002
    for i in find_line(lines, re.compile(r"\b0\.0\.0\.0\b|INADDR_ANY")):
        report("ERROR", "E002", i + 1, "桥接层应只监听 127.0.0.1，避免把调试接口暴露到网络")

    # W003
    if "ui_bridge_start" in text and "ui_bridge_set_state_provider" not in text:
        report("WARN", "W003", 0, "未注册状态回调：AI 只能看界面，读不到 sum/items 之类内部变量，无法做深度 Debug")

    # W004 子控件创建是否都有稳定 ID
    controls = control_creations(text)
    setids = find_line(lines, SETID_RE)
    if controls:
        if len(setids) >= len(controls):
            pass
        else:
            for line_no, cls, _args in controls:
                near = [s for s in setids if line_no < s <= line_no + 80]
                if not near:
                    report("WARN", "W004", line_no,
                           f"控件 {cls[:24]} 附近没有 ui_bridge_set_id，AI 只能看到 auto/ 兜底 ID")

    # W005
    if "ui_bridge_start" in text and "ui_bridge_stop" not in text:
        report("WARN", "W005", 0, "没有 ui_bridge_stop：退出时未清理线程与端口，反复调试可能端口占用")

    # W006
    for m in SETID_ARG_RE.finditer(text):
        arg = m.group(1).strip()
        line_no = text[: m.start()].count("\n") + 1
        if not (arg.startswith('"') or arg.startswith("L\"")):
            report("WARN", "W006", line_no, f"ID 传入非字面量 {arg}：SetPropA 只存指针，栈变量会悬空")

    # I007
    drawing = find_line(lines, re.compile(r"\b(TextOut|Rectangle|Ellipse|FillRect|FillSolidRect|DrawText|GDIplus|AlphaBlend)\s*\(", re.I))
    if drawing and "ui_bridge_add_virtual" not in text:
        report("INFO", "I007", drawing[0] + 1, "检测到自绘绘制但未登记虚拟控件：自绘区域对 AI 完全不可见、不可点")

    if not quiet and not errors and not warns:
        print(f"{path}: 通过（无告警）")
    return 1 if errors else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="检查 Win32 源码的 AI 可测性")
    ap.add_argument("files", nargs="+")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    rc = 0
    for f in args.files:
        rc |= check_file(f, args.quiet)
    return rc


if __name__ == "__main__":
    sys.exit(main())
