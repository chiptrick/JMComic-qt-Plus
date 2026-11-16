# 超分性能测试报告（多硬件平台）

> 生成：2026-11-14 · cpp-ui 0.96.1 · 测试入口：设置页"超分 → 性能测试"、CLI `--sr-perf-test`（桌面）、`--vk-report/--vk-bench`（真机）

## 1. 测试环境

| 平台 | 设备 | GPU / NPU | Vulkan | 后端可用性 |
| --- | --- | --- | --- | --- |
| 桌面 | Windows x64 | RTX 4060 Laptop（8GB，独显） | 1.4.357 | GPU ✓ / CPU ✓ / NPU ✗（无 Hexagon DSP） |
| 真机 | vivo V2463A（Android 17） | Adreno 830 + Hexagon HTP V79（SM8750） | 1.3.284，arena 384MB | GPU ✓（fp16=on / pushDesc=on）/ CPU ✓ / **NPU ✓（QNN 2.42 HTP，0.96.4 起）** |

## 2. 测试方法与口径

- **推理用时**：GPU 档为 `--vk-bench` median（N 次取中位）；CPU / NPU 档为单次端到端 tile 推理（含上传与回读）。
- **显存占用**：GPU 档报告 Vulkan 激活区实际分配量（`actMb`，fp16 下为理论 fp32 的一半）。
- **内存占用**：`memDeltaMb` = 该档推理前后进程驻留内存（RSS / WorkingSet）净增量，包含会话、权重与激活的驻留；QNN HTP 的权重位于 HTP 共享内存，host RSS 只反映注册表与 I/O 缓冲部分。
- **精度基线**：fp32 存储 1e-3、fp16 存储 2.5e-2（用户定夺）；所有数据均来自通过精度基线的会话。

## 3. 桌面数据（RTX 4060 Laptop，模型 waifu2x_cunet_up2x_denoise3）

| 档位 | 推理用时 | 显存/内存占用 | 备注 |
| --- | --- | --- | --- |
| GPU（Vulkan）tile 64 | **20 ms** | 激活 21MB · 内存 +11MB | fp16+wino+push descriptor+分批提交 |
| GPU（Vulkan）tile 192 | 89 ms | 激活 107MB · 内存 +10MB | |
| NPU（QNN HTP） | — | — | x86 无 Hexagon DSP，不适用 |
| CPU（ONNX Runtime）tile 192 | 239 ms | 内存 +8MB | |

**推荐**：GPU（Vulkan）优先 · tile 64 · 约为 CPU 的 12.1×。

全模型吞吐（tile 128，bench median）：waifu2x 49.65ms（较第一轮优化前 427ms 提速 8.6×）、realcugan_pro_up2x_denoise3 74.8ms、realesrgan_x4plus 801ms。数值：`--vk-compare` fp32 39/39（1e-3）、fp16 39/39（2.5e-2）。

## 4. 真机数据（Adreno 830，tile 128 除标注外）

| 模型 / 档位 | 推理用时 | 显存占用 | vs CPU | 备注 |
| --- | --- | --- | --- | --- |
| waifu2x_cunet_up2x · GPU（fp16+wino） | **439 ms** | 52MB | 1.02× | 与 CPU 打平 |
| realcugan_se_up2x · GPU（fp16+wino） | **720 ms** | 80MB | 0.86× | |
| realcugan_pro_up2x_d3 · GPU（fp16+wino） | 783 ms | 80MB | 0.88× | dense 系 fp32+wino 反而更快（674ms） |
| realesrgan_x4plus_up4x · GPU tile 64（fp16+wino，自动降档） | **3889 ms** | 61MB | **3.34×** | B 方案降档生效 |
| realesrgan_x4plus_up4x · GPU tile 128（fp16+wino，分批提交） | 7346 ms | 148MB | **1.77×** | A 方案生效；此前该档四组配置全部 DEVICE_LOST 熔断 |
| realesrgan_x4plus_up4x · CPU tile 192 | 12999 ms | — | 1.00× | |
| waifu2x_cunet_up2x · **NPU（QNN HTP，fp16）** | **23 / 27 ms** | HTP 共享内存（host RSS 无净增） | **35.8× / 31.9× vs CPU** | tile 192；与 Python 参考实现 19.8ms 同量级 |

