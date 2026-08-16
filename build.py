import os
import shutil
import subprocess
import sys
from pathlib import Path

# ================= 配置区域 =================
COMPILER = os.environ.get("CXX", "clang++")
TARGET_NAME = "compiler"
DEBUG_TARGET_NAME = "compiler_dbg"

BASE_FLAGS = ["-std=c++17", "-O2"]
DEBUG_FLAGS = ["-std=c++17", "-g", "-O0"]
WARN_FLAGS = []
LINK_FLAGS = ["-lm"]
# ===========================================


def include_flags(project_root: Path) -> list[str]:
    include_root = project_root / "src" / "include"
    dirs = [include_root]
    if include_root.exists():
        dirs.extend(sorted(p for p in include_root.rglob("*") if p.is_dir()))
    dirs.append(project_root / "src" / "lib")

    flags: list[str] = []
    for d in dirs:
        if d.exists():
            flags.extend(["-I", str(d)])

    extlibs = Path("/extlibs")
    if extlibs.exists():
        flags.extend(["-L/extlibs", "-I/extlibs", "-lantlr4-runtime", "-Wl,-rpath=/extlibs"])

    return flags

def clean():
    """清理所有编译产物"""
    project_root = Path(__file__).parent.absolute()

    for name in [TARGET_NAME, DEBUG_TARGET_NAME]:
        target_path = project_root / name
        if os.name == 'nt':
            target_path = target_path.with_suffix(".exe")

        if target_path.exists():
            target_path.unlink()
            print(f"🧹 已删除: {target_path.name}")

        dsym_dir = project_root / (name + ".dSYM")
        if dsym_dir.exists():
            shutil.rmtree(dsym_dir)
            print(f"🧹 已删除: {dsym_dir.name}")

    legacy_build_dir = project_root / "build"
    if legacy_build_dir.exists():
        shutil.rmtree(legacy_build_dir)
        print(f"🧹 已清理旧的 build 目录")

def build(debug: bool = False):
    project_root = Path(__file__).parent.absolute()
    target_name = DEBUG_TARGET_NAME if debug else TARGET_NAME
    target_path = project_root / target_name

    if os.name == 'nt':
        target_path = target_path.with_suffix(".exe")

    source_files = []
    src_dir = project_root / "src"
    for file_path in sorted(src_dir.rglob("*.cpp")):
        source_files.append(str(file_path))

    if not source_files:
        print("❌ 错误: src 目录下未找到任何 .cpp 文件！")
        sys.exit(1)

    print(f"📂 发现 {len(source_files)} 个源文件")

    cmd = (
        [COMPILER]
        + (DEBUG_FLAGS if debug else BASE_FLAGS)
        + WARN_FLAGS
        + include_flags(project_root)
        + source_files
        + LINK_FLAGS
        + ["-o", str(target_path)]
    )

    mode = "debug " if debug else ""
    print(f"🚀 正在编译 {mode}{target_path.name}...")
    print(" ".join(cmd))
    try:
        subprocess.run(cmd, check=True)
        
        dsym_dir = target_path.with_suffix(".dSYM")
        if dsym_dir.exists():
            shutil.rmtree(dsym_dir)
            
        print(f"✅ 编译成功！可执行文件: ./{target_path.name}")
        
    except subprocess.CalledProcessError:
        print("\n❌ 编译失败，请检查代码错误。")
        sys.exit(1)
    
    return target_path

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "clean":
        clean()
        sys.exit(0)

    debug = False
    args = sys.argv[1:]
    if "-d" in args:
        debug = True
        args.remove("-d")

    exe_path = build(debug)

    # 可选：支持直接运行测试文件
    if args:
        arg_file = args[0]
        if os.path.exists(arg_file):
            print(f"\n🚀 立即运行: ./{exe_path.name} {arg_file}")
            print("-" * 40)
            out_file = str(Path("out") / (Path(arg_file).stem + ".s"))
            Path("out").mkdir(exist_ok=True)
            subprocess.run([str(exe_path), "-S", "-o", out_file, arg_file])
            print(f"📄 汇编输出: {out_file}")
