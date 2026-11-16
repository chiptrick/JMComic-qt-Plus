# C++ 桌面端 / 移动端 Vulkan 超分后端（cpp-ui）

> 状态（2026-11-14，含 arm64 真机验证）：**桌面与移动端均已打通并验收**。  
> 39 个 fp32 模型在同一 tile 上与 ONNX Runtime（CPU）逐像素对比全部通过；  
> arm64 真机（V2463A / Adreno 830）出包、安装、端到端计算自检、46 个模型扫描、  
> 吞吐基准、量化模型与设备丢失回退全部实测。  
> 本文说明整体架构、与既有后端的接口集成、算法与参数、桌面/移动差异、回退与兼容性。

## 0. 决策与边界

用户确认的四项决策：原生 Vulkan compute（不引 ncnn / 不用 QRhi）、覆盖全部 39 个非量化模型、  
优先级 QNN → Vulkan → CPU、移动端先保证编译与失败回退。真机接入后本轮把"只做编译与回退"  
推进到**真机出包 + 全链路验证**，并补齐了量化模型、算子融合、设备丢失重建、tile 参数化。

| 项       | 结论                                                                                                              |
| ------- | --------------------------------------------------------------------------------------------------------------- |
| 依赖      | 系统 `vulkan-1.dll` / `libvulkan.so`；Khronos 头随仓（桌面），NDK sysroot 头（Android）                                       |
| 模型      | 直接读现有 `sr_models/*.onnx`，不生成第二份权重、不做格式转换                                                                        |
| 量化模型    | QDQ（`*_q16` / `lowq`）**目录里存在即动态加入可选列表**；Vulkan 明确拒绝，ORT CPU 实测 39/39 可执行（桌面）、真机 7/7 可执行                         |
| 固定 tile | ONNX 导出时把输入空间维写死为 192，ORT 只接受 192；Vulkan 执行器按输入动态推断，可接受 32~512 的自定义 tile                                        |
| 桌面验证    | `--vk-compare ""` 39/39 PASS；`--vk-model-test ""` 39/39 无 NaN；单算子自检通过；离线回归 75/75                                |
| 真机验证    | Adreno 830 / Vulkan 1.3.284：probe/conv/pad 自检通过；tile 128 扫描 46 模型 `ok=18 fail=0 skipped=28`，所有 SKIP 的 CPU 回退均成功 |

## 1. 整体架构

```
SrEngine（既有）  tile 切分 / 反射补边与收缩边 / 结果拼装 / 进度 / 取消 / 后端选择与降级记录
  ├── ORT CPU / QNN HTP（既有）
  └── SrVk::Session（新增）
        ├── vk_onnx     最小 protobuf 读取：节点 / 属性 / 初始化器 / 图输入输出
        ├── sr_graph    常量折叠 -> 形状推断 -> conv+act 融合 -> 活跃期显存规划 -> 逐算子 dispatch
        ├── shaders/*.comp  11 个 GLSL 450 计算内核（构建期 qsb -> SPIR-V -> qrc）
        ├── vk_device   实例/设备/队列、arena、staging、PSO 缓存、DEVICE_LOST 重建与熔断
        └── vk_api      loader 动态加载 + 两阶段入口解析 + 设备枚举/能力
```

**分层契约**：`SrEngine` 负责"一张图 → 若干 tile → 拼回一张图"，Vulkan 只负责  
`RunTile(fp32 tile 进, fp32 出, &outW, &outH)`，与既有 ORT `RunTile` 完全同签名。  
所以阅读器自动超分、图片超分页、批量超分、下载链路**一行都不用改**；  
数值验收也能直接在同一个 tile 上做 CPU/GPU diff。

### 本轮新增的实现要点

- **算子融合**：`Conv → Relu / LeakyRelu / Sigmoid` 在 `prepare()` 里合成一步，  
  conv 着色器用 `P(13)` 模式位与 `P(14)` alpha 位在写回前应用激活。  
  条件是该 conv 输出仅被这一个 act 消费；PRelu（需 slope 绑定）与 Clip（参数槽不够）不融合。  
  以 waifu2x cunet 2x 为例，75 个节点压成 50 个执行步，少一次 dispatch、一次全图 barrier  
  和一整张中间张量的显存往返；融合后 39/39 数值对比仍全部 PASS。
- **DEVICE_LOST 生命周期**：`Check()` 捕获 `VK_ERROR_DEVICE_LOST` → `Session::run` 丢弃旧  
  arena 并 `Device::Recover()` 重建实例/设备/权重上传，重跑当前 tile；第二次丢失即  
  **进程内熔断**（`broken()`），`Available()` 直接返回 false，避免反复重置驱动。
- **运行期 CPU 回退**：`SrEngine::RecoverToCpu()` 临时锁偏好为 cpu、重建 ORT 会话；  
  若 tile 因回退从自定义值变回 192，`UpscaleEx` 带 guard 整图重跑一次。  
  |  |  |  
  | --- | --- |
- **量化模型动态目录**：`Catalog()` 扫描 `sr_models/*_q16*|*lowq*.onnx` 追加条目  
  （arch=`quant`，分组显示"量化模型（QNN / CPU）"），不污染默认 39 个 fp32 项。
- **APK 重打包工具** `cpp/tools/apk_repack.py`：见 §4 的对齐/签名说明。

### 关键实现约束（全部来自实测踩坑）

1. **入口解析两阶段**：`vkGetInstanceProcAddr(nullptr, ...)` 在 Windows loader 上只给出  
   `vkCreateInstance` / `vkEnumerateInstanceVersion`；实例级函数必须在建实例后解析；  
   `vkGetDeviceQueue` 又必须在设备级入口解析之后调用，提前调用即空指针崩溃。
