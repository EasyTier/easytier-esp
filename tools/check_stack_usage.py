#!/usr/bin/env python3
"""任务栈预算的静态校验（配合 -DEASYTIER_STACK_USAGE=ON 构建）。

背景
----
设备侧三个任务（I/O 6 KiB、控制面 10 KiB、数据面 8 KiB）的栈大小是否够用，宿主测试
完全覆盖不到：只有真机跑起来才会栈溢出，而栈溢出往往表现为"莫名其妙的崩溃"。
本脚本用编译器给出的 `-fstack-usage` 数据（每个函数一帧的字节数）+ 真实调用图，
计算从每个任务入口出发的**最坏调用链**，与代码里声明的栈大小比较。

用法
----
    idf.py -DEASYTIER_STACK_USAGE=ON build
    python3 tools/check_stack_usage.py

退出码：0 表示所有任务都有余量；1 表示存在超预算的任务（或找不到数据）。
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

PROJECT = Path(__file__).resolve().parent.parent
COMPONENT_DIR = PROJECT / "components" / "easytier-esp"
BUILD_DIR = PROJECT / "build"
ELF = BUILD_DIR / "easytier-esp.elf"
CONTEXT_CPP = COMPONENT_DIR / "src" / "context.cpp"

# 任务入口符号 → (C++ 实现函数, 栈大小常量名)。
#
# 注意：`extern "C"` 入口包装只是 `et::x_task(arg)` 的一层壳，但它的机器码在部分
# 反汇编器版本下无法解码（看不到其中的 call），因此分析从 **C++ 实现函数** 开始，
# 再把入口包装自身的栈帧加上去。
TASK_ENTRIES = {
    "et_io_task_entry": ("et::io_task", "kIoTaskStack"),
    "et_proto_task_entry": ("et::proto_task", "kProtoTaskStack"),
    "et_data_task_entry": ("et::data_task", "kDataTaskStack"),
}

# 每层函数调用的额外开销（返回地址 + 对齐等）的保守估计。
CALL_OVERHEAD = 16


def find_tool(name: str) -> Optional[str]:
    """在 IDF 工具链目录里查找 xtensa 工具。"""
    roots = sorted((Path.home() / ".espressif" / "tools" / "xtensa-esp-elf").glob("*/*/bin"))
    for root in reversed(roots):
        candidate = root / f"xtensa-esp32s3-elf-{name}"
        if candidate.exists():
            return str(candidate)
    return None


def normalize(func: str) -> str:
    """把函数名归一化成 "作用域::名字"，便于把 .su 与符号表对齐。

    .su 里的名字可能带返回类型与模板实参（如 `void et::f<T>(std::span<...>)`），
    而符号表去修饰后是 `et::f<std::span<...> >(std::span<...>)`，两者只能按
    "作用域::名字" 这一层对齐。同名重载取最大帧，属于保守估计。
    """
    name = func.strip()
    # 去掉匿名命名空间的两种写法差异
    name = name.replace("(anonymous namespace)::", "").replace("{anonymous}::", "")
    paren = name.find("(")
    head = name[:paren] if paren >= 0 else name
    head = re.sub(r"<.*", "", head)  # 去掉模板实参
    parts = head.split()
    return parts[-1] if parts else head


def parse_stack_usage() -> Dict[str, int]:
    """解析所有 .su 文件，返回 归一化函数名 → 最大栈帧字节数。"""
    frames: Dict[str, int] = {}
    for su in BUILD_DIR.rglob("*.su"):
        try:
            text = su.read_text(errors="replace")
        except OSError:
            continue
        for line in text.splitlines():
            # 形如：src/io_task.cpp:123:456:void et::io_task(void*)\t96\tstatic
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            fields = parts[0].split(":", 3)
            if len(fields) < 4:
                continue
            size_field = parts[1].split(",")[0].strip()
            if not size_field.isdigit():
                continue
            func = normalize(fields[3])
            if not func:
                continue
            frames[func] = max(frames.get(func, 0), int(size_field))
    return frames


def demangle(names: Set[str]) -> Dict[str, str]:
    """用 c++filt 还原符号名（找不到工具就原样返回）。"""
    cxxfilt = find_tool("c++filt")
    if cxxfilt is None or not names:
        return {name: name for name in names}
    proc = subprocess.run(
        [cxxfilt], input="\n".join(sorted(names)), capture_output=True, text=True, check=False
    )
    out = proc.stdout.splitlines()
    if len(out) != len(names):
        return {name: name for name in names}
    return dict(zip(sorted(names), out))


def read_declared_stacks() -> Dict[str, int]:
    """从 context.cpp 读取各任务声明的栈大小（字节）。"""
    text = CONTEXT_CPP.read_text()
    declared: Dict[str, int] = {}
    for symbol, (_root, const) in TASK_ENTRIES.items():
        match = re.search(rf"constexpr\s+uint32_t\s+{const}\s*=\s*(\d+)\s*;", text)
        if match is None:
            raise SystemExit(f"context.cpp 中找不到 {const} 的定义")
        declared[symbol] = int(match.group(1))
    return declared


def build_call_graph() -> Tuple[Dict[str, Set[str]], Set[str]]:
    """用 objdump 反汇编 ELF，构建 函数 → 被调用函数集合 的调用图。"""
    objdump = find_tool("objdump")
    if objdump is None:
        raise SystemExit("找不到 xtensa-esp32s3-elf-objdump")
    if not ELF.exists():
        raise SystemExit(f"找不到 {ELF}，请先构建")

    proc = subprocess.run([objdump, "-d", "--no-show-raw-insn", str(ELF)],
                          capture_output=True, text=True, check=False)
    lines = proc.stdout.splitlines()

    symbol_re = re.compile(r"^([0-9a-f]{8}) <(.+)>:$")
    # 直接调用；以及尾调用（编译器常把"调用后立即返回"优化成 j 跳转）。
    call_re = re.compile(r"\b(?:call(?:0|4|8|12)?|j)\s+([0-9a-f]+)\s+<([^>]+)>")

    current: Optional[str] = None
    graph: Dict[str, Set[str]] = {}
    all_funcs: Set[str] = set()
    addr_to_func: Dict[int, str] = {}
    pending: List[Tuple[str, int, str]] = []

    for line in lines:
        match = symbol_re.match(line)
        if match:
            current = match.group(2)
            all_funcs.add(current)
            graph.setdefault(current, set())
            addr_to_func[int(match.group(1), 16)] = current
            continue
        if current is None:
            continue
        call = call_re.search(line)
        if call:
            pending.append((current, int(call.group(1), 16), call.group(2)))

    # 直接调用有时带符号名，有时只有地址（对象内部符号）。
    for caller, addr, name in pending:
        target = name
        if target.startswith(".") or not target:
            # 找不超过该地址的最近函数符号。
            best = None
            for start, func in addr_to_func.items():
                if start <= addr and (best is None or start > best[0]):
                    best = (start, func)
            target = best[1] if best else name
        graph.setdefault(caller, set()).add(target)

    return graph, all_funcs


def worst_path(graph: Dict[str, Set[str]], frames: Dict[str, int], entry: str):
    """记忆化求从入口出发的最大栈帧路径（环会被当作回边忽略）。"""
    memo: Dict[str, Tuple[int, List[str]]] = {}

    def solve(func: str, on_stack: Set[str]) -> Tuple[int, List[str]]:
        if func in memo:
            return memo[func]
        if func in on_stack:
            return 0, []  # 递归回边：忽略
        on_stack = on_stack | {func}
        best = 0
        best_tail: List[str] = []
        for callee in graph.get(func, ()):
            total, tail = solve(callee, on_stack)
            if total > best:
                best = total
                best_tail = tail
        result = (frames.get(func, 0) + CALL_OVERHEAD + best, [func] + best_tail)
        memo[func] = result
        return result

    return solve(entry, set())


def main() -> int:
    frames = parse_stack_usage()
    if not frames:
        print("找不到 -fstack-usage 数据；请用 idf.py -DEASYTIER_STACK_USAGE=ON build 构建",
              file=sys.stderr)
        return 1

    declared = read_declared_stacks()
    graph, all_funcs = build_call_graph()

    # .su 里的函数名是 mangled，调用图来自符号表；做一层名字映射：
    # 直接比较 mangled 名，同时对 C++ 名字尝试去前缀匹配。
    # 符号表里的 mangled 名先还原成人可读形式，再按"作用域::名字"对齐 .su。
    names = demangle(all_funcs)
    normalized_graph: Dict[str, Set[str]] = {}
    for caller, callees in graph.items():
        caller_name = normalize(names.get(caller, caller))
        normalized_graph.setdefault(caller_name, set()).update(
            normalize(names.get(c, c)) for c in callees
        )

    ok = True
    print(f"{'任务':<20}{'预算(B)':>10}{'最坏链(B)':>12}{'余量(B)':>10}  判定")
    for entry, budget in sorted(declared.items(), key=lambda kv: kv[1]):
        root, _const = TASK_ENTRIES[entry]
        # 入口包装自身的帧 + C++ 实现函数的调用链。
        stub = frames.get(normalize(entry), 0) + CALL_OVERHEAD
        total, path = worst_path(normalized_graph, frames, normalize(root))
        total += stub
        path = [normalize(entry)] + path
        margin = budget - total
        verdict = "OK" if margin > 0 else "超预算"
        if margin <= 0:
            ok = False
        print(f"{entry:<20}{budget:>10}{total:>12}{margin:>10}  {verdict}")
        if path:
            shown = path[-6:]
            chain = " → ".join(f"{f}({frames.get(f, 0)}B)" for f in shown)
            prefix = "… → " if len(path) > len(shown) else ""
            print(f"    最坏链：{prefix}{chain}")

    # 额外列出最大的若干栈帧：调用链分析依赖反汇编能否解码全部 call/j，
    # 这里给出"组件内最大的单个栈帧"，便于人工核对是否又出现了 KB 级局部数组。
    print("\n组件内最大的栈帧（前 8）:")
    component_frames = []
    for name, size in frames.items():
        if size <= 0:
            continue
        component_frames.append((size, name))
    component_frames.sort(reverse=True)
    for size, name in component_frames[:8]:
        print(f"  {size:>6} B  {name}")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
