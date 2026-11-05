#!/usr/bin/env bash
# Autonomous implementation driver for waytinyime; requires Python 3 and Pi >= 1.1.
# Run ./auto-loop.sh --help before starting an unattended (potentially paid) run.
set -euo pipefail
command -v python3 >/dev/null || { printf 'auto-loop: python3 is required\n' >&2; exit 2; }
exec python3 - "$(realpath -- "${BASH_SOURCE[0]}")" "$@" <<'PY'
import argparse
import ctypes
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time
import uuid

SCRIPT = Path(sys.argv[1])
ROOT = SCRIPT.parent
STATE_DIR = ROOT / ".cache" / "auto-loop"
STATE_FILE = STATE_DIR / "state.json"
BUILD = STATE_DIR / "build"
FCITX_DIR = Path.home() / ".local" / "share" / "fcitx5"
VERSION = 1

# Each iteration targets the first unverified milestone. Every review rechecks ALL
# criteria against the current source, rather than trusting earlier completion.
MILESTONES = [
    ("fcitx5 配置与码表", [
        ("conf", "生产入口实际加载 fcitx5 table .conf：正确解析 [InputMethod]、[Table] "
         "及相关子节/本地化键、注释、空白、默认值，按配置解析码表路径；明确 .conf "
         "与 .txt 的选用/覆盖规则。只读加载用户真实 flypy.conf、tiger.conf、tigress.conf "
         "及其 File=table/*.dict 二进制词典，必要时用 libime 解码；不得要求用户改写配置或"
         "将 .dict 当文本。处理 PageSize、AutoSelect/Length/Regex、ExactMatch、分页/选词键"
         "等与输入交互相关的实际设置，不能只解析后丢弃。须通过生产入口分别加载/查询三套"
         "配置，不能只写未接入 main 的解析器；记录选项支持范围及错误诊断。"),
        ("txt", "兼容 libime/fcitx5 官方 UTF-8 表文本格式及中文别名：KeyCode/键码、"
         "Length/码长、[Data]/[数据]、中文词条、重复编码及稳定候选顺序；正确处理 "
         "BOM/CRLF、引号与转义、分区注释和出现的 [Rule]/[Phrase]、特殊前缀语义，"
         "不得把规则/提示混作普通候选。可复用 libime，不臆造频次字段。实测加载、"
         "查询、非法输入诊断，保留现有 flypy.txt 行为；说明任何仍不支持的语义，"
         "不能以静默忽略或缩减兼容范围取得通过。"),
    ]),
    ("Wayland 上屏与候选定位", [
        ("wayland-text", "在支持 input-method-v2、virtual-keyboard-v1 的真实合成器中，"
         "用真实原生 Wayland 文本应用完成编码输入、选词、UTF-8 上屏、退格和取消；"
         "同时记录客户端实际收到的文本/协议证据，不能只验证内部回调、启动或截图。"
         "应用侧 text-input 与输入法侧 input-method 是不同协议，不要求本输入法扮演合成器。"),
        ("wayland-popup", "在同一真实 Wayland 应用中观测候选窗口随插入点移动；移动窗口、"
         "屏幕边缘、缩放/多输出及失焦隐藏均有实际几何或图像证据，无抢焦点/悬挂弹窗。"
         "遵守 input_popup_surface_v2 由合成器定位的语义，text_input_rectangle 是弹窗"
         "相对坐标的事件，不是全局坐标；不能仅因回调为空就断言定位错误或写死全局偏移。"),
    ]),
    ("Xwayland 上屏与候选定位", [
        ("xwayland-text", "由该合成器启动真正的 Xwayland（不是 Xvfb/Xorg 冒充），在真实 "
         "XIM 应用中通过 XMODIFIERS=@im=waytinyime 完成选词和中文上屏；验证接收文本、"
         "编码协商、未消费按键的 press/release 转发以及原生 Wayland/Xwayland 焦点切换。"
         "应用的 GTK_IM_MODULE/Qt 设置须确实走 XIM，不能让系统 fcitx5 代为上屏。"),
        ("xwayland-popup", "实现 XIM 候选绘制、生命周期及定位，不得保留 processor_updated "
         "空实现；消费 PreeditPosition/SpotLocation 等真实 IC 更新，将客户窗口相对坐标"
         "正确转换到弹窗使用的坐标空间，处理窗口移动、边界、焦点和缩放。用提供 spot "
         "的真实 Xwayland 应用记录光标/弹窗坐标或图像，至少两个插入位置及窗口移动。"
         "对不提供插入点的 XIM 应用明确标注几何降级，不能声称能凭空获得精确光标位置。"),
    ]),
    ("中英文切换与最终回归", [
        ("toggle", "保留并完善现有 Ctrl+Space 中英文切换；在真实 Wayland 与 Xwayland "
         "应用中分别验证中文→英文→中文，按一次只切一次、无双重处理；组合键不误吞。"
         "现有 input_processor 已有 mode.chinese 和 Ctrl+Space，不要重复造状态机。"),
        ("english", "英文模式在两类真实应用中直接输入 ASCII、空格、标点，保留 Ctrl/Alt/"
         "Super 快捷键与按键释放，无候选框或残留 preedit；切回中文后仍可正常选词上屏。"
         "切换时明确处理尚未提交的编码，不能串字或重复提交。"),
        ("lifecycle", "两前端共享一致且有文档的模式策略；反复切换应用、输入模式和启动/"
         "退出，无跨应用 preedit、卡键、重复输入、泄漏的弹窗或后台进程。完整回归全部"
         "目标，提供依赖、支持的合成器/应用、启动命令、配置/码表路径和兼容限制的文档。"
         "证据必须来自本轮构建的二进制和同一份源码，不得复用旧版本截图/日志。"),
    ]),
]
CRITERIA = {key: text for _, items in MILESTONES for key, text in items}
GOALS_HASH = hashlib.sha256(json.dumps(MILESTONES, ensure_ascii=False).encode()).hexdigest()