2. **QLibrary 生命周期**：必须静态 + `PreventUnloadHint`，否则析构 `dlclose` 让函数指针悬空。
3. **fence 统一规则**：每次 `QueueSubmit` *之前* `ResetFences`，之后只 `WaitForFences`；  
   `download()` 若在结束时把 fence 复位，下一次 `beginPass` 的 `WaitForFences` 会永久阻塞。
4. **push constant 布局**：`int p[16]` 在 std140 数组下步长与 C++ 紧排不一致的坑用  
   `ivec4 q[4]` + `#define P(i) q[i>>2][i&3]` 彻底固定；`pcdump.comp` 可逐槽验证。
5. **staging 环形分配**：一次 pass 内多次上传都从 offset 0 拷贝会互相覆盖；按 256 字节对齐 bump。
6. **conv 的列坐标边界**：必须先对"列坐标"做 `0 <= ixCol < W` 判断，再拼 `rowBase + ixCol`。
7. **reflect pad 不用负数取模**：GLSL 的 `%` 对负数实现定义，实测 `reflect(-1)` 被算成 3。
8. **常量上传按引用判定**：不能按"元素数 > 8"跳过（SE block 的 bias 恰好 8 个 float），  
   只上传确实被 GPU 算子绑定的常量。
9. **Concat 多段共用一个输出张量**：分配器必须用 `allocated` 去重（x4plus 整片 NaN 的根因）。
10. **描述符池**：x4plus 单 pass 1714 次 dispatch → 池给 8192 set，失败退 2048。
11. **APK 对齐必须预补偿**：`apksigner` 会给每个非 lib 条目插 4 字节对齐 padding，  
    若不预先补齐，签名后所有 `lib/**/*.so` 的 4096 对齐偏移整体 +20，安装直接  
    `INSTALL_FAILED_INVALID_APK`。同时必须移除旧 v1 签名文件、禁用 v1 签名。
12. **Android assets 路径**：`assets:/sr_models/*.onnx` 是 Qt 伪路径，ORT 打不开；  
    与 `EnsureModel` 一样必须读进内存后 `CreateSessionFromArray`（`DebugOrtTile` 已对齐）。

## 2. 与现有后端的接口集成

### 2.1 后端优先级与偏好

`SrEngine::BackendPref()` 由 `JM_SR_BACKEND` 或 `ui.ini [sr] backend` 决定，取值

