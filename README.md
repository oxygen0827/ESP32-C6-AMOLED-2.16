# ESP32-C6 Touch AMOLED 2.16

ESP32-C6 Touch AMOLED 2.16 的 Arduino、ESP-IDF 和 Clare C6 示例工程及硬件资料。

官方资料：

- 中文 Wiki：<https://www.waveshare.net/wiki/ESP32-C6-Touch-AMOLED-2.16>
- English Wiki：<https://www.waveshare.com/wiki/ESP32-C6-Touch-AMOLED-2.16>

## 目录

- `01_Arduino_Libraries/`：LVGL 与板级 Arduino 库
- `02_Example/`：Arduino 和 ESP-IDF 示例
- `03_Clare_C6/`：Clare C6 应用工程
- `03_Firmware/`：可直接烧录的固件
- `resource/`：原理图、数据手册和尺寸资料

构建输出、ESP-IDF 自动生成组件、本机 `sdkconfig` 和私密配置均由 `.gitignore` 排除；依赖会在首次配置工程时重新生成。

## Arduino 工具配置

![Arduino 工具配置](<Tools Configuration.png>)
