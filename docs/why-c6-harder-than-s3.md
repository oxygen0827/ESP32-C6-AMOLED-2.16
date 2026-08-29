# 为什么 Clare 在 C6 上问题远多于 S3（喵伴板）— 对照分析（2026-08-29）

> 对照样本：附件 `vocat/`（喵伴 ESP-VoCat 出厂固件，含 `products/speaker` 与 `products/ws_meeting_demo`，均为 ESP32-S3，已部署验证）vs 本仓库 `03_Clare_C6`（ESP32-C6 移植，调试日志见 `logs/` 2026-08-26 ~ 08-28 共 60+ 份）。

## 一句话结论

**C6 上暴露的绝大多数问题（TLS 握手失败、X509_ALLOC、MPI_ALLOC、esp-aes 分配失败、堆泄漏致命化）在 S3 上同样存在，但 S3 用 8MB Octal PSRAM + 跳过证书验证把它们全部掩盖了。C6 没有 PSRAM、RAM 只有一个数量级更小的内部 SRAM，同样的代码缺陷直接变成致命错误。** 这不是"C6 芯片差"，而是两块板的内存预算差了近 200 倍。

## 1. 内存：决定性差距（根因）

| 项目 | 喵伴 S3 | C6（Waveshare 2.16） |
|---|---|---|
| 外部 RAM | **8MB Octal PSRAM**（`SPIRAM_MODE_OCT` + XIP，`vocat/products/ws_meeting_demo/sdkconfig.defaults:14-20`） | **无 PSRAM**（ESP32-C6 硬件不支持外部 RAM） |
| 可用堆 | 兆字节级 | 启动后约 **192 KiB 区域**，Wi-Fi/LVGL 初始化后 **free≈43 KB、最大连续块≈25 KB**（`logs/clare_heapmap_20260828.log:171`） |
| mbedTLS 缓冲区去向 | `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` → **全部进 PSRAM**（`sdkconfig.defaults:50`） | 只能挤内部 SRAM |
| Wi-Fi/lwIP 缓冲 | `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` → PSRAM | 内部 SRAM，还被迫压缩（MSS 1240、动态 RX 缓冲 16） |
| .bss 段 | 可放外部 RAM | 全部占内部 SRAM |

一条 TLS 握手的典型内存需求：IN/OUT 记录缓冲（默认 16 KB+16 KB）+ X509 证书链解析（4 证书 Let's Encrypt 链需数 KB 连续块）+ RSA bignum。**在 S3 上这些全落在 8MB PSRAM 里；在 C6 上它们要在一个只剩 25 KB 最大连续块的堆里抢位置。**

## 2. 证书验证：S3 直接跳过了最难的一步

喵伴两个工程都设了（`vocat/products/ws_meeting_demo/sdkconfig.defaults:10-11`、`products/speaker/sdkconfig.defaults:14-15`）：

```
CONFIG_ESP_TLS_INSECURE=y
CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y
```

即 **S3 参考固件根本不解析、不验证服务器证书链**。而 C6 版 Clare 走了"正确"路线：固定 CA 链（`main/clare_ca_chain.h`，5 证书）+ 全量验证。于是：

- X509 解析 4~5 张 RSA 证书 → 需要大块连续堆 → 碎片化后 `X509_ALLOC_FAILED (-0x2880)`
- RSA-2048 验签 → bignum 运算 → `MPI_ALLOC_FAILED (-0x0010)`

S3 从没踩过这些坑，不是因为它更稳，而是因为它**把这一整段代码路径关掉了**。C6 的 `sdkconfig.defaults:83-89` 现在也加了同样的 INSECURE 开关作为"路线 A"实验——这本质上就是向 S3 的做法靠拢。

## 3. 硬件加速器的陷阱

C6 的 AES/MPI/SHA 硬件驱动在每次握手中途要从 `MALLOC_CAP_DMA` 池分配描述符；堆一碎就失败（`logs/clare_clean_retest_20260828.log`、`clare_ws_fix_20260828.log`），最后被迫三项全切软件（`03_Clare_C6/sdkconfig.defaults:68-81`）。