|        |     |        |                                                |
| ------ | --- | ------ | ---------------------------------------------- |
| \`auto | qnn | vulkan | cpu`（`vk`/`gpu` 别名）。`EnsureModel()\` 内的顺序保持不变： |

```
pref=vulkan           -> 先试 Vulkan，失败带原因走 ORT
pref=auto & QNN 可用   -> QNN(tier 3→0)，任一级失败再试 Vulkan，最后 CPU
pref=auto & QNN 不可用 -> 直接试 Vulkan，最后 CPU
pref=qnn / cpu        -> 不碰 Vulkan
```

任一失败都写 `backendNote`。设置页是四选一（自动 / NPU / GPU / CPU）。  
真机 `auto` 下若 Vulkan 熔断，`Available()` 返回 false，后续会话直接落到 CPU，  
不会反复触发 GPU 重置。

### 2.2 会话与 tile

`SrVk::Session` 独占一份 arena：`load(path)` → `ensureTile(tile)` → `run(...)`。  
`JM_SR_TILE` / `ui.ini [sr] tile`（32~512，按 8 对齐）**只作用于 Vulkan 会话**；  
ORT CPU / QNN 保持模型固定的 192。Android 上未显式配置时 Vulkan 默认 128。  
运行期回退 CPU 时 `s.tile` 变回 192，`UpscaleEx` 会整图重跑，输入缓冲与拼装参数一致。

### 2.3 显存模型

- **单个** device-local `VkBuffer` 作 arena，所有权重与激活都是它的子区间。
- 权重区按模型实际权重动态设定（`setWeightReserve`，16MB 对齐）：  
  x4plus 权重 63MB、激活 792MB，固定对半分会失败。
- 激活区按活跃期复用（first-fit 空闲块）；host-visible staging 默认 64MB。
- 预算：桌面 `min(vram/4, 1024MB)`；Android `min(vram/8, 384MB)`，`JM_VK_VRAM_MB` 可覆盖。  
  Adreno 830 把整机的 11121MB 共享 RAM 上报为 DEVICE_LOCAL，按 1/4 会算出 1GB arena。

## 3. 支持的超分算法与参数

39 个 fp32 模型 = waifu2x cunet 10 + Real-CUGAN 18 + Real-ESRGAN 7 + SRMD/SRMD-NF 6。  
算子覆盖：`Conv / ConvTranspose / Relu / LeakyRelu / PRelu / Sigmoid / Clip / Add / Mul / Sub /
Div / Concat / Pad / DepthToSpace / Resize / GlobalAveragePool / ReduceMean / Constant /
ConstantOfShape / Equal / Where / Expand`，形状类算子在 CPU 常量折叠。

| 家族                         | 结构要点                                                       | 关键参数                                                                   |
| -------------------------- | ---------------------------------------------------------- | ---------------------------------------------------------------------- |
| waifu2x cunet              | 全 valid 卷积 + SE + 转置卷积上采样 + 负 Pad 裁剪 + 残差                  | k=2/3，pads 0，stride 1/2；ConvTranspose k2/s2、k4/s2/pad3；LeakyRelu α=0.1 |
| Real-CUGAN SE/Pro          | 18 像素 reflect 预补边、SE 门控、ConvTranspose、`Sub/Div` 归一化        | reflect pad=18；ReduceMean(H,W)；Resize 最近邻 2×；DepthToSpace CRD          |
| Real-ESRGAN animevideo     | 34 组 Conv+PRelu → DepthToSpace(4,CRD) → 4× 最近邻残差 → 线性 0.5× | PRelu 每通道 slope；nearest(asymmetric) 与 linear(half_pixel)               |
| Real-ESRGAN x4plus/general | RRDB：23 残差块、多路 Concat（1024 节点 / 1714 dispatch）             | Concat(axis=1) 2~5 路；最大激活 792MB                                        |
| SRMD / SRMD-NF             | 输入 ⊕ 15 通道退化图 ⊕ 1 通道噪声图（19 通道）→ 12 层 Conv → DepthToSpace   | `Expand` 按广播结果定形；blocksize 2/3/4                                       |
| QDQ 量化（新增）                 | 权重 uint16 + scale/zero_point，214 个 Q/DQ 节点（152 个作用于激活）     | Vulkan 明确拒绝；ORT CPU 可执行（桌面 39/39、真机 7/7）                               |

### 计算内核（`vk/shaders/*.comp`）

| shader                                                   | 参数（push constant 槽）                                                        | 说明                                                     |
| -------------------------------------------------------- | -------------------------------------------------------------------------- | ------------------------------------------------------ |
| `conv`                                                   | C,H,W,OC,KH,KW,SX,SY,PX,PY,OH,OW,hasBias + **P(13) actMode / P(14) alpha** | 64 线程 × 每线程 4 像素；融合激活在写回前完成                            |
| `deconv`                                                 | 同上，权重 `[IC,OC,KH,KW]`                                                      | gather 形式                                              |
| `act`                                                    | n,mode,C,HW,α,min,max                                                      | 未被融合的 relu/leaky/sigmoid/prelu/clip                    |
| `eltwise` / `copy` / `d2s` / `pad` / `reduce` / `resize` | 见源码 `PC` 布局                                                                | 广播、Concat 分段、CRD、reflect/constant、SE 归约、nearest/linear |
| `probe` / `pcdump`                                       | 端到端链路 / push constant 逐槽诊断                                                 | 回归自检                                                   |

所有 kernel 没有针对某个模型写死的分支，参数全部来自图属性/初始化器。

## 4. 桌面端与移动端的差异化处理

| 维度       | Windows 桌面                            | Android 真机（V2463A / Adreno 830）                         |
| -------- | ------------------------------------- | ------------------------------------------------------- |
| Vulkan 头 | 随仓 `3rdparty/vulkan/include`          | NDK 30 sysroot，`VK_USE_PLATFORM_ANDROID_KHR`            |
| loader   | `vulkan-1.dll`                        | `libvulkan.so`，同一套动态解析                                  |
| 着色器      | 构建期 `qsb --glsl 450` → `.qsb` → qrc   | 完全相同                                                    |
| 设备选择     | 独显优先，实测 RTX 4060 Laptop（1.4.357）      | 单 GPU（1.3.284，`igpu`，`android.hardware.vulkan.level=1`） |
| 显存预算     | `min(vram/4, 1024MB)`，实测 arena 1024MB | `min(vram/8, 384MB)`，实测 arena **384MB**                 |
| 默认 tile  | 192（Vulkan 与 CPU 一致）                  | Vulkan **128**（激活约降到 44%），CPU 仍 192                     |
| staging  | 64MB                                  | `min(64MB, budget/4)`                                   |
| 模型打包     | 本地 `sr_models/`                       | APK `assets/sr_models/`（46 个 onnx：39 fp32 + 7 QDQ）      |
| 大模型策略    | x4plus 792MB 激活完整放入                   | 预算不足明确报"激活区不足"并回退 CPU；连续大 dispatch 触发 GPU watchdog 时熔断  |

**吞吐实测（单 tile，纯推理）**

| 平台 / 后端                      | waifu2x cunet 2x                     | Real-CUGAN SE 2x | Real-CUGAN SE 4x |
| ---------------------------- | ------------------------------------ | ---------------- | ---------------- |
| 桌面 RTX 4060 / Vulkan tile192 | 164ms（median）                        | —                | —                |
| 桌面 ORT CPU tile192           | 270ms                                | —                | —                |
| Adreno 830 / Vulkan tile192  | 2113ms                               | 2502ms           | 7191ms           |
| Adreno 830 / Vulkan tile128  | 1158ms（bench median；全量扫描 994~1166ms） | 1480ms           | 2543ms           |
| Adreno 830 / ORT CPU tile192 | 483ms（bench；192 报告 533ms）            | —                | —                |

最终版真机基准输出：`vk-bench ... tile=128 n=3 median=1157.69ms`，  
`ort-cpu ok=1 ms=483.21`，`speedup=0.42x (ort/vk median)`，即 CPU 约快 2.4 倍。  
结论必须如实说明：**移动 GPU 上这套朴素直接卷积比 ORT CPU 慢约 2~4 倍**，  
且连续跑 4x / 大模型会触发 `VK_ERROR_DEVICE_LOST`（真机在  
`realcugan_se_up4x_conservative` 起）。缩小 tile 到 128 能把单步延迟降到约一半、  
让 3x 模型落进 384MB 预算，但不能消除驱动 watchdog。  
因此移动端若追求稳定与速度，建议设置页显式选 CPU；Vulkan 作为可选路径保留，  
失败/熔断后由 CPU 兜底。是否把移动端 `auto` 默认改成 CPU 优先属于产品决策，本轮未改。

### APK 出包（新增可复现流程）

`cpp/tools/apk_repack.py` 用 Python 直接重写 zip：替换主 `.so`、所有 `lib/**/*.so`  
改为 4096 对齐 STORED、其它条目预补 4 字节对齐 padding、移除旧 v1 签名文件；  
再用 `apksigner.jar --v1-signing-enabled false` 签 v2/v3，最后 `--verify-only` 校验对齐。  
`am start ... --es jm.args '--vk-report ...'` 可一键触发真机报告（注意嵌套引号）。

## 5. 异常回退与兼容性策略

分层回退，每层都留"人话原因"（`BackendDetail()` / 日志 / 设置页 tooltip）：

1. loader 缺失 / 入口解析失败 → `Vulkan 不可用：缺少 vulkan-1.dll …`。
2. 无物理设备、无计算队列、arena/描述符池创建失败 → 报具体 `VkResult` 与原因。
3. 模型含未实现算子（QDQ、DCR DepthToSpace、非 half_pixel 的 linear Resize 等）  
   → `prepare()` 明确失败，**不会跑出错图**。
4. 显存不足（大激活 / 移动预算小） → `激活区分配失败（需 X MB）` → 回退 CPU。
5. 运行期 `dispatch` / `download` 失败 → `Session::run` 重建设备并重试一次。
6. 二次 `DEVICE_LOST` → 进程内熔断，`Available()` 返回 false，后续全部走 CPU。
7. `pref=vulkan` 失败仍走 ORT 但保留原因；`pref=auto` 失败只在 note 里说明。
8. **CPU 永远可用**：`JM_SR_BACKEND=cpu` 一键关闭 Vulkan；QDQ 模型、设备丢失、  
   显存不足三条路径在真机上均实测 `ort-cpu=1 ok`。
9. 兼容性边界：只支持 fp32 NCHW、正方形 tile、group=1 的 Conv/ConvTranspose；  
   QDQ 由 ORT CPU 执行，不做 fp32 模拟量化。

## 6. 验收入口与结果

| 命令                                             | 作用                                      | 结果                                                     |
| ---------------------------------------------- | --------------------------------------- | ------------------------------------------------------ |
| 桌面 `--vk-info` / `--vk-probe`                  | 设备摘要 / 端到端链路                            | `maxErr=0`                                             |
| 桌面 `--vk-conv-test` / `--vk-pad-test`          | 单算子 vs CPU                              | `maxErr=1.8e-07` / `bad=0`（真机同样通过）                     |
| 桌面 `--vk-compare ""`                           | 39 模型 Vulkan vs ORT 逐像素                 | **39/39 PASS**（融合后复跑仍 39/39）                           |
| 桌面 `--vk-model-test ""`                        | 39 fp32 + 7 QDQ 扫描                      | `tested=39 ok=39 fail=0 skipped=7`                     |
| 桌面 `--vk-quant-test <dir>`                     | QDQ：ORT CPU 能否跑 + Vulkan 是否明确拒绝         | 39 个 `ort=1` / `vulkan=rejected`                       |
| 桌面 `--vk-bench <模型> [--vk-iter N --vk-tile N]` | Vulkan 吞吐 + ORT CPU 对照                  | Vulkan 164ms vs CPU 270ms（1.65×）                       |
| 桌面 `--vk-compare --vk-tile 128`                | 自定义 tile                                | ORT 明确拒绝（模型固定 192），Vulkan 侧可跑                          |
| 桌面离线回归                                         | `c1_ui_desktop_suite.py --only offline` | **75/75**（并发跑时出现过一次动画用例 flaky，单独重跑 75/75）              |
| 真机 `--vk-report --vk-tile 192 --vk-bench 3`    | 全量 46 模型 + 探针 + 吞吐                      | `ok=15`，7 个 QDQ CPU 回退成功；4x 起触发 DEVICE_LOST            |
| 真机 `--vk-report --vk-tile 128 --vk-bench 3`    | 全量 46 模型（熔断版）                           | `ok=18 fail=0 skipped=28`，**28 个 SKIP 的 ORT CPU 全部成功** |
| Android x86_64 / arm64-v8a                     | 交叉编译                                    | 均产出 `.so`；arm64 APK 安装成功、真机报告可复现                       |


诊断开关：`JM_VK_DUMP` + `JM_VK_VALUES` / `JM_VK_DUMP_MAX`、`JM_VK_NAN_SCAN`、  
`JM_VK_NAN_CHECKPOINT` + `JM_VK_CP_FROM/TO`、`JM_VK_COMPARE_HEAD`，默认全关。

## 7. 剩余边界（明确列出，不掩盖）

- **移动 GPU watchdog**：Adreno 830 连续跑 4x / 大模型会 `VK_ERROR_DEVICE_LOST`；  
  本轮已实现重建一次 + 二次熔断 + CPU 兜底，但没有消除驱动层重置本身。
- **移动端吞吐**：滑窗/融合优化后 waifu2x 已与 ORT CPU 持平（427 vs 425ms）、  
  Real-CUGAN 仍慢 ~26%（845 vs 672ms）；fp16 与 Winograd 落地前不主张移动端默认 GPU。
- **是否调整移动端 `auto` 优先级**：用户原决策是 QNN → Vulkan → CPU，本轮未改；  
  真机数据仍建议移动端默认 CPU 优先，需另行确认。
- QNN / HTP 实推理仍需 arm64 真机 + 带 QNN EP 的 ORT + `_q16` 量化模型链路。
- 算子融合只覆盖 `Conv → Relu/LeakyRelu/Sigmoid`；PRelu、Clip 与跨层乘加未融合。
- 量化模型走 ORT CPU（QDQ 语义正确），Vulkan 不做 fp32 反量化模拟。
- tile 固定 192 是 39 个 ONNX 文件本身的约束，不是执行器限制；自定义 tile 仅 Vulkan 可用。

## 8. 基于 NCNN（RealSR-NCNN-Android）用法的 GPU 优化

参考对象 `tumuyan/RealSR-NCNN-Android`（其推理代码源自 nihui 的  
realsr/realcugan/waifu2x-ncnn-vulkan）。先对照它的 NCNN Vulkan 后端用法，  
再对照本项目定位瓶颈、实施改动。

### 8.1 NCNN 侧用法对照（从该仓库源码逐行核对）

| 维度        | NCNN（RealSR-NCNN-Android）                                                                                                       | 本项目（优化前）                                                                 |
| --------- | ------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------ |
| vkdev 初始化 | 进程级 `ncnn::create_gpu_instance()` → `get_gpu_device(gpuid)` → `net.set_vulkan_device(vkdev)`；结束时 `destroy_gpu_instance()`       | 单例 `vk::Device::Ensure()`（等价），并多出 DEVICE_LOST 重建/熔断（NCNN 没有）             |
| 算子选项      | `use_fp16_packed/storage/arithmetic=true`、`use_int8_storage=true`、`use_winograd_convolution=true`、`use_sgemm_convolution=true`  | 全 fp32，直接卷积（无 Winograd/SGEMM）                                            |
| 算子调度      | param 加载时按层建 VkPipeline（每层固定）；一层一次 dispatch                                                                                     | 每 kernel 首次 dispatch 编译并缓存（等价）；每 dispatch 都 CmdBindPipeline + 全局 barrier |
| 显存管理      | `vkdev->acquire_blob_allocator()`（blob 池，跨层复用）+ `acquire_staging_allocator()`，`process()` 结束 `reclaim_*`；权重走独立 weight allocator | 单一 arena（权重区 + 活跃期 bump 回收）+ 64MB 环形 staging（等价，权重常驻更省上传）                |
| 输入/输出     | `cmd.record_clone()` 上传/下载，staging 内部管理                                                                                         | 环形 staging 上传 + 输出区间回读（等价）                                               |
| 命令缓冲      | **每个 y-tile 行一个 `ncnn::VkCompute`，跨 preproc→extract→alpha→postproc 复用**，只在必要时 submit_and_wait                                   | 每 tile 一次 beginPass/endPass（同步 submit+wait），层内无多余提交（等价）                  |
| tile 选择   | 按 `vkdev->get_heap_budget()` 分档，**Adreno 特判降档**（>2800MB 时 160 而非 200）                                                           | 按 384MB 预算 + `JM_SR_TILE`，Adreno 默认 128（同思路）                             |

NCNN 在 Adreno 上快的两大支柱是 **fp16（存储+计算）** 与 **Winograd/SGEMM 卷积**；  
本项目优化前两者都没有，是性能差距的主因。

### 8.2 瓶颈定位（结合 39 模型算子统计）

对 39 个 fp32 模型的算子统计：Conv 1246 个（k3×1038、k1×208、k2×78），其中  
stride1 占 1246、pad0 占 683、pad1 占 641；ConvTranspose 100 个（78 个后接 LeakyRelu）。  
逐层 FLOPs 与实测对比：waifu2x cunet 2x 单 tile ≈ 430 GFLOP，Adreno 830 fp32 理论 ≈ 2 TFLOPS，  
纯算下限 ≈ 215ms，实测 1158ms → **GPU 利用率 ~19%**。浪费来自：

1. **直接卷积的读放大与分支**：每输出像素 K*K=9 次输入读（4 宽线程共享后 4.5 次），  
   且内层每像素 4 个边界判断分支；Adreno 的算术吞吐被访存与控制流拖住。
2. **ConvTranspose+LeakyRelu 未融合**：78 处，每处多一次 dispatch、一次 barrier、  
   一整张上采样后大张量（tile192 时 ≈ 6MB）的写+读往返。
3. **每 dispatch 重复 CmdBindPipeline + 全局 memory barrier**：x4plus 单 tile 1714 次  
   dispatch，全局 barrier 让驱动每次做全 arena 缓存维护。
4. （NCNN 的另两根支柱未做）fp16 与 Winograd，见 §8.5 路线图。

### 8.3 本轮关键修改点

| 修改         | 文件                                         | 内容                                                                                                                                                                                                   |
| ---------- | ------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| conv 滑窗特化  | `vk/shaders/conv.comp`                     | `SX==1&&SY==1&&KW∈{1,2,3}` 时按 KW 完全展开：先把该行 `K+kBlock-1` 个输入（带边界 guard）读进寄存器，再做无分支的乘加（K=3 时输入读 12→6 次）。**第一版用运行期下标 `xs[kx+t]`，NVIDIA/Adreno 都会把数组降级到 local memory，实测 164→269ms 退化；改为按 KW 常量展开后恢复并反超** |
| deconv 融合  | `vk/shaders/deconv.comp`、`vk/sr_graph.cpp` | deconv 增加与 conv 相同的 `P(13)/P(14)` 激活槽位（GLSL 不允许嵌套函数，`FusedAct` 提到 main 外）；融合规则从"仅 conv"扩展到 deconv，waifu2x/Real-CUGAN 上采样段 78 处生效                                                                     |
| 绑定去重       | `vk/vk_device.cpp/.h`                      | `boundPipe_` 记录当前管线，连续同名 kernel 跳过 `CmdBindPipeline`；`beginPass` 重置（规范要求重录后绑定状态未定义）                                                                                                                  |
| 精确 barrier | `vk/vk_device.cpp`                         | dispatch 后的全局 memory barrier 改为 **buffer barrier**，范围钉在本 dispatch 的输出区间（输出约定为最后一个绑定），避免每层全 arena 缓存维护                                                                                                |

### 8.4 优化前后实测

| 平台 / 模型 / tile                          | 优化前             | 优化后                          | 提升                                    |
| --------------------------------------- | --------------- | ---------------------------- | ------------------------------------- |
| 桌面 RTX 4060 / waifu2x 2x / t192         | 164ms           | **97.7ms（median）**           | 1.7×（vs ORT CPU 287ms → **2.9×**）     |
| 桌面 RTX 4060 / Real-CUGAN SE 2x / t192   | —（未单测）          | 124ms                        | vs ORT CPU 352ms → **2.8×**           |
| 桌面 `--vk-model-test` 全量                 | 21888ms         | **9094ms**                   | 2.4×                                  |
| 真机 Adreno 830 / waifu2x 2x / t128       | 1158ms          | **427ms**                    | **2.7×**（vs ORT CPU 425ms → 持平）       |
| 真机 Adreno 830 / waifu2x 2x / t192       | 2113ms          | **934ms**                    | 2.3×                                  |
| 真机 Adreno 830 / Real-CUGAN SE 2x / t128 | 1480ms          | **845ms**                    | 1.8×（vs ORT CPU 672ms → 0.8×，CPU 仍略快） |
| 真机 tile128 全量扫描                         | ok=18 / skip=28 | **ok=29 / fail=0 / skip=17** | Real-CUGAN Pro 全系与 SE 4x 恢复可用         |

真机 tile128 全量报告（优化后）：waifu2x 系 475ms/tile（优化前 ~1000ms）、  
Real-CUGAN SE 2x 750ms（优化前 1480ms）、SE 4x 1317ms（优化前 DEVICE_LOST）；  
17 个 SKIP（10 个 realesrgan-x4plus/SRMD 系 DEVICE_LOST + 7 个 QDQ）的 ORT CPU  
回退全部成功。数值：`--vk-compare ""` 39/39 PASS、`--vk-model-test ""` 39/39 ok、  
离线回归 75/75（优化前后均一致）。

### 8.5 第二轮优化落地（fp16 / Winograd / descriptor / 流水，本轮已全部实现）

四项路线图全部落地，逐项实现方式与实测：

**1) fp16 存储与计算（NCNN fp16_packing 模式）**

* 能力探测：`VkPhysicalDeviceVulkan12Features.shaderFloat16` +
  `VkPhysicalDeviceVulkan11Features.storageBuffer16BitAccess`（注意 16bit storage
  归 **11** features 结构；桌面随仓 Khronos 头是草案命名 `VULKAN_1_2_FEATURES`，
  NDK 新头是 `VULKAN_12_FEATURES`，数值同值、宏名不同，用 `#ifdef` 兼容）。
  只认 Vulkan 1.2+ 设备（桌面 NVIDIA/AMD/Intel 与 Adreno 6xx+ 全覆盖）。
