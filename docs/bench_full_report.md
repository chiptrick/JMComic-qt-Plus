# 超分模型全量测速报告（46 模型 × 全部可用硬件）

- 生成时间：2026-11-15
- 探针：192×192 单块（部分模型带 tile 覆盖，见引擎日志），GPU 3 次取中位数，NPU/CPU 单次
- 数值单位 ms；`✗` 表示该硬件明确拒绝（原因见第五节）

## 一、本机（Intel Arc + NVIDIA RTX 4060 Laptop + CPU）

| 硬件 | 成功 | 失败 | 中位数 | 最快 | 最慢 |
|---|---|---|---|---|---|
| GPU0 · Intel(R) Arc(TM) Graphics | 39/46 | 7 | 430ms | 115ms | 5.78s |
| GPU1 · NVIDIA GeForce RTX 4060 Laptop GPU | 39/46 | 7 | 112ms | 53ms | 1.15s |
| CPU · ONNX Runtime | 46/46 | 0 | 752ms | 137ms | 6.97s |

NPU 不可用：onnxruntime 未包含 QNN EP（桌面版 ONNX Runtime 不含 QNN EP）

界面内「一键测试全部」同口径复跑一遍：46 模型 **222.1s**，每行第三行同时列出 `GPU0 / GPU1 / CPU` 三条耗时（见 `docs/shots/sr_bench_dual_results.png`）。

## 二、Android 真机（vivo V2463A，Adreno 830 + QNN HTP + CPU）

| 硬件 | 成功 | 失败 | 中位数 | 最快 | 最慢 |
|---|---|---|---|---|---|
| GPU0 · Adreno (TM) 830 | 39/46 | 7 | 1.26s | 492ms | 14.51s |
| NPU · QNN HTP | 39/46 | 7 | 34ms | 8ms | 412ms |
| CPU · ONNX Runtime | 46/46 | 0 | 1.09s | 217ms | 16.73s |

NPU 相对 CPU 中位加速 **32.1×**（1.09s → 34ms）；Adreno 830 中位 1.26s，反而慢于 CPU —— 单块 192×192 时 Vulkan 的着色器编译与权重上传摊不薄。

## 三、逐模型明细（本机，单位 ms）