COMMON = """这是用户明确授权的无人值守实现循环。目标是完善现有 waytinyime 输入法，
在现有 Wayland 合成器（含其 Xwayland）下运行，不是新建合成器、窗口管理器或替换系统输入法。
遵守仓库指令；不要提问或等待人工确认，遇阻先调研并尝试安全替代。仅修改此仓库中的实现、
构建依赖与使用文档，不修改 auto-loop.sh、tests/、循环控制文件或验收标准；不新建/扩展测试用例、
测试客户端或伪造验收程序，可以运行现有测试、现有应用和工具以及生产诊断接口。
不安装 Pi 插件、不改用户/系统配置、不访问凭据、不 sudo、不提交/推送/reset/clean、不删除用户改动。
用户明确授权参考 ~/.local/share/fcitx5：只读查看 conf、txt 和其引用的系统 .dict 及符号链接目标；
不得写入/替换链接或读取、修改 *.user.dict、*.history 等个人学习数据。实际没有 .txt 时，
以项目 src/assets/flypy.txt 验证文本导入，以 libime 工具只读解码本地 .dict，不能直接假设它是文本。
运行图形验证须使用隔离的嵌套/无头真实合成器、独立 Wayland socket/Xwayland/运行目录；
不要关闭现有 fcitx5，不要向用户桌面或已有窗口注入按键。可以补充项目开发依赖及自动启动方式。
没有显示环境不是通过理由，先尝试安全的嵌套合成器；仍无法实测则如实记为未通过并指出阻塞。
所有启动的应用/合成器均由本次命令托管并在结束前清理，不留脱管后台进程；临时文件仅写本轮 runtime/。
不把 mock、grep 到符号、构建成功、内部回调、自己写的 PASS 文本当作真实上屏/定位证据。
.config/.txt 规范参考（须核实实际语义）：
https://github.com/fcitx/libime/blob/master/src/libime/table/table-text-format_zh.md
https://github.com/fcitx/fcitx5-chinese-addons/tree/master/im/table
"""

GATE = r'''set -euo pipefail
meson setup "$2" "$1"
meson compile -C "$2"
meson test -C "$2" --no-rebuild --print-errorlogs
"$2/src/waytinyime" --version
"$2/src/waytinyime" --help
'''


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")


def say(text):
    print(f"[{now()}] {text}", flush=True)


def save_json(path, value):
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")
    os.replace(temporary, path)