* shader 参数化：9 个 kernel（conv/deconv/act/eltwise/copy/d2s/pad/reduce/resize
  + convw）用 `FT/FLD/FST` 宏抽象存储类型；`qsb -D USE_FP16=1` 生成 `_fp16.qsb`
  变体，运行期按能力选择。存储 half、shader 内 unpack 成 fp32 累加——精度安全。
* 显存/带宽减半：所有张量 2 字节/元素（`elemBytes()`），arena 需求减半，
  x4plus tile128 act 从 296MB → 148MB。
* CPU 侧 half 转换：`vk_half.h`（位操作 RNE，denormal 按文献公式规格化）。
* 开关：`JM_VK_FP16=0` 退回 fp32（能力不足自动退）。

**2) Winograd F(2,3) 卷积**

* `convw.comp`：每线程一个 2x2 输出 tile；输入变换 `T = B^T·D·B^T`（行/列方向
  同一组稀疏系数：`t0=a0-a2, t1=a1+a2, t2=a2-a1, t3=a1-a3`）、输出
  `y = A^T·M·A`（列组合 `v0=M[:,0]+M[:,1]+M[:,2]`、`v1=M[:,1]-M[:,2]-M[:,3]`，
  行组合同构）。常数：`B^T=[[1,0,-1,0],[0,1,1,0],[0,-1,1,0],[0,1,0,-1]]`、
  `G=[[1,0,0],[.5,.5,.5],[.5,-.5,.5],[0,0,1]]`、`A^T=[[1,1,1,0],[0,1,-1,-1]]`。
  数值由 `--vk-conv-test` 的 winograd 用例兜底（maxErr=7.2e-07 vs CPU）。