| # | 模型 | Arc | RTX 4060 | CPU |
|---:|---|---:|---:|---:|
| 1 | waifu2x cunet 2x 降噪3 | 297 | 87 | 289 |
| 2 | waifu2x cunet 2x 降噪2 | 297 | 99 | 265 |
| 3 | waifu2x cunet 2x 降噪1 | 296 | 89 | 514 |
| 4 | waifu2x cunet 2x 降噪0 | 299 | 88 | 345 |
| 5 | waifu2x cunet 2x | 306 | 96 | 290 |
| 6 | waifu2x cunet 1x 降噪3 | 117 | 53 | 146 |
| 7 | waifu2x cunet 1x 降噪2 | 115 | 53 | 176 |
| 8 | waifu2x cunet 1x 降噪1 | 117 | 54 | 182 |
| 9 | waifu2x cunet 1x 降噪0 | 118 | 53 | 137 |
| 10 | Real-CUGAN SE 2x | 430 | 126 | 974 |
| 11 | Real-CUGAN SE 2x 保守 | 437 | 118 | 1196 |
| 12 | Real-CUGAN SE 2x 降噪1 | 416 | 112 | 564 |
| 13 | Real-CUGAN SE 2x 降噪2 | 408 | 114 | 466 |
| 14 | Real-CUGAN SE 2x 降噪3 | 402 | 109 | 380 |
| 15 | Real-CUGAN SE 3x | 880 | 244 | 730 |
| 16 | Real-CUGAN SE 3x 保守 | 887 | 240 | 873 |
| 17 | Real-CUGAN SE 3x 降噪3 | 979 | 238 | 1044 |
| 18 | Real-CUGAN SE 4x | 728 | 202 | 925 |
| 19 | Real-CUGAN SE 4x 保守 | 747 | 201 | 897 |
| 20 | Real-CUGAN SE 4x 降噪3 | 760 | 203 | 967 |
| 21 | Real-CUGAN Pro 2x | 413 | 107 | 643 |
| 22 | Real-CUGAN Pro 2x 保守 | 430 | 112 | 602 |
| 23 | Real-CUGAN Pro 2x 降噪3 | 414 | 110 | 428 |
| 24 | Real-CUGAN Pro 3x | 875 | 238 | 886 |
| 25 | Real-CUGAN Pro 3x 保守 | 921 | 239 | 1136 |
| 26 | Real-CUGAN Pro 3x 降噪3 | 924 | 232 | 1086 |
| 27 | Real-ESRGAN anime 2x | 214 | 61 | 392 |
| 28 | Real-ESRGAN anime 3x | 200 | 72 | 384 |
| 29 | Real-ESRGAN anime 4x | 241 | 82 | 440 |
| 30 | Real-ESRGAN x4plus 4x | 5777 | 1148 | 6972 |
| 31 | Real-ESRGAN x4plus anime 4x | 2041 | 404 | 2615 |
| 32 | Real-ESRGAN general 4x | 425 | 117 | 538 |
| 33 | Real-ESRGAN general wdn 4x | 461 | 114 | 667 |
| 34 | SRMD 2x | 457 | 108 | 783 |
| 35 | SRMD 3x | 474 | 112 | 768 |
| 36 | SRMD 4x | 498 | 132 | 699 |
| 37 | SRMD-NF 2x | 460 | 102 | 737 |
| 38 | SRMD-NF 3x | 522 | 126 | 830 |
| 39 | SRMD-NF 4x | 551 | 187 | 842 |
| 40 | 量化 · realcugan_pro_up2x_q16_lowq | ✗ | ✗ | 1036 |
| 41 | 量化 · realcugan_pro_up3x_conservative_q16_lowq | ✗ | ✗ | 1766 |
| 42 | 量化 · realcugan_pro_up3x_q16_lowq | ✗ | ✗ | 1983 |
| 43 | 量化 · realesrgan_general_up4x_q16_lowq | ✗ | ✗ | 910 |
| 44 | 量化 · realesrgan_general_wdn_up4x_q16_lowq | ✗ | ✗ | 928 |
| 45 | 量化 · srmdnf_x2_q16_lowq | ✗ | ✗ | 951 |
| 46 | 量化 · srmdnf_x3_q16_lowq | ✗ | ✗ | 1005 |

## 四、逐模型明细（真机，单位 ms）