S3 为什么没事：DMA 池在 8MB+512KB 的内存里永远分得出描述符。**同一个驱动 bug/设计约束，有内存余量时永远不触发。**

## 4. CPU：单核 160 MHz vs 双核 240 MHz

- S3：双核 Xtensa 240 MHz，Wi-Fi 协议栈、TLS、LVGL、音频管线可以分到两核。
- C6：单核 RISC-V 160 MHz，TLS 握手期间 Wi-Fi RX、看门狗、LVGL 刷新全挤一个核。这就是 C6 上要把 `network_timeout_ms` 拉到 60 s、WS 任务优先级提到 10 才勉强稳定的原因（`docs/HANDOFF-2026-08-28.md §3.4`），也是软件 RSA 验签耗时敏感的根源。

## 5. 泄漏其实两边都有，只是 S3 稀释了

C6 侧测到的关键证据（`HANDOFF §4`）：一次 HTTP 会话后永久丢失 ~6 KB 堆、最大连续块掉 10 KB（esp_http_client/esp-tls 清理路径泄漏）。这段代码是 IDF 组件，**在 S3 上逐字节相同地泄漏**——只是 6 KB 对 8MB PSRAM 是噪声，对 C6 的 43 KB 余量是死刑。这就是为什么"C6 上问题这么多"：不是 C6 引入了更多 bug，而是 **C6 没有任何掩盖 bug 的余量**。

## 6. 其他次要差异

| 差异 | 影响 |
|---|---|
| IDF 5.5.1（喵伴）vs 5.5.3（C6） | esp-tls / esp_websocket_client 行为微调，已逐项排除 |
| esp_websocket_client 1.5.0（S3 参考）vs 1.8.0（C6） | 配置传递逻辑读过源码，无实质差异 |
| S3 参考 WS buffer_size=16384（`transcribe_ws.c:164`） | 在 PSRAM 里随意开大；C6 被迫把 TLS IN 压到 8192、OUT 压到 2048 |
| lwIP：S3 大开（SND_BUF 65535、RECVBOX 等） | C6 全面压缩以省 RAM |

## 7. 对 C6 移植的直接启示（按 ROI 排序）

1. **接受 S3 的现实主义**：C6 上启用 `ESP_TLS_INSECURE + SKIP_SERVER_CERT_VERIFY`（已是 sdkconfig.defaults 的路线 A 实验），或改用 **ECDSA 证书端点**（Let's Encrypt 可申请 EC 链，解析/验签内存和耗时都比 RSA 小一个量级）。
2. **固定 CA 链方案若保留**：必须在 HTTP 会话后做堆整理，或找到那 ~6 KB 泄漏（HANDOFF §6 第 2 步方案 A/B 仍然适用）。
3. **不要试图在 C6 上复刻 S3 的缓冲配置**：S3 的 16 KB WS buffer、64 KB TCP 发送窗是 PSRAM 特权，C6 必须维持压缩值。
4. **软件加密三件套（AES/SHA/MPI）保留**：在 C6 的碎片化堆上，硬件加速器驱动本身就是不稳定源。
5. 长期看，如果产品形态确定要做"会议转写+问答"双 WSS 长连接，**带 PSRAM 的 S3（或 C 系列外挂 RAM 的方案）是结构性更合适的硬件**；C6 适合做被阉割过的单连接轻量客户端。

## 附：证据索引

- C6 堆水位：`logs/clare_heapmap_20260828.log`、`logs/clare_sw_sha_20260828.log`
- 证书链/验证细节：`logs/clare_verify_info_20260828.log`
- 硬件加速失败：`logs/clare_clean_retest_20260828.log`、`logs/clare_ws_fix_20260828.log`
- 泄漏与遗留问题清单：`docs/HANDOFF-2026-08-28.md`
- S3 参考配置：`vocat/products/ws_meeting_demo/sdkconfig.defaults`、`vocat/products/speaker/sdkconfig.defaults`
- S3 参考 WS 实现：`vocat/products/ws_meeting_demo/main/transcribe_ws.c`、`host_ws.c`