* 权重预变换在 CPU prepare 时完成（`WinogradTransformWeights`，权重
  `[OC][IC][3][3]` → `[OC][IC][4][4]`，显存 x1.78），GPU 不做权重变换。
* 门控（用户确认）：stride=1、3x3、对称 pad、常量权重、**C>=16 且 OC>=16**
  （3 通道输入层与小通道层变换开销占比高，保持直接卷积）。x4plus 349/1714 步、
  RealESR general 33/70 步命中。`JM_VK_WINO=0` 全局禁用。
* 两个实现教训（调试自检时抓出）：
  * convw 首版漏了输入的**列变换**（只做 B^T 行变换）——2D 分离式必须
    行列都变换，1D 公式正确不等于 2D 正确，用滑动窗口（含 pad）做数值验证；
  * 输出列坐标 `ox=tX*2` 已是像素，窗口起点 `winX = ox - PX`，不能再乘 2
    （winY 的 oy 是 tile 行号所以 `oy*2` 是对的，两者量纲不同——第一版从
    第 2 个 tile 起全部错位）。

**3) descriptor 复用**

* 首选 `VK_KHR_push_descriptor`：dispatch 直接 `vkCmdPushDescriptorSetKHR`
  （零 allocate / 零 set bind），pipeline layout 用独立 push 位布局创建。