| # | 模型 | Adreno 830 | NPU | CPU |
|---:|---|---:|---:|---:|
| 1 | waifu2x cunet 2x 降噪3 | 1082 | 24 | 572 |
| 2 | waifu2x cunet 2x 降噪2 | 1079 | 24 | 552 |
| 3 | waifu2x cunet 2x 降噪1 | 1077 | 25 | 542 |
| 4 | waifu2x cunet 2x 降噪0 | 1080 | 25 | 546 |
| 5 | waifu2x cunet 2x | 1076 | 24 | 548 |
| 6 | waifu2x cunet 1x 降噪3 | 494 | 8 | 221 |
| 7 | waifu2x cunet 1x 降噪2 | 495 | 9 | 217 |
| 8 | waifu2x cunet 1x 降噪1 | 495 | 8 | 222 |
| 9 | waifu2x cunet 1x 降噪0 | 496 | 9 | 225 |
| 10 | Real-CUGAN SE 2x | 1330 | 33 | 793 |
| 11 | Real-CUGAN SE 2x 保守 | 1329 | 32 | 772 |
| 12 | Real-CUGAN SE 2x 降噪1 | 1286 | 32 | 799 |
| 13 | Real-CUGAN SE 2x 降噪2 | 1292 | 32 | 795 |
| 14 | Real-CUGAN SE 2x 降噪3 | 1297 | 32 | 791 |
| 15 | Real-CUGAN SE 3x | 2744 | 81 | 1683 |
| 16 | Real-CUGAN SE 3x 保守 | 2738 | 84 | 1507 |
| 17 | Real-CUGAN SE 3x 降噪3 | 2788 | 77 | 1473 |
| 18 | Real-CUGAN SE 4x | 2197 | 59 | 1222 |
| 19 | Real-CUGAN SE 4x 保守 | 2209 | 66 | 1212 |
| 20 | Real-CUGAN SE 4x 降噪3 | 2205 | 66 | 1218 |
| 21 | Real-CUGAN Pro 2x | 1305 | 34 | 851 |
| 22 | Real-CUGAN Pro 2x 保守 | 1303 | 34 | 856 |
| 23 | Real-CUGAN Pro 2x 降噪3 | 1310 | 34 | 863 |
| 24 | Real-CUGAN Pro 3x | 2742 | 97 | 1628 |
| 25 | Real-CUGAN Pro 3x 保守 | 2756 | 92 | 1660 |
| 26 | Real-CUGAN Pro 3x 降噪3 | 2733 | 92 | 1649 |
| 27 | Real-ESRGAN anime 2x | 492 | 26 | 539 |
| 28 | Real-ESRGAN anime 3x | 493 | 37 | 537 |
| 29 | Real-ESRGAN anime 4x | 500 | 41 | 572 |
| 30 | Real-ESRGAN x4plus 4x | 14506 | 412 | 16734 |
| 31 | Real-ESRGAN x4plus anime 4x | 4464 | 142 | 5209 |
| 32 | Real-ESRGAN general 4x | 983 | 50 | 1052 |
| 33 | Real-ESRGAN general wdn 4x | 986 | 52 | 1071 |
| 34 | SRMD 2x | 1187 | 29 | 1189 |
| 35 | SRMD 3x | 1230 | 44 | 1207 |
| 36 | SRMD 4x | 1257 | 45 | 1226 |
| 37 | SRMD-NF 2x | 1176 | 31 | 1186 |
| 38 | SRMD-NF 3x | 1208 | 42 | 1203 |
| 39 | SRMD-NF 4x | 1236 | 46 | 1232 |
| 40 | 量化 · realcugan_pro_up2x_q16_lowq | ✗ | ✗ | 922 |
| 41 | 量化 · realcugan_pro_up3x_conservative_q16_lowq | ✗ | ✗ | 1766 |
| 42 | 量化 · realcugan_pro_up3x_q16_lowq | ✗ | ✗ | 1822 |
| 43 | 量化 · realesrgan_general_up4x_q16_lowq | ✗ | ✗ | 1115 |
| 44 | 量化 · realesrgan_general_wdn_up4x_q16_lowq | ✗ | ✗ | 1128 |
| 45 | 量化 · srmdnf_x2_q16_lowq | ✗ | ✗ | 1190 |
| 46 | 量化 · srmdnf_x3_q16_lowq | ✗ | ✗ | 1229 |

## 五、失败项说明

- 7 个 `量化 · *_q16_lowq` 模型在两块 GPU 上均被拒绝：`量化模型（QDQ，含 DequantizeLinear）暂不支持 Vulkan 后端`。这是 Vulkan 后端的设计约束，不是测速异常。
- 同一批量化模型在真机 NPU 上 `QNN 建会话回退（cpu）`：QNN EP 未接管这些 QDQ 图，按失败记录；CPU 全部可跑。
- 桌面 NPU 整列不可用（无 QNN EP），按约定只在工具条图例标注一次，不逐行重复。

## 六、复现命令

```
# 桌面（本机）
jmcomic_ui.exe --sr-bench all

# Android（jm.args 走 intent extra）
adb shell 'am start -n org.jmcomic.ui/org.qtproject.qt.android.bindings.QtActivity --es jm.args "--sr-bench all"'
```

多 GPU 工具条图例见 `docs/shots/sr_bench_dualgpu.png`。