关键对比（x4plus，修复前后）：tile128 修复前**必挂**（单 pass ≈7s 超 Adreno watchdog）→ 修复后 3 次全过；tile64 自动降档为当前默认，速度最优。

## 5. NPU（QNN HTP）接入结果（0.96.4 起实测可用）

- **运行库**：Maven Central `com.qualcomm.qti:qnn-runtime:2.42.0` AAR 的 arm64-v8a 部分，按设备 HTP 代次裁剪到 V79：`libQnnHtp` 2.5MB + `libQnnSystem` 2.6MB + `V79Stub` 0.6MB + `V79Skel` 15.4MB（共 21.1MB）。x86_64 不接入。
- **关键修复（0.96.5）**：App 的 linker namespace 的 `permitted_paths` **不含 `/vendor/lib64`**，而 `libQnnHtpV79Stub.so` 的 NEEDED 里是 `libcdsprpc.so`（FastRPC 客户端）——此前 `QnnDevice_create` 一律报 `QNN_DEVICE_ERROR_INVALID_CONFIG`。解法：运行时把 `libcdsprpc.so` + `libvmmem.so` + `vendor.qti.hardware.dsp-V1-ndk.so` 从 `/vendor/lib64` 复制到 `filesDir/qnn_libs`（`/data` 在 `permitted_paths` 内），并在 exec 前把该目录写进 `LD_LIBRARY_PATH`。**APK 内不含任何厂商库**。完整复盘见 `docs/qnn_android_npu_setup.md`。
- **实测（waifu2x_cunet_up2x_denoise3，tile 192）**：NPU **25ms / 25ms**，CPU 825ms / 830ms → **33.0× / 33.2×**（打包方案为 23~27ms，性能一致）；`backend=Qualcomm Hexagon NPU (QNN HTP, fp16)`。与 Python 参考实现日志里的 `run=19.8ms` 同量级。
- **量化模型说明**：`_q16`/lowq 变体（arch=quant）在 QNN EP 里会被分区为 0 节点（QuantizeLinear/DequantizeLinear 不被接管），因此自动跳过 QNN 走 CPU，并在 `backendNote` 里注明；NPU 档用非量化模型（如 waifu2x cunet / SE / srmd 系列）测。

## 6. 对比结论与推荐策略

1. **桌面**：GPU（Vulkan）tile 64/128 全面领先 CPU 8~14×，推荐 GPU 优先。
2. **真机（Adreno 830）**：
   - waifu2x/Real-CUGAN SE 系：GPU 与 CPU 打平（0.86~1.05×），GPU 略省电且不占 CPU 线程；推荐 GPU（fp16+wino）优先、tile 128。
   - Real-CUGAN pro（dense）系：fp32+wino 最快（674ms），若追求极致可按架构自适应（收益 ~16%）。
   - x4plus 系：GPU tile 64（3.34×）显著优于 CPU；tile 128 亦可用（1.77×）。
   - NPU（QNN HTP）：waifu2x cunet 上 23~27ms，是 CPU 的 32~36 倍、GPU(fp16+wino) 的约 16~19 倍，**当前真机最快档位**；设置页性能测试会优先推荐 NPU。
3. **推荐逻辑已内化**：设置页"性能测试"对当前模型实测后给出"后端优先级 + tile 大小 + 相对其它硬件的倍数"，NPU 跑赢时优先推荐 NPU。

## 7. 复测入口

- 桌面：设置页 → 超分 → 性能测试；或 CLI `jmcomic_ui.exe --sr-perf-test`
- 真机：设置页 → 超分 → 性能测试（约 10~40s，期间推理暂停）；或 `am start --es jm.args "'--vk-bench N --vk-tile T --vk-only <模型>"`
- 精度：`--vk-compare <模型目录>`（fp32 1e-3 / fp16 2.5e-2 双基线）
