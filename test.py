import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import time

COMPILER_CMD = "./compiler"
SYLIB_C = "src/lib/sylib.c"
SYLIB_INCLUDE_DIR = str(Path(SYLIB_C).parent)
GCC_CMD = "riscv64-linux-gnu-gcc"
CLANG_CMD = "clang"
QEMU_CMD = "qemu-riscv64"
TIMEOUT = 60.0

GCC_FLAGS = ["-static", "-I", SYLIB_INCLUDE_DIR]
CLANG_RISCV_FLAGS = ["--target=riscv64-linux-gnu", "-static", "-I", SYLIB_INCLUDE_DIR]
DEFAULT_OUT_DIR = "out"

TEST_DIR_ALIASES = {
    "f": "test/official_Functional/functional_recover/functional",
    "hf": "test/official_Functional/functional_recover/h_functional",
    "p": "test/official_Performance",
}

TEST_DIR_GROUPS = {
    "all": ["f", "hf", "p"],
}

IR_PASSES = [
    "frontend",
    "liunswitch",
    "whiletofor",
    "redproj",
    "ipmm",
    "unswitch",
    "hdce",
    "lowerfor",
    "hmem2reg",
    "hlicm",
    "flatten",
    "mem2reg",
    "memo",
    "pretce",
    "inline",
    "posttce",
    # "constspec", 
    # "postinline",
    "dfe",
    "sr",
    "loopsimplify",
    "looprotate",
    "lcssa",
    "loopunroll",
    "predle",
    # "loopgvn",
    "lsr",
    "onlylicm",
    "licm",
    "postdle",
]

ITER_LICM_PASS_RE = re.compile(
    r"^licm\d+-(only|cf|cse|gvn|gvnhoist|instsimplify|simplifycfg|dse|dce)$"
)


def die(msg: str) -> None:
    print(f"\033[31merror:\033[0m {msg}")
    sys.exit(1)


def warn(msg: str) -> None:
    print(f"\033[33mwarning:\033[0m {msg}")


def is_valid_ir_pass(pass_name: str) -> bool:
    return pass_name in IR_PASSES or bool(ITER_LICM_PASS_RE.match(pass_name))


def normalize_case_name(name: str) -> str:
    if name.isdigit() and 0 <= int(name) <= 9:
        return f"0{name}"
    return name


def resolve_dir_alias(raw_dir: str) -> str:
    return TEST_DIR_ALIASES.get(raw_dir, raw_dir)


def resolve_scope_dirs(raw_dir: str) -> list[str]:
    if raw_dir in TEST_DIR_GROUPS:
        return [resolve_dir_alias(alias) for alias in TEST_DIR_GROUPS[raw_dir]]
    return [resolve_dir_alias(raw_dir)]


def find_case_in_dir(test_dir: str, query: str) -> str:
    query = normalize_case_name(query)
    query_stem = Path(query).stem
    if not os.path.isdir(test_dir):
        die(f"找不到目录: {test_dir}")

    matches = []
    for root, _, files in os.walk(test_dir):
        for file in files:
            if not file.endswith(".sy"):
                continue
            stem = Path(file).stem
            if query == file or query_stem == stem or query_stem in stem:
                matches.append(os.path.join(root, file))

    if not matches:
        die(f"no file: {query}")
    if len(matches) > 1:
        joined = "\n".join(matches)
        die(f"ambiguous name: {query}\n{joined}")
    return matches[0]


def resolve_test_target(args) -> tuple[str | None, list[str]]:
    if args.file:
        scope, query = args.file
        test_dir = resolve_dir_alias(scope)
        sy_path = find_case_in_dir(test_dir, query)
        return sy_path, [test_dir]

    return None, resolve_scope_dirs(args.dir)