def digest(tests_only=False):
    if tests_only:
        paths = sorted(p.relative_to(ROOT).as_posix() for p in (ROOT / "tests").rglob("*")
                       if p.is_file() or p.is_symlink())
    else:
        raw = subprocess.check_output(["git", "ls-files", "--cached", "--others",
                                       "--exclude-standard", "-z"], cwd=ROOT)
        paths = sorted(set(os.fsdecode(p) for p in raw.split(b"\0") if p))
    h = hashlib.sha256()
    for name in paths:
        if not tests_only and name.split("/", 1)[0] in {".cache", "build", "result"}:
            continue
        p = ROOT / name
        h.update(os.fsencode(name) + b"\0")
        if p.is_symlink():
            h.update(b"link:" + os.fsencode(os.readlink(p)))
            if p.is_file():
                h.update(p.read_bytes())
        elif p.is_file():
            h.update(str(p.stat().st_mode & 0o777).encode() + b":" + p.read_bytes())
        else:
            h.update(b"missing")
        h.update(b"\0")
    if not tests_only:
        h.update(json.dumps(local_inputs(), ensure_ascii=False, sort_keys=True).encode())
    return h.hexdigest()


def local_inputs():
    # Follow Home Manager symlinks, but never touch personal history/user tables.
    paths = list((FCITX_DIR / "inputmethod").glob("*.conf"))
    paths += list((FCITX_DIR / "table").glob("*.txt"))
    paths += [p for p in (FCITX_DIR / "table").glob("*.dict") if not p.name.endswith(".user.dict")]
    return {str(p): {"resolved": str(p.resolve()), "bytes": p.stat().st_size,
                     "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
            for p in sorted(paths) if p.is_file()}


def proc_identity(pid):
    try:
        return (pid, Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19])
    except (OSError, IndexError):
        return None


def descendants(pid, found=None):
    # Pi's bash tools can have their own process groups. Track descendants too,
    # not just the top-level Pi process group, to clean them up on timeout.
    found = set() if found is None else found
    try:
        files = list(Path(f"/proc/{pid}/task").glob("*/children"))
        children = set()
        for p in files:
            try:
                children.update(int(x) for x in p.read_text().split())
            except OSError:
                pass
        for child in children:
            identity = proc_identity(child)
            if identity and identity not in found:
                found.add(identity)
                descendants(child, found)
    except OSError:
        pass
    return found


def cleanup(process, owned):
    owned.update(descendants(os.getpid()))
    for sig in (signal.SIGTERM, signal.SIGKILL):
        if process.poll() is None:
            try:
                os.killpg(process.pid, sig)
            except ProcessLookupError:
                pass
        for pid, start in owned:
            if proc_identity(pid) == (pid, start):
                try:
                    os.kill(pid, sig)
                except ProcessLookupError:
                    pass
        if sig == signal.SIGTERM:
            time.sleep(0.5)
    process.wait()
    # Reap orphaned grandchildren adopted via PR_SET_CHILD_SUBREAPER.
    while True:
        try:
            if os.waitpid(-1, os.WNOHANG)[0] == 0:
                break
        except ChildProcessError:
            break


def run(command, prefix, seconds, json_output=False):
    out_path = prefix.with_suffix(".jsonl" if json_output else ".log")
    err_path = prefix.with_suffix(".stderr.log")
    with out_path.open("wb") as out, err_path.open("wb") as err:
        process = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.DEVNULL,
                                   stdout=out, stderr=err if json_output else subprocess.STDOUT,
                                   start_new_session=True)
        deadline = time.monotonic() + seconds
        owned = set()
        try:
            while process.poll() is None:
                owned.update(descendants(os.getpid()))
                if time.monotonic() >= deadline:
                    say(f"命令超时（{seconds}s），清理本轮进程：{prefix.name}")
                    return 124
                time.sleep(0.5)
            return process.returncode
        finally:
            cleanup(process, owned)


def parse_pi(path):
    last = None
    settled = False
    calls = {}
    successful = {}
    with path.open() as stream:
        for line in stream:
            event = json.loads(line)
            if not isinstance(event, dict):
                raise ValueError("Pi JSONL 记录不是对象")
            kind = event.get("type")
            if kind == "agent_settled":
                settled = event.get("aborted") is False
            elif kind == "agent_start":
                settled = False
            elif kind == "message_end" and event.get("message", {}).get("role") == "assistant":
                last = event["message"]
            elif kind == "tool_execution_start" and event.get("toolName") == "bash":
                calls[event["toolCallId"]] = event.get("args", {}).get("command")
            elif kind == "tool_execution_end" and event.get("toolCallId") in calls:
                result = event.get("result", {})
                structured = result.get("structuredContent", {})
                command = calls[event["toolCallId"]]
                marker = re.match(r"# auto-loop-evidence: ([A-Za-z0-9_.-]{1,64})\r?\n", command or "")
                if (marker and event.get("isError", result.get("isError", True)) is False
                        and structured.get("exit_code", 0) == 0):
                    key = marker.group(1)
                    # A real check may be retried after correcting its invocation.
                    # Preserve EVERY successful execution under the marker instead
                    # of rejecting an otherwise complete, independently run review.
                    successful.setdefault(key, []).append(
                        {"command": command, "tool_call_id": event["toolCallId"]})
    if not settled or not last or last.get("stopReason") != "stop":
        raise ValueError("Pi 未正常 settled/stop（JSON 模式退出码 0 不等于任务成功）")
    text = "\n".join(b.get("text", "") for b in last.get("content", []) if b.get("type") == "text")
    return text, successful


