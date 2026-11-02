"""扫描 APK 内 libpybundle.so 中所有 ELF 的 PT_LOAD 对齐（不落地解包）。

为什么需要单独做：check_elf_page_align.py 只扫 lib/<abi>/，而 libpybundle.so 被
buildozer 当作普通文件拷入、被对齐门禁当作"非 ELF 数据包"跳过。但它在运行期
（android/start.c → ExtractBundle）会解包到 files/app/_python_bundle/，
其中的 *.cpython-314-*.so 与 site-packages/*.so 都会被 import → 同样受 16KB 约束。
已知实例：PySide6 6.12.0 官方 android wheel 的 shiboken6/Shiboken.abi3.so
4 个 PT_LOAD 全部 p_align=0x1000（上游缺陷，见 docs/implementation_task_list.md §0A 更新 15）。

用法（也是修订 22 定义的"bundle 门禁"，与 lib/ 门禁并列、缺一不算过）：
    python android/tools/check_pybundle_align.py <apk> [abi] [--member <apk内路径>]
                                                [--allow <路径片段> ...]
    默认 member 为 lib/<abi>/libpybundle.so；C1 把 bundle 放在 assets/libpybundle.so，
    需显式指定，否则工具会因成员不存在而报错（不是"通过"）。
    --allow 用于声明"已知且可接受"的例外（按路径片段匹配），命中者不进 fatal 计数，
    但仍会逐条打印——例外必须写在命令行里，不允许静默变绿。
退出码：0 = 全部对齐，或未对齐项全部被 --allow 声明；1 = 存在未声明的未对齐段；2 = 参数/文件错误。
"""
import struct
import sys
import tarfile
import zipfile
from collections import defaultdict

PT_LOAD = 1


def load_aligns(data: bytes):
    """返回 ELF64 的 PT_LOAD 段 p_align 列表；不是 ELF64 返回 None。"""
    if len(data) < 64 or data[:4] != b"\x7fELF":
        return None
    if data[4] != 2:  # 只处理 ELF64
        return None
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phentsize = struct.unpack_from("<H", data, 0x36)[0]
    phnum = struct.unpack_from("<H", data, 0x38)[0]
    out = []
    for i in range(phnum):
        base = phoff + i * phentsize
        if base + 56 > len(data):
            break
        p_type = struct.unpack_from("<I", data, base)[0]
        if p_type != PT_LOAD:
            continue
        p_align = struct.unpack_from("<Q", data, base + 48)[0]
        out.append(p_align)
    return out


def main(apk: str, abi: str, member: str = "", allow=()) -> int:
    member = member or f"lib/{abi}/libpybundle.so"
    try:
        fh = zipfile.ZipFile(apk)
    except (OSError, zipfile.BadZipFile) as e:
        print(f"[fail] 打不开 APK {apk}: {e}")
        return 2
    if member not in fh.namelist():
        print(f"[fail] APK 内没有 {member}；如果是 C1 的 assets/ 布局，请加 "
              f"--member assets/libpybundle.so")
        fh.close()
        return 2
    with fh, fh.open(member) as fobj:
        with tarfile.open(fileobj=fobj, mode="r|gz") as tf:
            total = 0
            bad = []
            for m in tf:
                if not m.isfile() or not m.name.endswith(".so"):
                    continue
                f = tf.extractfile(m)
                if f is None:
                    continue
                data = f.read()
                aligns = load_aligns(data)
                if aligns is None:
                    continue
                total += 1
                if any(a < 16384 for a in aligns):
                    bad.append((m.name, sorted(set(aligns))))

    print(f"bundle 内 ELF64 .so 总数: {total}   含 <16384 段: {len(bad)}")
    if len(bad) == 0:
        print("[OK] bundle 内所有 .so 均满足 16KB 对齐")
        return 0

    allowed = [(n, al) for n, al in bad
               if any(tok in n for tok in allow)]
    fatal = [(n, al) for n, al in bad if (n, al) not in allowed]

    if allowed:
        print(f"[NOTE] {len(allowed)} 项命中 --allow（已声明例外，不进 fatal）:")
        for name, al in allowed:
            print(f"  {name:<70} {[hex(a) for a in al]}")

    if not fatal:
        print("[OK] 除已声明例外外，bundle 内 .so 均满足 16KB 对齐")
        return 0

    print(f"[FAIL] {len(fatal)} 个 .so 的 PT_LOAD 段不满足 16384 对齐"
          f"（16KB 设备上会按兼容模式处理，fatal 时拒绝加载）")

    groups = defaultdict(int)
    for name, _ in fatal:
        parts = name.split("/")
        key = parts[2] if len(parts) > 2 else name
        groups[key] += 1
    if groups:
        print("\n按包/目录汇总未对齐数量:")
        for k, v in sorted(groups.items(), key=lambda kv: -kv[1]):
            print(f"  {k:<42} {v}")
        print("\n前 20 条明细:")
        for name, al in fatal[:20]:
            print(f"  {name:<70} {[hex(a) for a in al]}")
    return 1


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    argv = sys.argv[1:]
    member_arg = ""
    allow_arg = []
    if "--member" in argv:
        i = argv.index("--member")
        if i + 1 >= len(argv):
            print("[fail] --member 需要一个路径参数")
            sys.exit(2)
        member_arg = argv[i + 1]
        del argv[i:i + 2]
    while "--allow" in argv:
        i = argv.index("--allow")
        if i + 1 >= len(argv):
            print("[fail] --allow 需要一个路径片段参数")
            sys.exit(2)
        allow_arg.append(argv[i + 1])
        del argv[i:i + 2]
    if not argv:
        print(__doc__)
        sys.exit(2)
    apk_arg = argv[0]
    abi_arg = argv[1] if len(argv) > 1 else "x86_64"
    sys.exit(main(apk_arg, abi_arg, member_arg, allow_arg))
