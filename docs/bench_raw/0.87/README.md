# 0.87 基线 APK（原始产物）—— 本体不入库，只登记 sha256

## 存放位置

**仓库外**：`C:\Users\WQ159\jm-baseline\0.87\`（WSL 里是 `/mnt/c/Users/WQ159/jm-baseline/0.87/`）

```bash
ls -l /mnt/c/Users/WQ159/jm-baseline/0.87/
# JMComic-0.87-arm64-v8a-debug.apk
# JMComic-0.87-x86_64-debug.apk
```

为什么放在仓库外：`git clean -xdf` 会删掉**所有未跟踪**文件（带 `-x` 时连被忽略的一起删）。
放在仓库里无论加不加 `.gitignore` 都可能被一次清理抹掉，而 805MB 又不适合入库。
所以仓库内只保留这份 `README.md`（sha256 + 来源 + 复现命令），二进制放仓库外。

> `.gitignore` 里那条 `docs/bench_raw/**/*.apk` 是**多一层保护**（实测口径）：
> - 未忽略的未跟踪文件 → `git clean -df` **会删**；
> - 已忽略的文件 → `git clean -df` **不删**，只有带 `-x` 的 `git clean -xdf` 才删。
>
> 也就是说：**忽略只能挡 `-df`，挡不住 `-xdf`**。所以"重要的东西移出仓库"才是终解，
> 目录里这条 ignore 只是"万一有人把 APK 拷回来，不会被 `git add -A` 拖进仓库"的保险。

## 校验值

| 文件 | 字节 | sha256 |
| --- | --- | --- |
| `JMComic-0.87-arm64-v8a-debug.apk` | 453275738 | `fcd430625d879eacf4dcb3faaa7326fbf3fb0494be73d1581be36ffd848653c7` |
| `JMComic-0.87-x86_64-debug.apk` | 389031056 | `2406fdb629de7cc4b884390b1d9de49411a748c15b5cdac7c9686a29aa010150` |

核对：`sha256sum /mnt/c/Users/WQ159/jm-baseline/0.87/*.apk`

## 用途（16KB 事故的原始证据）

1. 静态门禁复现 —— x86_64 包"15 个库未对齐"：
   `python3 android/tools/check_elf_page_align.py <apk> --abi x86_64`
2. 设备侧复现 —— 在 16KB AVD（`PAGE_SIZE=16384`）上装包即崩：
   `bash android/tools/avd_verify.sh --apk <apk> --abi x86_64 --launch-wait 6`
   期望看到 `dlopen failed: ".../libc++_shared.so" program alignment (4096) cannot be
   smaller than system page size (16384)` → `QtLoader: UnsatisfiedLinkError` → 进程未存活。
3. 对照新包时先比 sha256，确认"验的确实是这一份" —— 别再出现"拿旧包当新包"的假成功
   （0.87 / 0.88 之间已经因此误判过两轮）。

## 纪律

- 这两个文件是**只读基线**：任何重新打包都必须另存新名字，不要覆盖。
- 不要 `git add` 任何 APK；需要入库请走 Git LFS 并先经 team-lead 同意。