def dump_ir(sy_path: str, passes: list[str], out_root: str) -> None:
    base_name = Path(sy_path).stem
    case_dir = Path(out_root) / base_name
    case_dir.mkdir(parents=True, exist_ok=True)

    for p in passes:
        out_file = case_dir / f"{p}.ir"
        with open(out_file, "w", encoding="utf-8") as f:
            try:
                subprocess.run(
                    [COMPILER_CMD, sy_path, f"--dump-{p}-ir"],
                    stdout=f,
                    stderr=subprocess.DEVNULL,
                    check=True,
                    timeout=TIMEOUT,
                )
            except subprocess.CalledProcessError as e:
                die(f"导出 IR 失败: pass={p}, file={sy_path}, exit={e.returncode}")

    print(f"IR 已输出到 {case_dir}")


def dump_cfg_ir(sy_path: str, out_root: str) -> None:
    base_name = Path(sy_path).stem
    case_dir = Path(out_root) / base_name
    case_dir.mkdir(parents=True, exist_ok=True)

    out_file = case_dir / "cfg.ir"
    with open(out_file, "w", encoding="utf-8") as f:
        subprocess.run(
            [COMPILER_CMD, sy_path, "--dump-cfg-ir"],
            stdout=f,
            stderr=subprocess.DEVNULL,
            check=True,
            timeout=TIMEOUT,
        )

    print(f"最终 CFG IR 已输出到 {out_file}")


def build_exec(asm_path: str, exe_path: str) -> tuple[bool, str]:
    try:
        subprocess.run(
            [GCC_CMD] + GCC_FLAGS + [asm_path, SYLIB_C, "-o", exe_path],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True,
        )
        return True, ""
    except FileNotFoundError:
        return False, f"找不到交叉编译器: {GCC_CMD}"
    except subprocess.CalledProcessError as e:
        err = e.stderr.decode("utf-8", errors="ignore").strip()
        return False, err or "汇编/链接失败"


def runner_kind(runner: str) -> tuple[str, str | None]:
    runner = runner.lower()
    aliases = {
        "o": "ours",
        "g0": "gcc-o0",
        "g1": "gcc-o1",
        "g2": "gcc-o2",
        "g3": "gcc-o3",
        "gs": "gcc-os",
        "gz": "gcc-oz",
        "c0": "clang-o0",
        "c1": "clang-o1",
        "c2": "clang-o2",
        "c3": "clang-o3",
        "cs": "clang-os",
        "cz": "clang-oz",
    }
    runner = aliases.get(runner, runner)
    if runner == "ours":
        return "ours", None

    for prefix in ("gcc", "clang"):
        if runner.startswith(prefix + "-o"):
            opt = "-" + runner[len(prefix) + 1 :].upper()
            if opt in ("-O0", "-O1", "-O2", "-O3", "-OS", "-OZ"):
                return prefix, opt

    die(
        f"未知 runner: {runner}；可用示例: "
        "o, g2, g3, c2, c3, ours, gcc-o2, gcc-o3, clang-o2, clang-o3"
    )
    raise AssertionError("unreachable")


def normalize_runner_name(runner: str) -> str:
    runner = runner.lower()
    aliases = {
        "o": "ours",
        "g0": "gcc-o0",
        "g1": "gcc-o1",
        "g2": "gcc-o2",
        "g3": "gcc-o3",
        "gs": "gcc-os",
        "gz": "gcc-oz",
        "c0": "clang-o0",
        "c1": "clang-o1",
        "c2": "clang-o2",
        "c3": "clang-o3",
        "cs": "clang-os",
        "cz": "clang-oz",
    }
    return aliases.get(runner, runner)


def build_sysy_c_input(sy_path: str, c_path: str) -> None:
    """把 sylib.c 和 SysY 源拼成一个 C 翻译单元，供 gcc/clang baseline 使用。"""
    with open(SYLIB_C, "r", encoding="utf-8") as f:
        sylib = f.read()
    with open(sy_path, "r", encoding="utf-8") as f:
        source = f.read()

    # sylib.c 已 include sylib.h；把 SysY 源接在后面可直接使用 starttime 宏。
    with open(c_path, "w", encoding="utf-8") as f:
        f.write(sylib)
        f.write("\n\n#line 1 \"")
        f.write(os.path.basename(sy_path).replace("\\", "\\\\").replace('"', '\\"'))
        f.write("\"\n")
        f.write(source)
        f.write("\n")


