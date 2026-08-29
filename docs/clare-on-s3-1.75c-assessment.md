# Clare 部署到 ESP32-S3-Touch-AMOLED-1.75C 的可行性评估（2026-08-29）

> 评估对象：附件 `ref-ESP32-S3-Touch-AMOLED-1.75C/`（Waveshare 官方板卡仓库，含 ESP-IDF v5.5.5 示例、XiaoZhi v2.4.2 完整固件、官方 BSP 托管组件）。
> 问题：把 Clare 从 C6 2.16 板移植到这块 S3 板，能否"一次成功"？

## 结论

**高概率一次成功（估计 80–90%），但不是"零改动"——需要一次明确的移植工作（显示适配 + 引脚/配置重建），工作量约半天到一天。** C6 上折磨了三天的所有致命问题（TLS 握手失败、X509_ALLOC、MPI_ALLOC、esp-aes 分配失败、60s 超时 hack）在这块板上**结构性不存在**。

## 一、硬件同源度：出乎意料地高

| 子系统 | Clare C6（2.16 板） | S3 1.75C 板 | 移植影响 |
|---|---|---|---|
| 音频编解码 | ES8311 + ES7210（`03_Clare_C6/components/port_bsp/codec_bsp.cpp`） | **同款 ES8311 + ES7210**（README_ZH.md 硬件概览） | 几乎零改动 |
| 触摸 | CST9217（`waveshare/esp_lcd_touch_cst9217 ^1.0.4`） | **同款 CST9217** | 零改动 |
| 电源管理 | AXP2101 @0x34（`main.cpp:508`） | **同款 AXP2101** | 零/极小改动 |
| 显示 | SH8601，480×480 QSPI | **CO5300，466×466 QSPI** | **主要改动点**：面板驱动不同、分辨率硬编码 480（`clare_ui.cpp:405`）需改 466 |
| MCU | ESP32-C6，单核 160MHz，无 PSRAM | ESP32-S3，双核 240MHz，**8MB Octal PSRAM**，32MB Flash | 所有内存类问题消失 |

## 二、C6 的全部坑在这块板上结构性消失

对照 `docs/why-c6-harder-than-s3.md` 的五大根因逐条核对：

1. **内存**：XiaoZhi 与 LVGL 示例的 sdkconfig 均证实 `SPIRAM_MODE_OCT` + `SPIRAM_SPEED_80M`（`02_Example/XiaoZhi-v2.4.2/sdkconfig.defaults.esp32s3`）。可直接照搬喵伴的 `MBEDTLS_EXTERNAL_MEM_ALLOC=y`——mbedTLS 缓冲进 PSRAM，X509/MPI/记录缓冲分配失败整类消失，**还能把完整的证书链验证开回来**（不用走 INSECURE 妥协路线）。
2. **硬件加速器**：S3 的 AES/MPI/SHA 驱动配 vocat 验证过的配置，DMA 分配有余量，不需要切软件三件套。
3. **CPU**：双核 240MHz，TLS 握手不再和 Wi-Fi/LVGL 抢单核，60s 超时、任务优先级 hack 全部可以回退。
4. **Wi-Fi/lwIP**：可直接采用喵伴验证过的大缓冲配置（SND_BUF 65535 等），不用 MSS 1240 压缩。
5. **sdkconfig 参考**：`vocat/products/ws_meeting_demo/sdkconfig.defaults` 就是同协议栈（同后端、双 WSS）在 S3 上的生产验证配置，照抄即可。

## 三、已有"全链路已通"的活证明

- **XiaoZhi v2.4.2 原生支持这块板**：`main/Kconfig.projbuild:517` 有 `BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_1_75C`。小智是完整的生产级语音助手（音频采集+播放、Wi-Fi、TLS、LVGL 全链路），等于这块板的"Clare 同类应用"已经跑通。
- **03_esp-brookesia 示例内含 GMF 音频代理**（`components/brookesia_core/ai_framework/agent/audio_processor.c`），与喵伴 vocat 同一套 GMF 栈。
- **官方 BSP 托管组件** `waveshare/esp32_s3_touch_amoled_1_75c ^3.0.0` 包揽显示/触摸/音频/PMIC 初始化——不用像 C6 那样手写四个 BSP 文件。

## 四、剩余风险（诚实的 10–20%）

1. **显示适配是唯一实质改动**：Clare 的 `display_bsp`/`lvgl_bsp` 是为 SH8601 手写的，要换成 CO5300（BSP 已封装）；UI 有 480 硬编码（`clare_ui.cpp:405`、`:373` 标签）需参数化为 466。AMOLED 圆角/偏移若不同需微调。
2. **引脚全部重映射**：`user_config.h` 全是 C6 引脚号，改由官方 BSP 接管后这文件基本作废，但要确认 Clare 代码里没有绕过 BSP 直接操作引脚的地方。
3. **构建配置一次做对**：分区表、Flash 32MB、PSRAM 参数、`sdkconfig.private` 首写优先的老坑（删了再 reconfigure）——这些是我们已经踩过的流程坑，照清单执行即可。
4. **IDF 版本**：C6 工程用 5.5.3，板仓库示例用 5.5.5。建议直接在 5.5.5 上构建新工程（BSP ^3.0.0 针对它验证过），不要复用 5.5.3。
5. **不推荐的做法**：把 C6 工程原地改 target。建议新建 S3 工程，把 `clare_net/clare_audio/clare_ui/main` 四个文件 + `clare_ca_chain.h` 平移过去，BSP 层整个换新。

## 五、建议执行路径

1. 以 `02_Example/ESP-IDF-v5.5.5/02_lvgl_demo_v9`（BSP + LVGL 9.5）为骨架建新工程 `04_Clare_S3`；
2. sdkconfig 合并：喵伴 `ws_meeting_demo/sdkconfig.defaults` 的 PSRAM/mbedTLS/lwIP/Wi-Fi 段 + 板卡的 32MB Flash/BSP 段；
3. 平移 Clare 四个 main 文件，显示初始化换 BSP 调用，分辨率 466 参数化；
4. 保留完整证书验证（`clare_ca_chain.h` + global CA store），PSRAM 兜底；
5. 烧录跑 boot 自测，预期三绿（HTTP + WSS kind=0/1）。

**验收标准**（与 C6 同一套）：`session err=0` + transcribe/host 双 WSS 握手成功 + 无 ALLOC 类报错。