def review_report(path, fingerprint):
    text, successful = parse_pi(path)
    # Tolerate a short explanation around ONE fenced JSON, but never choose
    # between multiple conflicting verdicts or extract arbitrary brace fragments.
    fenced = re.findall(r"```(?:json)?[ \t]*\n(.*?)\n```", text, re.S)
    if len(fenced) > 1:
        raise ValueError("验收返回多个 JSON 围栏，结论不唯一")
    report = json.loads(fenced[0] if fenced else text)
    if not isinstance(report, dict):
        raise ValueError("验收 JSON 必须是对象")
    if report.get("source_digest") != fingerprint:
        raise ValueError("验收源码指纹不符")
    checks = report.get("checks")
    if not isinstance(checks, list) or len(checks) != len(CRITERIA):
        raise ValueError("验收必须逐项覆盖全部固定目标")
    seen = set()
    for check in checks:
        if not isinstance(check, dict):
            raise ValueError("验收项必须是对象")
        key = check.get("id")
        if key not in CRITERIA or key in seen or type(check.get("passed")) is not bool:
            raise ValueError("验收包含未知/重复目标或非布尔通过值")
        seen.add(key)
        if not isinstance(check.get("reason"), str) or not check["reason"].strip():
            raise ValueError(f"{key}: 缺少结论理由")
        if check["passed"]:
            evidence = check.get("evidence")
            if not isinstance(evidence, list) or not evidence:
                raise ValueError(f"{key}: 通过但缺少证据")
            for item in evidence:
                if (not isinstance(item, dict) or not isinstance(item.get("command_id"), str)
                        or item.get("command_id") not in successful
                        or not isinstance(item.get("observation"), str)
                        or not item["observation"].strip()):
                    raise ValueError(f"{key}: 证据标识未对应本轮实际成功执行的命令")
                # Bind to the exact tool event ourselves; never ask the model to
                # reconstruct a multi-kilobyte shell command in its final JSON.
                executions = successful[item["command_id"]]
                item.update(executions[-1])
                item["executions"] = executions
    if (not isinstance(report.get("blockers"), list)
            or not isinstance(report.get("next_action"), str)):
        raise ValueError("缺少 blockers/next_action")
    # Contradictory completion is not accepted.
    if all(c["passed"] for c in checks) and report["blockers"]:
        raise ValueError("验收声称全部通过但仍报告阻塞")
    return report


def goals_text():
    return "\n\n".join(f"阶段 {i}: {title}\n" + "\n".join(f"- {k}: {v}" for k, v in items)
                         for i, (title, items) in enumerate(MILESTONES, 1))


def pi_command(args, reviewer=False):
    command = [args.pi, "--mode", "json", "--no-session", "--no-approve", "--no-mcp",
               "--no-skills", "--no-prompt-templates", "--tools",
               "read,bash" if reviewer else "read,bash,edit,write"]
    # Keep globally installed provider extensions (e.g. antigravity). Do not use
    # --no-extensions, which could silently remove the selected provider.
    model = (args.review_model or args.model) if reviewer else args.model
    if model:
        command += ["--model", model]
    return command


def gate_command(args):
    command = ["bash", "--noprofile", "--norc", "-c", GATE, "auto-loop-gate", str(ROOT), str(BUILD)]
    if args.build_env == "nix" or (args.build_env == "auto" and shutil.which("nix")):
        # The dev shell needs only tracked flake files; compilation reads the live
        # checkout, including new source files, not nix's Git-filtered package src.
        command = ["nix", "develop", "--no-update-lock-file", str(ROOT), "--command"] + command
    return command


