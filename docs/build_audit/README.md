# docs/build_audit —— 16KB 门禁"放行"的审计主档

这个目录**必须存在**（本 README 就是它的占位文件）。原因很实际：

- `android/tools/record_unaligned_override.py` 在 `ALLOW_UNALIGNED=1` 放行时会**追加**
  写 `unaligned_overrides.jsonl`；
- 如果目录不存在，它会临场 `os.makedirs` —— 那一刻新建的是一个**未跟踪目录**，
  在 `git clean -df` / IDE 的 "Clean untracked files" 下会被直接删掉，
  而它里面装的正是"谁、在哪台机器、哪个 commit 上、为什么放行了 16KB 门禁"。
- 所以目录型产物要连同占位文件一起入库：**空目录 git 存不住，事故证据存不住 = 没有证据**。

## 与 `android/build_logs/unaligned_override.json` 的关系

| 位置 | 角色 | 是否入库 |
| --- | --- | --- |
| `docs/build_audit/unaligned_overrides.jsonl` | **审计主档**，一行一次放行，只追加 | 入库（本目录） |
| `android/build_logs/unaligned_override.json` | 就地面板：构建现场一眼可见"这次放行了什么" | 不入库（`.gitignore` 排除，清缓存即失） |

两份内容一致（同一份记录），但**只有 jsonl 是长期可查的**：`build_logs/` 会被清缓存、
被 `git clean -xdf` 带走，不能作为事故复盘依据。

## 记录格式（每行一个 JSON 对象，字段见 `record_unaligned_override.py`）

| 字段 | 含义 |
| --- | --- |
| `time` | 放行时刻，ISO8601 带时区（本地时间） |
| `operator` | `SUDO_USER`/`USER`/`LOGNAME` 取到的操作者，取不到写 `unknown` |
| `host` | 主机名（区分"哪台机器编的"） |
| `git_sha` | 仓库短 commit；不在 git 仓库里写 `nogit` |
| `abi` | 放行的 ABI，如 `arm64-v8a` |
| `apk` | 产物**文件名**（只留 basename：构建机路径各不相同） |
| `reason` | 放行原因，来自 `ALLOW_UNALIGNED_REASON`；未填写 `unspecified` |
| `unaligned` | 未对齐库名列表，形如 `lib/arm64-v8a/libcrypto.so` |
| `unaligned_detail` | 每个库的细节（`min_align`/不合规段数/机器类型），排查时不必重跑检查 |
| `unaligned_count` | 未对齐库数量（便于 grep/统计） |

## 纪律

1. **只追加，不改写、不删行**。写错了就再追加一条说明（审计的可信度来自"不能事后编辑"）。
2. 放行必须带 `reason`。不填不拦构建，但记录里只会是 `unspecified`，终端也会醒目提示。
3. 复盘时先按 `git_sha` + `abi` 定位，再看 `unaligned` 列表：
   ```bash
   # 最近 5 次放行
   tail -n 5 docs/build_audit/unaligned_overrides.jsonl | jq -r '[.time,.abi,.git_sha,.reason,(.unaligned|length)]|@tsv'
   # 某个 commit 上都放了什么
   jq -c 'select(.git_sha=="b11cdfd")' docs/build_audit/unaligned_overrides.jsonl
   # 有多少次是没写原因的
   grep -c '"reason": "unspecified"' docs/build_audit/unaligned_overrides.jsonl
   ```
4. 目标状态是**这个文件长期为空/不存在**：门禁不失败就不需要放行。
   文件里条数变多，说明 16KB 对齐在退化，应该去查 `check_elf_page_align.py` 而不是习惯性放行。

背景与完整门禁逻辑见 `android/build_android.sh` 的"16KB 页对齐门禁"段与
`android/tools/check_elf_page_align.py`。
