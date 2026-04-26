import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

COMPILER_CMD = "./compiler"
SYLIB_C = "src/lib/sylib.c"
GCC_CMD = "riscv64-linux-gnu-gcc"
QEMU_CMD = "qemu-riscv64"
TIMEOUT = 60.0

GCC_FLAGS = ["-static"]
DEFAULT_OUT_DIR = "out"

TEST_DIR_ALIASES = {
    "f": "test/official_Functional/functional_recover/functional",
    "hf": "test/official_Functional/functional_recover/h_functional",
    "p": "test/official_Performance",
    "c": "test/custom",
}

IR_PASSES = [
    "frontend",
    "high-mem2reg",
    "high-licm",
    "flatten-cfg",
    "mem2reg",
    "scalar-cleanup",
    "inline",
    "post-spec-inline",
    "dfe",
    "strength-reduce",
    "loop-simplify",
    "loop-rotate",
    "lcssa",
    "loop-unroll",
    "deadloop-elim-pre-licm",
    "loop-strength-reduce",
    "licm-only",
    "licm",
    "deadloop-elim-post-licm",
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


def resolve_test_target(args) -> tuple[str | None, str | None]:
    scope_dir = resolve_dir_alias(args.dir)

    if args.file:
        scope, query = args.file
        test_dir = resolve_dir_alias(scope)
        sy_path = find_case_in_dir(test_dir, query)
        return sy_path, test_dir

    return None, scope_dir


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


def run_test(sy_path: str, in_path: str, out_path: str) -> tuple[bool, str]:
    print(f"Testing {os.path.basename(sy_path)} ... ", end="", flush=True)

    with tempfile.TemporaryDirectory() as tmpdir:
        asm_path = os.path.join(tmpdir, "output.s")
        exe_path = os.path.join(tmpdir, "a.out")

        try:
            with open(asm_path, "w", encoding="utf-8") as asm_file:
                subprocess.run(
                    [COMPILER_CMD, sy_path],
                    stdout=asm_file,
                    stderr=subprocess.DEVNULL,
                    check=True,
                    timeout=TIMEOUT,
                )
        except subprocess.TimeoutExpired:
            print("❌ 编译超时")
            return False, "编译超时"
        except subprocess.CalledProcessError:
            print("❌ 编译崩溃 (Compiler Error)")
            return False, "编译崩溃"

        ok, msg = build_exec(asm_path, exe_path)
        if not ok:
            print("❌ 汇编/链接失败")
            return False, msg

        input_data = None
        if os.path.exists(in_path):
            with open(in_path, "rb") as f:
                input_data = f.read()

        ok, program_result, elapsed = run_program(exe_path, input_data)
        if not ok:
            print(f"❌ {program_result}")
            return False, program_result

        with open(out_path, "r", encoding="utf-8") as f:
            expected_full = f.read().strip()

        actual_clean = normalize_output(program_result)
        expected_clean = normalize_output(expected_full)

        if actual_clean == expected_clean:
            print(f"✅ PASS ({elapsed:.3f}s)")
            return True, ""

        print("❌ FAIL (输出不一致)")
        print("--- 期望输出 ---")
        print(expected_clean)
        print("--- 您的输出 ---")
        print(actual_clean)
        return False, "输出不一致"


def iter_cases(test_dir: str):
    if not os.path.isdir(test_dir):
        die(f"找不到目录: {test_dir}")
    for root, _, files in os.walk(test_dir):
        for file in sorted(files):
            if file.endswith(".sy"):
                yield os.path.join(root, file)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "-d",
        "--dir",
        type=str,
        default="f",
        help="测试目录：f=functional, hf=h_functional, p=performance, c=custom",
    )
    parser.add_argument(
        "-f",
        "--file",
        nargs=2,
        metavar=("SCOPE", "CASE"),
        help="单测：`-f p 03_sort2` / `-f f 00_main` / `-f hf 00_comment2` / `-f c loop_case`",
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
    args = parser.parse_args()

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

    sy_path, scope_dir = resolve_test_target(args)

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
        run_test(sy_path, in_path, out_path)
        sys.exit(0)

    if dump_mode:
        for path in iter_cases(scope_dir):
            if passes:
                dump_ir(path, passes, args.out_dir)
            if args.dump_cfg_ir:
                dump_cfg_ir(path, args.out_dir)
        sys.exit(0)

    pass_count = 0
    total_count = 0
    failed_tests = []

    for sy_file in iter_cases(scope_dir):
        total_count += 1
        base = sy_file[:-3]
        in_path = base + ".in"
        out_path = base + ".out"

        if not os.path.exists(out_path):
            warn(f"缺少答案文件 {out_path}，跳过测试")
            total_count -= 1
            continue

        success, reason = run_test(sy_file, in_path, out_path)
        if success:
            pass_count += 1
        else:
            failed_tests.append((os.path.basename(sy_file), reason))

    print("=" * 40)
    print(f"测试完成! 总计: {total_count}, 通过: {pass_count}, 失败: {total_count - pass_count}")

    if failed_tests:
        print("\n❌ 未通过的测试用例及原因汇总:")
        for file, reason in failed_tests:
            print(f"  - {file:<25} : {reason}")
    elif total_count > 0:
        print("🎉 恭喜！所有测试全部完美通过！")