def verify_test_log():
    rows = [json.loads(line) for line in (BUILD / "meson-logs" / "testlog.json").read_text().splitlines()
            if line.strip()]
    names = {r.get("name", "").rsplit(":", 1)[-1].rsplit("/", 1)[-1].strip() for r in rows}
    if not {"code-table", "input-engine"}.issubset(names) or any(r.get("result") != "OK" for r in rows):
        raise ValueError("既有 code-table/input-engine 测试必须实际运行且全部 OK，不能跳过/删除")


def protected_unchanged(state, script_hash, inputs):
    if hashlib.sha256(SCRIPT.read_bytes()).hexdigest() != script_hash:
        raise RuntimeError("auto-loop.sh 被修改；停止以免验收规则被改变。请人工审查后重新启动。")
    if digest(tests_only=True) != state["tests_digest"]:
        raise RuntimeError("tests/ 被修改；停止以免既有检查被削弱。请保留改动并人工审查。")
    if local_inputs() != inputs:
        raise RuntimeError("用户 fcitx5 配置/码表在运行期间变化；停止，请核查后重新启动验收。")


def load_state():
    if not STATE_FILE.exists():
        return {"version": VERSION, "goals_hash": GOALS_HASH, "root": str(ROOT), "iteration": 0,
                "tests_digest": digest(tests_only=True), "passed": [], "completed": False}
    state = json.loads(STATE_FILE.read_text())
    if state.get("version") != VERSION or state.get("goals_hash") != GOALS_HASH or state.get("root") != str(ROOT):
        raise RuntimeError("状态版本/目标/项目不匹配；请人工归档 .cache/auto-loop 后重启，不会自动覆盖。")
    return state