def build_baseline_exec(
    sy_path: str,
    exe_path: str,
    runner: str,
    baseline_cc: str,
    baseline_clang: str,
    clang_extra: list[str],
) -> tuple[bool, str]:
    kind, opt = runner_kind(runner)
    assert opt is not None

    with tempfile.TemporaryDirectory() as tmpdir:
        c_path = os.path.join(tmpdir, "combined.c")
        build_sysy_c_input(sy_path, c_path)

        if kind == "gcc":
            cmd = [baseline_cc] + GCC_FLAGS + [opt, "-x", "c", c_path, "-o", exe_path]
        else:
            cmd = [baseline_clang] + clang_extra + [opt, "-x", "c", c_path, "-o", exe_path]

        try:
            subprocess.run(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=True,
                timeout=TIMEOUT,
            )
            return True, ""
        except FileNotFoundError:
            return False, f"找不到 baseline 编译器: {cmd[0]}"
        except subprocess.TimeoutExpired:
            return False, "baseline 编译超时"
        except subprocess.CalledProcessError as e:
            err = e.stderr.decode("utf-8", errors="ignore").strip()
            return False, err or "baseline 编译/链接失败"


_TOTAL_RE = re.compile(
    r"TOTAL:\s*(\d+)H-(\d+)M-(\d+)S-(\d+)us", re.MULTILINE
)


def _parse_sysy_total(stderr_text: str) -> float | None:
    """从 sylib 的 stderr 输出中解析 TOTAL 计时（秒）。"""
    m = _TOTAL_RE.search(stderr_text)
    if not m:
        return None
    h, mi, s, us = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))
    return h * 3600 + mi * 60 + s + us / 1_000_000


def run_program(exe_path: str, input_data: bytes | None) -> tuple[bool, str, float]:
    try:
        start_time = time.time()
        result = subprocess.run(
            [QEMU_CMD, exe_path],
            input=input_data,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,   # 捕获 sylib 计时输出
            timeout=TIMEOUT,
        )
        wall_elapsed = time.time() - start_time
    except FileNotFoundError:
        return False, f"找不到模拟器: {QEMU_CMD}", 0.0
    except subprocess.TimeoutExpired:
        return False, "运行超时", 0.0

    actual_output = result.stdout.decode("utf-8", errors="ignore").strip()
    actual_full = f"{actual_output}\n{result.returncode}".strip()

    # 优先使用程序内部计时（排除 QEMU 启动 + I/O 时间）
    stderr_text = result.stderr.decode("utf-8", errors="ignore")
    sysy_time = _parse_sysy_total(stderr_text)
    elapsed = sysy_time if sysy_time is not None else wall_elapsed

    return True, actual_full, elapsed


def normalize_output(text: str) -> str:
    return "\n".join(line.strip() for line in text.splitlines() if line.strip())


def compiler_cmd_for_case(sy_path: str, asm_path: str) -> list[str]:
    cmd = [COMPILER_CMD, "-S", "-o", asm_path, sy_path]
    parts = Path(sy_path).parts
    if "official_Performance" in parts:
        cmd.append("-O1")
    return cmd


