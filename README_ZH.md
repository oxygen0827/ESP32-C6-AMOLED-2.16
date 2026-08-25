# ESP32-C6-Touch-AMOLED-2.16 本地资料与示例

本目录对应 Waveshare **ESP32-C6-Touch-AMOLED-2.16**，与旁边的
`ESP32-S3-Touch-AMOLED-2.16` 是两个不同主控 SKU。当前连接的开发板通过
USB-Serial/JTAG 识别为 ESP32-C6，因此本目录才是匹配的示例和资料集合。

## 官方来源

- 产品页：<https://www.waveshare.com/esp32-c6-touch-amoled-2.16.htm>
- 中文资料页：<https://docs.waveshare.net/ESP32-C6-Touch-AMOLED-2.16/Resources-And-Documents>
- 示例仓库（GitHub）：<https://github.com/waveshareteam/ESP32-C6-Touch-AMOLED-2.16>
- 示例仓库（官网链接的 Gitee 镜像）：<https://gitee.com/waveshare/ESP32-C6-Touch-AMOLED-2.16>

本地代码树来自 GitHub 提交 `294543798f1a44e2f2c4d2976522323f2beee11d`；
官网 Gitee 镜像当前提交为 `b77979b8302e1f94b81a7f18696ce60d7afde94a`，两者内容树一致。

## 本地内容

- `01_Arduino_Libraries/`：官方随仓库提供的库和依赖
- `02_Example/Arduino-v3.3.3/`：9 个 Arduino 例程，目标 Arduino-ESP32 3.3.3
- `02_Example/ESP-IDF-v5.5.3/`：9 个 ESP-IDF 例程，目标 `esp32c6`
- `02_Example/XiaoZhi-v2.2.5/`：官方随附的 XiaoZhi v2.2.5 工程
- `03_Firmware/`：官方预编译固件，仅用于恢复/对照，烧录前必须确认分区和地址
- `resource/`：从官方资料页下载的原理图、尺寸包、C6 主控手册和板载器件资料

资料来源、哈希和仍需核验的项目见 [resource/资料索引.md](resource/资料索引.md)。