* 回退：一次 allocate 预取 8192 个 set 的轮转池（省每 dispatch 的 allocate 调用），
  pass 全同步后回卷安全；池预取失败再退"每 dispatch allocate"旧路径。
* 实测 RTX 4060 `fp16=on pushDesc=on`。

**4) 多 tile 流水（深度 2）**

* Device 层：`cmds_[2]/fences_[2]` 轮转，第 k 个 pass 用 slot k%2，复用前
  `WaitForFences`（SIGNALED 初始保证首次立即返回）；**endPass 只 submit 不等待**，
  等待职责移交给 slot 复用与 `xferFence`（下载的独立 fence，队列序上涵盖全部
  在飞 pass）。
* 激活池头部预留两份输入 + 两份输出 slot（fp16 下 tile128 共 ~1MB），
  "上传 tile k+1 输入"与"计算 tile k"在设备上重叠。
* `Graph::runStream(count, TileStream)`：`submitTile(0)` → 循环
  `submitTile(i+1); finishTile(i)`——CPU 在 `finishTile` 等待期间 GPU 正在算
  下一 tile，消除每 tile 的 GPU 空窗。取消时已提交 tile 跑完但不回读，
  上层整图重跑。`JM_VK_NO_STREAM=1` 退回串行。
* download 走 staging 环（不再固定 offset 0）：回卷时等全部在飞 fence 再复位。
  **教训**：权重 >24MB 分批上传的模型（x4plus 56MB、realcugan_pro）在
  endPass 不等待后，closePass 的 resetStaging 会覆盖还没拷完的 staging 段
  → 权重损坏（x4plus maxAbs 0.9、realcugan_pro 3e14）。修复：closePass 里
  endPass 后 `waitForAllPasses()` 再 resetStaging。