def run_case_once(
    sy_path: str,
    in_path: str,
    out_path: str,
    runner: str,
    baseline_cc: str,
    baseline_clang: str,
    clang_extra: list[str],
) -> tuple[bool, str, float]:
    with tempfile.TemporaryDirectory() as tmpdir:
        asm_path = os.path.join(tmpdir, "output.s")
        exe_path = os.path.join(tmpdir, "a.out")

        kind, _ = runner_kind(runner)
        if kind == "ours":
            try:
                result = subprocess.run(
                    compiler_cmd_for_case(sy_path, asm_path),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=True,
                    timeout=TIMEOUT,
                )
            except subprocess.TimeoutExpired:
                return False, "编译超时", 0.0
            except subprocess.CalledProcessError as e:
                err = e.stderr.decode("utf-8", errors="ignore").strip()
                return False, err or "编译崩溃", 0.0

            if result.stdout:
                return False, "编译器 stdout 非空", 0.0
            if result.stderr:
                err = result.stderr.decode("utf-8", errors="ignore").strip()
                return False, f"编译器 stderr 非空: {err}", 0.0
            if not os.path.exists(asm_path) or os.path.getsize(asm_path) == 0:
                return False, "编译器未生成汇编文件", 0.0

            ok, msg = build_exec(asm_path, exe_path)
            if not ok:
                return False, msg, 0.0
        else:
            ok, msg = build_baseline_exec(
                sy_path, exe_path, runner, baseline_cc, baseline_clang, clang_extra
            )
            if not ok:
                return False, msg, 0.0

        input_data = None
        if os.path.exists(in_path):
            with open(in_path, "rb") as f:
                input_data = f.read()

        ok, program_result, elapsed = run_program(exe_path, input_data)
        if not ok:
            return False, program_result, 0.0

        with open(out_path, "r", encoding="utf-8") as f:
            expected_full = f.read().strip()

        actual_clean = normalize_output(program_result)
        expected_clean = normalize_output(expected_full)

        if actual_clean == expected_clean:
            return True, "", elapsed

        reason = (
            "输出不一致\n"
            "--- 期望输出 ---\n"
            f"{expected_clean}\n"
            "--- 实际输出 ---\n"
            f"{actual_clean}"
        )
        return False, reason, elapsed


def run_test(
    sy_path: str,
    in_path: str,
    out_path: str,
    runner: str,
    baseline_cc: str,
    baseline_clang: str,
    clang_extra: list[str],
) -> tuple[bool, str, float]:
    label = runner if runner != "ours" else "ours"
    print(f"Testing {os.path.basename(sy_path)} [{label}] ... ", end="", flush=True)
    ok, reason, elapsed = run_case_once(
        sy_path, in_path, out_path, runner, baseline_cc, baseline_clang, clang_extra
    )

    if ok:
        print(f"✅ PASS ({elapsed:.8f}s)")
        return True, "", elapsed

    if reason.startswith("输出不一致"):
        print("❌ FAIL (输出不一致)")
        print(reason)
        return False, "输出不一致", elapsed

    print(f"❌ {reason}")
    return False, reason, elapsed


def iter_cases(test_dir: str):
    if not os.path.isdir(test_dir):
        die(f"找不到目录: {test_dir}")
    for root, _, files in os.walk(test_dir):
        for file in sorted(files):
            if file.endswith(".sy"):
                yield os.path.join(root, file)


def parse_runner_list(raw: str) -> list[str]:
    runners = [normalize_runner_name(x.strip()) for x in raw.split(",") if x.strip()]
    if not runners:
        die("--compare-runners 不能为空")
    for runner in runners:
        runner_kind(runner)
    return runners


def format_time(value: float | None) -> str:
    return "-" if value is None else f"{value:.3f}s"