def main():
    global FCITX_DIR
    parser = argparse.ArgumentParser(
        prog=SCRIPT.name, description="无人值守驱动 Pi 完善 waytinyime；不是合成器，也不保证 AI 必然收敛。",
        epilog="默认持续重试直到全部目标通过；可能无限消耗模型额度/磁盘。Ctrl+C 或 SIGTERM 安全停止，"
               "同命令续跑。返回码：0=验收完成（或 status/dry-run），2=前置/保护错误，"
               "3=达到迭代上限但未完成，130=被中断。日志/进度位于 .cache/auto-loop/。")
    parser.add_argument("--fcitx-dir", type=Path, default=FCITX_DIR, help="只读参考目录；默认 ~/.local/share/fcitx5")
    parser.add_argument("--verify-first", action="store_true", help="本次首轮跳过实现，先干净构建和独立重验；未达标后恢复开发")
    parser.add_argument("--status", action="store_true", help="只读查看进度，不调用模型")
    parser.add_argument("--dry-run", action="store_true", help="显示全部目标及命令，不写文件/调用模型")
    parser.add_argument("--pi", default=os.environ.get("PI_BIN", "pi"), help="Pi 可执行文件（默认 pi）")
    parser.add_argument("--model", default=os.environ.get("AUTO_LOOP_MODEL", "vimedia/deepseek-flash-tx"),
                        help="实现模型 provider/id；默认 vimedia/deepseek-flash-tx")
    parser.add_argument("--review-model", default=os.environ.get("AUTO_LOOP_REVIEW_MODEL"), help="独立验收模型；默认同实现模型")
    parser.add_argument("--max-iterations", type=int, default=0, help="本次运行最多迭代数；0=不设上限（默认）")
    parser.add_argument("--round-timeout", type=int, default=1800, help="每次实现/验收模型调用超时秒数，默认 1800")
    parser.add_argument("--check-timeout", type=int, default=1800, help="每次干净构建及既有测试超时秒数，默认 1800")
    parser.add_argument("--retry-delay", type=int, default=15, help="未达标后的重试间隔秒数，默认 15")
    parser.add_argument("--build-env", choices=["auto", "nix", "current"], default="auto", help="auto 优先使用仓库 nix develop，否则当前环境")
    args = parser.parse_args(sys.argv[2:])
    if args.max_iterations < 0 or args.round_timeout < 1 or args.check_timeout < 1 or args.retry_delay < 0:
        parser.error("迭代数/间隔不可为负，超时必须为正")
    os.chdir(ROOT)
    FCITX_DIR = args.fcitx_dir.expanduser().absolute()
    if args.status:
        if not STATE_FILE.exists():
            say("尚未运行；执行 ./auto-loop.sh 开始。")
            return 0
        state = json.loads(STATE_FILE.read_text())
        print(json.dumps(state, ensure_ascii=False, indent=2))
        if state.get("source_digest") != digest():
            say("注意：当前源码已变化，历史完成状态不再是当前源码的通过凭证。")
        return 0
    if args.dry_run:
        print(goals_text())
        print("\n只读 fcitx5 数据：", json.dumps(local_inputs(), ensure_ascii=False, indent=2))
        print("\n实现命令：", json.dumps(pi_command(args), ensure_ascii=False))
        print("验收命令：", json.dumps(pi_command(args, True), ensure_ascii=False))
        print("硬性检查：", json.dumps(gate_command(args), ensure_ascii=False))
        return 0
    if not shutil.which(args.pi):
        raise RuntimeError(f"找不到 Pi：{args.pi}；请先安装并登录，然后重新运行。")
    if args.build_env == "nix" and not shutil.which("nix"):
        raise RuntimeError("--build-env nix 需要 nix 命令")
    top = subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip()
    if Path(top).resolve() != ROOT:
        raise RuntimeError("auto-loop.sh 必须放在 waytinyime Git 仓库根目录")
    if STATE_DIR.resolve() != STATE_DIR:
        raise RuntimeError("拒绝使用经符号链接重定向的循环状态目录")
    os.umask(0o077)
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    lock = (STATE_DIR / "lock").open("a+")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        raise RuntimeError("此仓库已有 auto-loop 运行；可使用 --status 查看状态")
    # Keep lock alive until main returns. subprocess closes it (CLOEXEC).
    # Adopt double-forked/setsid descendants so timeout cleanup also reaches
    # nested compositors/Xwayland after an intermediate launcher exits.
    if ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) != 0:
        raise RuntimeError("无法启用 Linux 子进程托管，拒绝启动可能遗留图形进程的循环")
    state = load_state()
    script_hash = hashlib.sha256(SCRIPT.read_bytes()).hexdigest()
    inputs = local_inputs()
    protected_unchanged(state, script_hash, inputs)
    current = digest()
    if state.get("source_digest") != current:
        state.update(passed=[], completed=False, source_digest=current, retry_target=None)
    state["fcitx_inputs"] = inputs
    state["fcitx_dir"] = str(FCITX_DIR)
    save_json(STATE_FILE, state)
    say("开始/恢复自动迭代；默认无次数上限，会产生模型费用。Ctrl+C 可停止；不会自动提交改动。")
    say(f"日志与状态：{STATE_DIR}")
    count = 0
    while args.max_iterations == 0 or count < args.max_iterations:
        count += 1
        state["iteration"] += 1
        iteration = state["iteration"]
        verified = set(state.get("passed", []))
        target = next(((i, title, items) for i, (title, items) in enumerate(MILESTONES, 1)
                       if any(k not in verified for k, _ in items)), None)
        retry_target = state.get("retry_target")
        if isinstance(retry_target, int) and 1 <= retry_target <= len(MILESTONES):
            title, items = MILESTONES[retry_target - 1]
            target = (retry_target, title, items)
        if args.verify_first and count == 1:
            target = None
        tag = uuid.uuid4().hex[:8]
        round_dir = STATE_DIR / "iterations" / f"{iteration:06d}-{tag}"
        round_dir.mkdir(parents=True)
        # Keep AF_UNIX socket paths below Linux's 108-byte limit (sway IPC and
        # Wayland sockets do not fit under the deeply nested iteration path).
        runtime = STATE_DIR / "rt" / tag
        runtime.mkdir(parents=True)
        previous_round = state.get("round_dir")
        state.update(completed=False, updated_at=now(), round_dir=str(round_dir),
                     previous_round_dir=previous_round, runtime_dir=str(runtime),
                     target=target[1] if target else "全部目标重新验收", status="running")
        save_json(STATE_FILE, state)
        say(f"第 {iteration} 轮；已通过 {len(verified)}/{len(CRITERIA)}；本轮目标：{state['target']}")
        feedback = {k: v for k, v in state.items() if k != "report"}
        if "report" in state:
            feedback["report"] = {k: v for k, v in state["report"].items() if k != "checks"}
            feedback["report"]["checks"] = [
                {k: v for k, v in c.items() if k != "evidence"} for c in state["report"]["checks"]]
        context = (f"\n仓库：{ROOT}\n本轮临时目录：{runtime}\n硬性检查命令：{gate_command(args)!r}\n"
                   f"用户只读 fcitx5 参考目录：{FCITX_DIR}（详见 fcitx_inputs 清单，跟随符号链接读取）\n"
                   f"最近完整验收反馈目录：{[str(p.parent) for p in sorted((STATE_DIR / 'iterations').glob('*/review-summary.txt'))[-3:]]}；"
                   "实现前读取其中最新反馈；旧证据只能用于定位，不能替代本轮重跑。\n"
                   f"上轮状态（仅供定位问题，不是证据）：\n{json.dumps(feedback, ensure_ascii=False)}\n"
                   f"\n固定验收目标：\n{goals_text()}\n")
        try:
            if target:
                prompt = COMMON + context + (
                    f"\n你是实现者。本轮优先完成阶段 {target[0]}：{target[1]}，以最小可审查改动推进，"
                    "同时优先修复 last_error/上轮指出的回归或构建阻塞，不要反复重写已经完成的模块。"
                    "先查看 git status 和相关源码，保留用户改动。若 previous_round_dir 存在，"
                    "先读其中 review-summary.txt；如果没有该文件则从 review.jsonl 读取最终 assistant "
                    "消息及其 blockers/next_action。即使证据格式错误使验收未获接受，其中的问题也要修复。"
                    "可以复用上一轮嵌套 sway/foot/xterm/zenity 的启动操作，但必须重跑当前二进制取得新证据。"
                    "每轮必须争取一个可核验增量；如果连续失败应分析日志并换方法，不能重复空转。"
                    "如 Nix 锁定依赖无法获取/哈希错误，调查并合理修复项目 flake 版本/依赖，"
                    "不要绕过构建检查或禁用测试。可以先运行 nix develop 获取开发工具。"
                    "最终简要说明实际改动、执行命令、尚未完成项及下一步，不要自行判定循环成功。"
                )
                prompt_file = round_dir / "implement.md"
                prompt_file.write_text(prompt)
                code = run(pi_command(args) + ["@" + str(prompt_file)], round_dir / "implement",
                           args.round_timeout, True)
                protected_unchanged(state, script_hash, inputs)
                if code:
                    raise ValueError(f"实现模型调用失败/超时，退出码 {code}；见 {round_dir / 'implement.stderr.log'}")
                text, _ = parse_pi(round_dir / "implement.jsonl")
                (round_dir / "implementation-summary.txt").write_text(text)
            # Always rebuild from scratch; do not accept a stale binary or old test log.
            if BUILD.is_symlink():
                raise RuntimeError("拒绝清理符号链接形式的构建目录")
            if BUILD.exists():
                shutil.rmtree(BUILD)
            fingerprint = digest()
            code = run(gate_command(args), round_dir / "checks", args.check_timeout)
            protected_unchanged(state, script_hash, inputs)
            if code:
                raise ValueError(f"干净构建/既有测试失败，退出码 {code}；见 {round_dir / 'checks.log'}")
            verify_test_log()
            if digest() != fingerprint:
                raise ValueError("构建期间源码变化，不能验收混合版本")
            prompt = COMMON + context + (
                f"\n你是独立验收者，使用新会话，不相信实现者的结论。本轮源码指纹：{fingerprint}\n"
                f"已干净构建并运行既有测试，日志：{round_dir / 'checks.log'}\n"
                f"唯一待验收二进制：{BUILD / 'src' / 'waytinyime'}\n"
                "不修改任何源码、配置、文档、测试或控制状态；只能在本轮 runtime 下写临时运行数据。"
                "read,bash 是验收工具白名单而不是 OS 沙箱；bash 也不得用来修改源码。"
                "当前默认模型不支持图像输入，不要用 read 读取截图；优先采集实际数值几何、"
                "协议日志和客户端文本，可保存截图供事后人工审查但不能据此假称自己看过图片。"
                "逐项阅读相关源码，亲自运行真实应用/协议与几何检查；优先验证当前已实现能力，"
                "明显未实现的项直接 false 并指出具体缺口。可以读取本轮日志定位，但旧日志不作通过证据。"
                "每条准备作为证据的 bash command 必须以唯一标识注释开头，格式严格为（真实换行）：\n"
                "# auto-loop-evidence: conf-load\nset -euo pipefail\n实际命令\n"
                "标识自行取简短英文名，不同检查使用不同名；重试同一检查可复用标识，控制器保留全部"
                "成功执行记录。最终 evidence 只填写 command_id 与具体观测，"
                "控制器会自动从工具事件绑定原始完整命令和 tool_call_id，无需重抄长命令。"
                "没有标识的探查命令可以运行，但不能作为 passed=true 的依据。"
                "优先复用上一轮实际可行的环境启动步骤，避免重新研究已有的嵌套图形环境。"
                "对上屏项记录实际客户端文本、应用/合成器/显示环境；对定位项记录光标及弹窗几何/图像"
                "和移动/边界结果。仅源码检查或单元测试不满足这些运行项。不能运行就 false；"
                "没有人工参与、没有实际证据时，不能推测通过。你的命令需自行检查中间步骤的退出码，"
                "避免用末尾 echo/true 掩盖失败。不要执行本循环脚本或再次调用模型。"
                "最终只输出一个 JSON 对象（可以用 json 代码围栏），结构如下，checks 必须包含所有固定 id 各一次：\n"
                + json.dumps({"source_digest": fingerprint, "checks": [
                    {"id": key, "passed": False, "reason": "具体结论/缺口及源码位置",
                     "evidence": []} for key in CRITERIA], "blockers": [],
                    "next_action": "下一轮最小可执行改进；有阻塞时列出安全恢复步骤"}, ensure_ascii=False)
                + '\n通过项 evidence 格式：[{"command_id":"conf-load",'
                  '"observation":"实际观测结果及证据路径/源码位置"}]。command_id 必须对应本轮实际成功执行的带标识命令。\n'
            )
            prompt_file = round_dir / "review.md"
            prompt_file.write_text(prompt)
            code = run(pi_command(args, True) + ["@" + str(prompt_file)], round_dir / "review",
                       args.round_timeout, True)
            protected_unchanged(state, script_hash, inputs)
            if code:
                raise ValueError(f"验收模型调用失败/超时，退出码 {code}；见 {round_dir / 'review.stderr.log'}")
            if digest() != fingerprint:
                raise ValueError("验收期间源码变化；本轮结果作废，将重新构建验收")
            text, _ = parse_pi(round_dir / "review.jsonl")
            (round_dir / "review-summary.txt").write_text(text)
            report = review_report(round_dir / "review.jsonl", fingerprint)
            save_json(round_dir / "review.json", report)
            passed = [c["id"] for c in report["checks"] if c["passed"]]
            state.update(passed=passed, source_digest=fingerprint, report=report,
                         last_error=None, retry_target=None, status="incomplete", updated_at=now())
            if len(passed) == len(CRITERIA):
                state.update(completed=True, status="completed")
                save_json(STATE_FILE, state)
                say(f"全部 {len(CRITERIA)} 项通过干净构建、既有测试与独立运行验收，自动退出。")
                say(f"最终证据：{round_dir / 'review.json'}；源码指纹：{fingerprint}")
                return 0
            say(f"本轮通过 {len(passed)}/{len(CRITERIA)}；下一步：{report['next_action']}")
        except (ValueError, OSError, subprocess.SubprocessError) as error:
            # Earlier passes are not current acceptance after a failed gate/API
            # call. Keep a work-target HINT separately, so a transient failure in
            # stage 3 does not send the implementer back to rewriting stage 1.
            protected_unchanged(state, script_hash, inputs)
            state.update(passed=[], completed=False, source_digest=digest(), last_error=str(error),
                         retry_target=target[0] if target else None,
                         status="retrying", updated_at=now())
            say(f"未达标，保留改动并继续：{error}")
        save_json(STATE_FILE, state)
        if args.max_iterations == 0 or count < args.max_iterations:
            time.sleep(args.retry_delay)
    state.update(completed=False, status="iteration_limit", updated_at=now())
    save_json(STATE_FILE, state)
    say("已达到本次迭代上限，目标未全部通过（非成功）；再次运行可续跑。")
    return 3


def interrupted(signum, frame):
    raise KeyboardInterrupt


signal.signal(signal.SIGTERM, interrupted)
try:
    sys.exit(main())
except KeyboardInterrupt:
    say("已中断并清理本次子进程；源码和日志保留，重新运行即可续跑。")
    sys.exit(130)
except (RuntimeError, OSError, ValueError, subprocess.SubprocessError) as error:
    print(f"auto-loop: {error}", file=sys.stderr)
    sys.exit(2)
PY