**验收（桌面 RTX 4060 Laptop）**

| 项目 | fp32+wino | fp16+wino |
| ------------------------------------------------- | --------------- | --------------- |
| `--vk-compare`（vs ORT，基线 1e-3 / fp16 2.5e-2）   | **39/39 PASS**  | **39/39 PASS**  |
| `--vk-conv-test` 直接卷积 / winograd               | 3.0e-07 / 7.2e-07 | — |
| `--vk-pad-test`                                    | bad=0           | — |
| `--vk-model-test` 全量                              | —               | **39/39 ok** nan=0 |
| `--vk-bench` waifu2x 2x tile128                     | 115ms           | **49.65ms（8.6× vs 优化前 427ms）** |
| `--vk-bench` realcugan_pro_up2x_denoise3 t128       | —               | 74.8ms |
| `--vk-bench` realesrgan_x4plus t128                 | —               | 801ms（dense/Concat 密集，wino 命中率低） |
| `--vk-stream-test` 512px（流水 vs 串行）             | —               | 1800ms vs 1892ms，**pixelHash 一致** |

fp16 数值基线（用户定夺 = **2.5e-2**）：RRDB/dense 架构 + SE 4x 的 7 个模型在
fp16 存储下的累积舍入 maxAbs 为 0.0067~0.020，与 Winograd 无关（`JM_VK_WINO=0`
对照 FAIL 集合完全一致），是 half 在 30~90 层深网络上的固有累积（NCNN fp16
同等行为，视觉无差异）。2.5e-2 基线下 fp16 全量 39/39 PASS；
`JM_VK_FP16=0` 时以 1e-3 基线复核 fp32 数值路径 39/39 PASS。

### 8.6 真机复测（Adreno 830，fp16 × Winograd 四象限矩阵，2026-11-14）

设备：V2463A / Adreno (TM) 830 / Vulkan 1.3.284 / arena 384MB（fp16=on pushDesc=on）。
CLI：`--vk-report/--vk-sweep` + 新增 `--vk-fp16 0/1`、`--vk-wino 0/1`（qputenv 落地 env）、
`--vk-compare`（sweep 成功模型追加 ORT CPU 逐像素 diff）。

**性能（--vk-bench median，tile128）**

| 模型 | fp16+wino | fp16 无wino | fp32+wino | fp32 无wino（基线） | ORT CPU |
| --------------------------------------- | --------------- | ----------- | ----------- | --------------- | ------------ |
| waifu2x_cunet_up2x                       | **439ms**       | 532ms       | 514ms       | 514ms           | 447~532ms    |
| realcugan_se_up2x                        | **720ms**       | 858ms       | 755ms       | 820ms           | 621~793ms    |
| realcugan_pro_up2x_denoise3              | 783ms           | 849ms       | **674ms**   | 820ms           | 599~776ms    |
| realesrgan_x4plus_up4x                   | DEVICE_LOST     | TIMEOUT     | DEVICE_LOST | DEVICE_LOST     | 13.0s（兜底成功） |

激活显存：fp16 52/80MB vs fp32 76/125MB（-31%/-36%）。

**精度（--vk-compare，tile192，vs ORT CPU）**

| 模型 | fp16+wino maxAbs（tol 2.5e-2） | fp32+wino maxAbs（tol 1e-3） |
| --------------------------------------- | ------------------------------- | ---------------------------- |
| waifu2x_cunet_up2x                       | 0.00132 PASS                    | （桌面 39/39 背书）           |
| realcugan_se_up2x                        | 0.00240 PASS                    | 4.5e-06 PASS                 |
| realcugan_pro_up2x_denoise3              | 0.00390 PASS                    | 5.1e-06 PASS                 |
| realesrgan_animevideo_v3_up2x            | 0.00160 PASS                    | 2.5e-06 PASS                 |
| srmd_x4                                  | 0.00191 PASS                    | 2.4e-06 PASS                 |
| realesrgan_general_up4x                  | 0.00409 PASS                    | 4.2e-06 PASS                 |

**结论（供后端优先级决策）**

1. fp16+wino 是 cunet/SE/srmd/animevideo 族的真机最优（1.14~1.17× vs 第一轮
   fp32 基线），显存 -31~36%，精度全部在 fp16 基线内（≤0.0041）。
2. **pro 系（dense/Concat 多）例外**：fp32+wino 反而最快（674ms，1.22× vs 基线；
   fp16 下 783ms）——dense 架构 unpack 开销盖过带宽收益。若按架构自适应，
   dense 系应走 fp32+wino。
3. 无 wino 组（M2）全面弱于同配置 wino 组（真机 winograd 收益 17~19%），
   无独立优势场景。