def run_compare(
    cases: list[tuple[str, str, str]],
    runners: list[str],
    baseline_cc: str,
    baseline_clang: str,
    clang_extra: list[str],
) -> None:
    totals = {runner: 0.0 for runner in runners}
    counts = {runner: 0 for runner in runners}
    failures: list[tuple[str, str, str]] = []

    header = ["case"] + runners
    widths = [max(18, len(header[0]))] + [max(10, len(x)) for x in runners]
    print(" ".join(h.ljust(w) for h, w in zip(header, widths)))
    print(" ".join("-" * w for w in widths))

    for sy_file, in_path, out_path in cases:
        row_times: dict[str, float | None] = {}
        row_status: dict[str, str] = {}
        case_name = os.path.basename(sy_file)

        for runner in runners:
            ok, reason, elapsed = run_case_once(
                sy_file, in_path, out_path, runner, baseline_cc, baseline_clang, clang_extra
            )
            if ok:
                row_times[runner] = elapsed
                row_status[runner] = "ok"
                totals[runner] += elapsed
                counts[runner] += 1
            else:
                row_times[runner] = None
                row_status[runner] = "fail"
                failures.append((case_name, runner, reason.splitlines()[0]))

        row = [case_name.ljust(widths[0])]
        for i, runner in enumerate(runners, start=1):
            cell = format_time(row_times[runner])
            if row_status[runner] != "ok":
                cell = "FAIL"
            row.append(cell.ljust(widths[i]))
        print(" ".join(row))

    print("=" * 40)
    print("对比汇总:")
    for runner in runners:
        print(f"  {runner:<10} total={totals[runner]:.3f}s passed={counts[runner]}/{len(cases)}")

    if len(runners) >= 2:
        base = runners[0]
        print("相对首列:")
        for runner in runners[1:]:
            if totals[base] > 0:
                diff = totals[runner] - totals[base]
                pct = diff / totals[base] * 100
                print(f"  {runner:<10} {diff:+.3f}s ({pct:+.2f}%) vs {base}")

    if failures:
        print("\n❌ 失败项:")
        for case_name, runner, reason in failures:
            print(f"  - {case_name:<25} [{runner}] {reason}")
    else:
        print("🎉 所有 runner 输出均匹配")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "-d",
        "--dir",
        type=str,
        default="all",
        help="测试目录：all=f+hf+p, f=functional, hf=h_functional, p=performance",
    )
    parser.add_argument(
        "-f",
        "--file",
        nargs=2,
        metavar=("SCOPE", "CASE"),
        help="单测：`-f p 03_sort2` / `-f f 00_main` / `-f hf 00_comment2`",
    )
    parser.add_argument("--dump-ir", action="store_true", help="将 IR 打印到仓库根目录 out/ 文件夹")
    parser.add_argument(
        "--dump-pass",
        action="append",
        help="指定 IR pass，可重复传入；传 all 表示导出所有阶段",
    )
    parser.add_argument(
        "--dump-cfg-ir",
        action="store_true",
        help="额外导出最终 CFG IR 到 out/<case>/cfg.ir",
    )
    parser.add_argument("--out-dir", type=str, default=DEFAULT_OUT_DIR, help="IR 输出目录，默认 out/")
    parser.add_argument(
        "-R",
        "--runner",
        type=str,
        default="ours",
        help="执行后端：o/g2/g3/c2/c3 或完整名 ours/gcc-o2/gcc-o3/clang-o2/clang-o3，默认 ours",
    )
    parser.add_argument("-g0", dest="quick_runner", action="store_const", const="g0", help="等价于 -R g0")
    parser.add_argument("-g1", dest="quick_runner", action="store_const", const="g1", help="等价于 -R g1")
    parser.add_argument("-g2", dest="quick_runner", action="store_const", const="g2", help="等价于 -R g2")
    parser.add_argument("-g3", dest="quick_runner", action="store_const", const="g3", help="等价于 -R g3")
    parser.add_argument("-c0", dest="quick_runner", action="store_const", const="c0", help="等价于 -R c0")
    parser.add_argument("-c1", dest="quick_runner", action="store_const", const="c1", help="等价于 -R c1")
    parser.add_argument("-c2", dest="quick_runner", action="store_const", const="c2", help="等价于 -R c2")
    parser.add_argument("-c3", dest="quick_runner", action="store_const", const="c3", help="等价于 -R c3")
    parser.add_argument(
        "-c",
        "--compare",
        nargs="?",
        const="c2",
        default="",
        metavar="RUNNERS",
        help="和 ours 对比；不带参数默认 c2，例如 -c / -c g2,g3 / -c g3,c3",
    )
    parser.add_argument(
        "-C",
        "--compare-runners",
        type=str,
        default="",
        help="逗号分隔 runner 列表并输出对比表，例如 o,g2,g3",
    )
    parser.add_argument(
        "--baseline-cc",
        type=str,
        default=GCC_CMD,
        help=f"gcc baseline 编译器，默认 {GCC_CMD}",
    )
    parser.add_argument(
        "--baseline-clang",
        type=str,
        default=CLANG_CMD,
        help=f"clang baseline 编译器，默认 {CLANG_CMD}",
    )
    parser.add_argument(
        "--clang-extra",
        type=str,
        default=" ".join(CLANG_RISCV_FLAGS),
        help="clang baseline 额外参数，默认 '--target=riscv64-linux-gnu -static'",
    )
    args = parser.parse_args()
    if args.quick_runner:
        args.runner = args.quick_runner
    args.runner = normalize_runner_name(args.runner)
    runner_kind(args.runner)
    clang_extra = shlex.split(args.clang_extra)
    compare_runners = ""
    if args.compare:
        listed = parse_runner_list(args.compare)
        listed = [x for x in listed if x != "ours"]
        compare_runners = ",".join(["ours"] + listed)
    elif args.compare_runners:
        compare_runners = args.compare_runners

    dump_mode = args.dump_ir or bool(args.dump_pass) or args.dump_cfg_ir
    requested_passes = args.dump_pass or []
    if "all" in requested_passes or (args.dump_ir and not requested_passes):
        passes = IR_PASSES
    else:
        invalid = [p for p in requested_passes if not is_valid_ir_pass(p)]
        if invalid:
            print(f"未知 IR pass: {', '.join(invalid)}")
            print("可选 pass:")
            for p in IR_PASSES:
                print(f"  - {p}")
            print("  - licm<N>-only/cf/cse/gvn/gvnhoist/instsimplify/simplifycfg/dse/dce")
            sys.exit(1)
        passes = requested_passes

    sy_path, scope_dirs = resolve_test_target(args)

    if sy_path:
        base = sy_path[:-3]
        in_path = base + ".in"
        out_path = base + ".out"
        if dump_mode:
            if passes:
                dump_ir(sy_path, passes, args.out_dir)
            if args.dump_cfg_ir:
                dump_cfg_ir(sy_path, args.out_dir)
            sys.exit(0)
        if not os.path.exists(out_path):
            die(f"找不到答案文件: {out_path}")
        if compare_runners:
            run_compare(
                [(sy_path, in_path, out_path)],
                parse_runner_list(compare_runners),
                args.baseline_cc,
                args.baseline_clang,
                clang_extra,
            )
        else:
            run_test(
                sy_path,
                in_path,
                out_path,
                args.runner,
                args.baseline_cc,
                args.baseline_clang,
                clang_extra,
            )
        sys.exit(0)

    if dump_mode:
        for scope_dir in scope_dirs:
            for path in iter_cases(scope_dir):
                if passes:
                    dump_ir(path, passes, args.out_dir)
                if args.dump_cfg_ir:
                    dump_cfg_ir(path, args.out_dir)
        sys.exit(0)

    cases = []
    skipped_count = 0
    for scope_dir in scope_dirs:
        for sy_file in iter_cases(scope_dir):
            base = sy_file[:-3]
            in_path = base + ".in"
            out_path = base + ".out"
            if not os.path.exists(out_path):
                warn(f"缺少答案文件 {out_path}，跳过测试")
                skipped_count += 1
                continue
            cases.append((sy_file, in_path, out_path))

    if compare_runners:
        if skipped_count:
            warn(f"跳过 {skipped_count} 个缺少答案文件的用例")
        run_compare(
            cases,
            parse_runner_list(compare_runners),
            args.baseline_cc,
            args.baseline_clang,
            clang_extra,
        )
        sys.exit(0)

    pass_count = 0
    total_count = 0
    failed_tests = []
    total_elapsed = 0.0

    for sy_file, in_path, out_path in cases:
        total_count += 1

        success, reason, elapsed = run_test(
            sy_file,
            in_path,
            out_path,
            args.runner,
            args.baseline_cc,
            args.baseline_clang,
            clang_extra,
        )
        if success:
            pass_count += 1
            total_elapsed += elapsed
        else:
            failed_tests.append((os.path.basename(sy_file), reason))

    print("=" * 40)
    print(f"测试完成! 总计: {total_count}, 通过: {pass_count}, 失败: {total_count - pass_count}")
    print(f"runner: {args.runner}, total time: {total_elapsed:.3f}s")

    if failed_tests:
        print("\n❌ 未通过的测试用例及原因汇总:")
        for file, reason in failed_tests:
            print(f"  - {file:<25} : {reason}")
    elif total_count > 0:
        print("🎉 恭喜！所有测试全部完美通过！")
