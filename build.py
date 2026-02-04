import os
import subprocess
import sys
from pathlib import Path
import shutil

# ================= 配置区域 =================
COMPILER = "g++"
CFLAGS = ["-std=c++17", "-g", "-Wall", "-Wextra", "-Isrc/include"]
BUILD_DIR = "build"
TARGET_NAME = "compiler" 
# ===========================================

def clean():
    """清理构建目录以及根目录下的可执行文件"""
    # 1. 清理 build 目录
    if os.path.exists(BUILD_DIR):
        shutil.rmtree(BUILD_DIR)
        print(f"🧹 已清理目录: {BUILD_DIR}")
    
    # 2. 清理根目录下的 compiler 文件
    root_exe = Path(TARGET_NAME)
    if os.name == 'nt': # Windows兼容
        root_exe = root_exe.with_suffix(".exe")
        
    if root_exe.exists():
        root_exe.unlink()
        print(f"🧹 已清理根目录文件: {root_exe}")

def build():
    """编译项目并部署到根目录"""
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
    src_dir = project_root / "src"
    # 递归查找 src 目录下所有的 .cpp 文件
    for file_path in src_dir.rglob("*.cpp"):
        source_files.append(str(file_path))

    if not source_files:
        print("❌ 错误: src 目录下未找到任何 .cpp 文件！")
        sys.exit(1)

    print(f"📂 发现源文件: {[Path(p).name for p in source_files]}")

    # 2. 组装编译命令
    cmd = [COMPILER] + CFLAGS + source_files + ["-o", str(target_path)]

    print(f"🚀 正在编译 {TARGET_NAME}...")
    try:
        # check=True 会在编译失败时抛出异常
        subprocess.run(cmd, check=True)
        print(f"✅ 编译成功！")
    except subprocess.CalledProcessError:
        print("\n❌ 编译失败，请检查代码错误。")
        sys.exit(1)
    
    # 3. 复制到根目录
    root_exe = project_root / target_path.name
    try:
        shutil.copy2(target_path, root_exe)
        print(f"📦 已生成可执行文件: ./{root_exe.name}")
    except Exception as e:
        print(f"⚠️ 复制到根目录失败: {e}")
        return target_path
    
    return root_exe

if __name__ == "__main__":
    # 支持 python build.py clean
    if len(sys.argv) > 1 and sys.argv[1] == "clean":
        clean()
        sys.exit(0)
    
    # 执行编译
    exe_path = build()

    if len(sys.argv) > 1:
        arg_file = sys.argv[1]
        if os.path.exists(arg_file):
            print(f"\n🚀 立即运行: ./{exe_path.name} {arg_file}")
            print("-" * 40)
            subprocess.run([str(exe_path), arg_file])