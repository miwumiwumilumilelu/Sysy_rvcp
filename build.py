import os
import subprocess
import sys
from pathlib import Path
import shutil

# ================= 配置区域 =================
COMPILER = "g++"
# C++17 是必须的，因为我们要用 std::string_view 和 alignof
CFLAGS = ["-std=c++17", "-g", "-Wall", "-Wextra", "-Isrc"]
BUILD_DIR = "build"
TARGET_NAME = "manc"
# ===========================================

def clean():
    """清理构建目录"""
    if os.path.exists(BUILD_DIR):
        shutil.rmtree(BUILD_DIR)
        print(f"🧹 已清理目录: {BUILD_DIR}")

def build():
    """编译项目"""
    project_root = Path(__file__).parent.absolute()
    build_path = project_root / BUILD_DIR
    target_path = build_path / TARGET_NAME

    # Windows 兼容
    if os.name == 'nt':
        target_path = target_path.with_suffix(".exe")

    # 创建构建目录
    build_path.mkdir(parents=True, exist_ok=True)

    # 1. 扫描源文件
    source_files = []
    # 递归查找 src 目录下所有的 .cpp 文件
    src_dir = project_root / "src"
    for file_path in src_dir.rglob("*.cpp"):
        source_files.append(str(file_path))

    if not source_files:
        print("❌ 错误: src 目录下未找到任何 .cpp 文件！")
        sys.exit(1)

    print(f"📂 发现源文件: {[Path(p).name for p in source_files]}")

    # 2. 组装编译命令
    # 核心指令: g++ -std=c++17 -Isrc src/main/main.cpp src/ir/Context.cpp -o build/manc
    cmd = [COMPILER] + CFLAGS + source_files + ["-o", str(target_path)]

    print(f"🚀 正在编译 Manc...")
    try:
        subprocess.run(cmd, check=True)
        print(f"✅ 编译成功！输出文件: {target_path}")
    except subprocess.CalledProcessError:
        print("\n❌ 编译失败，请检查代码错误。")
        sys.exit(1)
    
    return target_path

def run(target_path):
    """运行编译后的程序"""
    print(f"\n🧪 正在运行测试 (Main)...")
    print("=" * 40)
    try:
        # 直接运行生成的可执行文件
        subprocess.run([str(target_path)], check=True)
        print("=" * 40)
        print("🎉 运行结束。")
    except subprocess.CalledProcessError as e:
        print(f"❌ 运行时发生错误，返回码: {e.returncode}")

if __name__ == "__main__":
    # 如果带参数 clean，则执行清理
    if len(sys.argv) > 1 and sys.argv[1] == "clean":
        clean()
    else:
        exe_path = build()
        run(exe_path)