4. x4plus_up4x（RRDB 1024 节点）四组全部触发 Adreno watchdog（VK_ERROR_DEVICE_LOST
   → 熔断），与 fp16/wino 无关，是 4x 大激活模型在 Adreno 830 上的固有问题；
   ORT CPU 兜底全部成功（13s/tile）。真机 tile 应保持 128 且 4x 大模型仍建议 CPU。
5. ORT CPU 与 Vulkan fp16+wino 在真机上基本打平（0.86~1.05×）：第一轮的
   2.4× 劣势已消除，但 GPU 尚无决定性优势。后端优先级仍由用户定夺。

### 8.8 分析：RealSR-NCNN-Android 为何能跑 x4plus（真机 DEVICE_LOST 根因）

**现象**：x4plus_up4x 真机（Adreno 830）tile128 四组配置（fp16/fp32 × wino）全部
DEVICE_LOST 熔断；桌面同代码 39/39 正常。

**NCNN 侧证据（源码核对）**：

1. **分批提交（决定性）**——ncnn `src/net.cpp` 的 `NetPrivate::forward_layer`：
   ```cpp
   // for avoiding driver timeout
   // commit as soon as we collect enough pending
   const uint32_t rough_score = vkdev->info.rough_score();
   uint32_t pending_dispatch_threshold = 32 * 1024;
   if (rough_score > 75)  pending_dispatch_threshold = 8 * 1024 * 1024;
   else if (rough_score > 50)  pending_dispatch_threshold = 4 * 1024 * 1024;
   else if (rough_score > 15)  pending_dispatch_threshold = 1 * 1024 * 1024;
   else if (rough_score > 10)  pending_dispatch_threshold = 256 * 1024;
   if (cmd.pending_dispatch_total() > pending_dispatch_threshold)
       { cmd.submit_and_wait(); cmd.reset(); }
   ```
   `pending_dispatch_total` 累加每次 dispatch 的工作组数（`gx*gy*gz`）；
   Adreno 830 的 rough_score ≈ 11~15（integrated +5、subgroup 128/32=+4、
   fp16 扩展 +2）→ 走 **256K 工作组阈值**。按真机实测换算（waifu2x tile128
   单 tile 439ms ≈ 3~5M 组），NCNN 在 Adreno 上每 **~25~40ms GPU 时间**就
   submit+wait 一次——单次提交时长永远远低于驱动 watchdog。
2. **按模型架构分档的保守 tile**（tumuyan `main.cpp`）：Real-ESRGANv 系
   （x4plus 属于 v2）专门分档 `heap_budget>3300→400 / >1900→200 / >550→100 /
   >200→64 / else 32`；非 v 系才有 Adreno 特判（>2800 时 160 而非 200）。
   `heap_budget` = VK_EXT_memory_budget 的驱动上报值，或 DEVICE_LOCAL heap
   MB × 0.5~0.7（ncnn gpu.cpp `get_heap_budget()`）。
3. 全 fp16（`use_fp16_packed/storage/arithmetic = true` + winograd + sgemm）——
   我们已具备，不是差异点。

**我们的根因（真机实证）**：整图 1714 步录进一个 command buffer 一次 submit。
真机 x4plus tile128 单 pass ≈ 桌面 801ms × 8.8×（RTX 4060→Adreno 830 折算，
waifu2x 49.65 vs 439ms）≈ **7s，超过 Adreno watchdog（约 4s）** → GPU hang →
驱动重置 → DEVICE_LOST。失败点（fence 等待期间驱动重置）与显存无关
（fp16 tile128 激活 148MB，arena 384MB 放得下）。

**实证**：x4plus **tile64**（fp16+wino）真机 **3 次 bench 全部成功，
median 3837ms/tile，无 DEVICE_LOST**（单 pass 时长 ÷1.8，低于 watchdog），
且 **speedup=3.38× vs ORT CPU**（CPU tile192 12986ms）——x4plus 并非
"真机必须 CPU"，而是"单 pass 太长"。

**修复方向（A+B 组合，2026-11-14 已实现）**：

* **A. 分批提交（已实现）**：`Graph::Impl::batchCheckpoints`（prepare 末尾按每步
  `gx*gy*gz` 累计生成切点，`BatchThresholdWg()`：桌面独显 8M / 其它 256K，
  `JM_VK_BATCH_WG` 覆盖或置 0 关闭）；`run()`/`runStream()` 的 dispatch 循环在
  切点 endPass→beginPass（fence 轮转自动同步跨批依赖）。**真机实证：x4plus
  tile128 从"四组全熔断"变为 3 次 bench 全过（median 7346ms，act 148MB，
  1.77× vs CPU），conv-test 数值不变**。
* **B. watchdog 自动降档（已实现）**：`TileOverride(bool *userSet)` 区分用户
  显式设置与平台默认；`TileForModel` 对 x4plus / realesrgan_general 系在
  Android 且未显式设置 tile 时降为 64（tile128 单 pass ~7s → tile64 3.8s）。
  **真机实证：x4plus 不带显式 tile 自动用 64（3889ms，act 61MB，3.34× vs CPU）**。
  `TileForCurrentModel()` 暴露生产 tile，真机 `--vk-report` 的默认 tile 也走
  它（修复了 report 默认 192 与生产不一致的问题）。
* **设置页（已实现）**：超分组新增"Tile 大小"（自动/64/96/128/160/192/256，
  写 ui.ini [sr] tile，env JM_SR_TILE 仍优先）与"性能测试"（后台线程跑
  GPU tile64 + 自动档 + CPU tile192 一次，给出推荐后端优先级与 tile）。
  底层 API：`SrEngine::TilePreference/SetTilePreference/RunPerfProbe/
  PerfRecommendation/TileForCurrentModel`；CLI 回归入口 `--sr-perf-test`。

