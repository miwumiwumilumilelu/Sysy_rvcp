import os
import subprocess
import argparse
import tempfile
import time

COMPILER_CMD = "./compiler"       
SYLIB_C = "src/lib/sylib.c"          
GCC_CMD = "riscv64-linux-gnu-gcc"  
QEMU_CMD = "qemu-riscv64"          
TIMEOUT = 100.0                     

GCC_FLAGS = ["-static"]

def run_test(sy_path, in_path, out_path):
    print(f"Testing {os.path.basename(sy_path)} ... ", end="", flush=True)

    with tempfile.TemporaryDirectory() as tmpdir:
        asm_path = os.path.join(tmpdir, "output.s")
        exe_path = os.path.join(tmpdir, "a.out")
        my_out_path = os.path.join(tmpdir, "my.out")

        try:
            with open(asm_path, "w") as asm_file:
                subprocess.run(
                    [COMPILER_CMD, sy_path],
                    stdout=asm_file,
                    stderr=subprocess.DEVNULL, 
                    check=True,
                    timeout=TIMEOUT
                )
        except subprocess.TimeoutExpired:
            print("❌ 编译超时")
            return False, "编译超时"
        except subprocess.CalledProcessError:
            print("❌ 编译崩溃 (Compiler Error)")
            return False, "编译崩溃"

        try:
            subprocess.run(
                [GCC_CMD] + GCC_FLAGS + [asm_path, SYLIB_C, "-o", exe_path],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=True
            )
        except subprocess.CalledProcessError as e:
            print("❌ 汇编/链接失败 (非法指令或语法错误)")
            return False, "汇编/链接失败"

        input_data = None
        if os.path.exists(in_path):
            with open(in_path, "rb") as f:
                input_data = f.read()

        try:
            start_time = time.time()
            result = subprocess.run(
                [QEMU_CMD, exe_path],
                input=input_data,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                timeout=TIMEOUT
            )
            elapsed = time.time() - start_time
        except subprocess.TimeoutExpired:
            print("❌ 运行死循环超时 (Timeout)")
            return False, "运行死循环超时"

        actual_output = result.stdout.decode('utf-8', errors='ignore').strip()
        actual_full = f"{actual_output}\n{result.returncode}".strip()

        with open(out_path, "r", encoding='utf-8') as f:
            expected_full = f.read().strip()

        actual_clean = '\n'.join([line.strip() for line in actual_full.split('\n') if line.strip()])
        expected_clean = '\n'.join([line.strip() for line in expected_full.split('\n') if line.strip()])

        if actual_clean == expected_clean:
            print(f"✅ PASS ({elapsed:.3f}s)")
            return True, ""
        else:
            print("❌ FAIL (输出不一致)")
            print("--- 期望输出 ---")
            print(expected_clean)
            print("--- 您的输出 ---")
            print(actual_clean)
            return False, "输出不一致"

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("-d", "--dir", type=str, default="test/official", help="测试用例目录")
    args = parser.parse_args()

    test_dir = args.dir
    if not os.path.exists(test_dir):
        print(f"找不到目录: {test_dir}")
        exit(1)

    pass_count = 0
    total_count = 0
    failed_tests = [] 

    for file in sorted(os.listdir(test_dir)):
        if file.endswith(".sy"):
            total_count += 1
            base_name = file[:-3]
            sy_path = os.path.join(test_dir, file)
            in_path = os.path.join(test_dir, f"{base_name}.in")
            out_path = os.path.join(test_dir, f"{base_name}.out")

            if not os.path.exists(out_path):
                print(f"⚠️ 警告: 缺少答案文件 {out_path}，跳过测试")
                total_count -= 1
                continue

            success, reason = run_test(sy_path, in_path, out_path)
            if success:
                pass_count += 1
            else:
                failed_tests.append((file, reason))

    print("=" * 40)
    print(f"测试完成! 总计: {total_count}, 通过: {pass_count}, 失败: {total_count - pass_count}")

    if failed_tests:
        print("\n❌ 未通过的测试用例及原因汇总:")
        for file, reason in failed_tests:
            print(f"  - {file:<25} : {reason}")
    elif total_count > 0:
        print("🎉 恭喜！所有测试全部完美通过！")