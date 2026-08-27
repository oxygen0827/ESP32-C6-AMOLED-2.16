# Clara C6 实机日志记录

- 设备端口: `/dev/cu.usbmodem21201`
- 波特率: `115200`
- 记录开始: 2026-08-26 22:00:48
- 固件: `03_Clara_C6/build/Clara_C6.bin`

## 说明

本文档通过串口实时采集 ESP32-C6 开发板的运行日志，用于排查 UI 字体显示和 `Connection lost` 网络问题。

## 日志

```
[0;33mW (1554) sh8601: The 3Ah command has been used and will be overwritten by external initialization sequence[0m
[0;33mW (1554) sh8601: The 36h command has been used and will be overwritten by external initialization sequence[0m
[0;32mI (1738) CST9217: Checkcode: 0x204ECACA[0m
[0;32mI (1740) CST9217: Resolution X: 480, Y: 480[0m
[0;32mI (1742) CST9217: Chip Type: 0x9220, ProjectID: 0x542F[0m
[0;32mI (1742) esp_lvgl:adapter: LVGL adapter initialized successfully[0m
[0;32mI (1752) esp_lvgl:touch: Touch input device registered successfully (IRQ mode: enabled)[0m
[0;32mI (1760) esp_lvgl:adapter: LVGL task started successfully[0m
i2c: {sda: 8, scl: 7}
i2s: {mclk: 19, bclk: 20, ws: 22, din: 21, dout: 23}
out: {codec: ES8311, pa: -1, pa_gain: 6, use_mclk: 1, pa_gain: 6}
Codec 0 dir 2 type:1
in: {codec: ES7210}
Codec 1 dir 1 type:2
[0;32mI (1782) CODEC_INIT: in:1 out:1 port: 1[0m
[0;32mI (1786) CODEC_INIT: Success to int i2c: 0[0m
[0;32mI (1792) CODEC_INIT: Init i2s 0 type: 3 mclk:19 bclk:20 ws:22 din:21 dout:23[0m
[0;32mI (1799) CODEC_INIT: tx:0x4087e71c rx:0x4087e8d8[0m
[0;33mW (1804) i2s_tdm: the current mclk multiple is too small, adjust the mclk multiple to 384[0m
[0;32mI (1813) CODEC_INIT: output init tdm ret 0[0m
[0;33mW (1817) i2s_common: the rx channel on I2S0 is switched from master to slave for full-duplex mode[0m
[0;33mW (1827) i2s_tdm: the current mclk multiple is too small, adjust the mclk multiple to 384[0m
[0;33mW (1836) i2s_tdm: the mclk/bclk ratio 3 is too small for the full-duplex rx slave (min 4), data might be sampled incorrectly, please increase mclk_multiple[0m
[0;32mI (1851) CODEC_INIT: Input init tdm ret 0[0m
[0;32mI (1855) CODEC_INIT: Init i2s 0 ok[0m
[0;32mI (1859) CODEC_INIT: Success to init i2s: 0[0m
[0;32mI (1864) CODEC_INIT: Success to int i2c: 0[0m
[0;32mI (1869) CODEC_INIT: Success to init i2s: 0[0m
[0;32mI (1874) CODEC_INIT: Get out handle 0x4087e71c port 0[0m
[0;32mI (1885) ES8311: Work in Slave mode[0m
[0;32mI (1890) ES7210: Work in Slave mode[0m
[0;32mI (1897) ES7210: Enable ES7210_INPUT_MIC1[0m
[0;32mI (1899) ES7210: Enable ES7210_INPUT_MIC2[0m
[0;32mI (1902) ES7210: Enable ES7210_INPUT_MIC3[0m
[0;32mI (1905) ES7210: Enable ES7210_INPUT_MIC4[0m
[0;32mI (1909) ES7210: Enable TDM mode[0m
[0;32mI (1918) I2S_IF: channel mode 2 bits:16/16 channel:2 mask:3[0m
[0;32mI (1918) I2S_IF: TDM Mode 1 bits:16/16 channel:2 sample_rate:16000 mask:3[0m
[0;32mI (1939) Adev_Codec: Open codec device OK[0m
[0;32mI (1939) I2S_IF: channel mode 2 bits:16/16 channel:2 mask:3[0m
[0;32mI (1939) I2S_IF: TDM Mode 0 bits:16/16 channel:2 sample_rate:16000 mask:3[0m
[0;32mI (1947) ES7210: Bits 8[0m
[0;32mI (1956) ES7210: Enable ES7210_INPUT_MIC1[0m
[0;32mI (1959) ES7210: Enable ES7210_INPUT_MIC2[0m
[0;32mI (1961) ES7210: Enable ES7210_INPUT_MIC3[0m
[0;32mI (1966) ES7210: Enable ES7210_INPUT_MIC4[0m
[0;32mI (1971) ES7210: Enable TDM mode[0m
[0;32mI (1977) ES7210: Unmuted[0m
[0;32mI (1977) Adev_Codec: Open codec device OK[0m
[0;32mI (1980) Adev_Codec: Input already open[0m
[0;32mI (1984) Adev_Codec: Input already open[0m
[0;32mI (2174) pp: pp rom version: 5b8dcfa[0m
[0;32mI (2175) net80211: net80211 rom version: 5b8dcfa[0m
I (2176) wifi:wifi driver task: 40868e1c, prio:23, stack:6656, core=0
I (2185) wifi:wifi firmware version: b9f67df
I (2185) wifi:wifi certification version: v7.0
I (2187) wifi:config NVS flash: enabled
I (2191) wifi:config nano formatting: disabled
I (2195) wifi:mac_version:HAL_MAC_ESP32AX_761,ut_version:N, band mode:0x1
I (2202) wifi:Init data frame dynamic rx buffer num: 16
I (2206) wifi:Init static rx mgmt buffer num: 5
I (2210) wifi:Init management short buffer num: 32
I (2215) wifi:Init static tx buffer num: 16
I (2219) wifi:Init static tx FG buffer num: 2
I (2223) wifi:Init static rx buffer size: 1700 (rxctrl:92, csi:512)
I (2229) wifi:Init static rx buffer num: 10
I (2233) wifi:Init dynamic rx buffer num: 16
[0;32mI (2238) wifi_init: accept mbox: 6[0m
[0;32mI (2241) wifi_init: tcpip mbox: 32[0m
[0;32mI (2245) wifi_init: udp mbox: 6[0m
[0;32mI (2249) wifi_init: tcp mbox: 6[0m
[0;32mI (2253) wifi_init: tcp tx win: 5760[0m
[0;32mI (2257) wifi_init: tcp rx win: 5760[0m
[0;32mI (2261) wifi_init: tcp mss: 1240[0m
[0;32mI (2265) wifi_init: WiFi IRAM OP enabled[0m
[0;32mI (2270) wifi_init: WiFi RX IRAM OP enabled[0m
[0;32mI (2275) wifi_init: WiFi SLP IRAM OP enabled[0m
W (2280) wifi:Password length matches WPA2 standards, authmode threshold changes from OPEN to WPA2
[0;32mI (2289) phy_init: phy_version 345,552b044,Jul 14 2026,10:37:00[0m
I (2358) wifi:11ax coex: WDEVAX_PTI0(0x55777555), WDEVAX_PTI1(0x00003377).

I (2358) wifi:mode : sta (20:6e:f1:16:e0:a4)
I (2359) wifi:enable tsf
I (2373) wifi:new:<6,0>, old:<1,0>, ap:<255,255>, sta:<6,0>, prof:1, snd_ch_cfg:0x0
I (2374) wifi:(connect)dot11_authmode:0x3, pairwise_cipher:0x3, group_cipher:0x3
I (2376) wifi:state: init -> auth (0xb0)
[0;32mI (2388) clara_net: Wi-Fi protocols limited to 11b/g/n mask=0x07[0m
I (2388) wifi:Set ps type: 0, coexist: 0

I (2391) wifi:state: auth -> assoc (0x0)
[0;32mI (2395) clara_c6: Clara UI ready; touch the Clara tile[0m
[0;32mI (2401) main_task: Returned from app_main()[0m
I (2411) wifi:(assoc)RESP, Extended Capabilities length:11, operating_mode_notification:0
I (2413) wifi:(assoc)RESP, Extended Capabilities, MBSSID:0, TWT Responder:0, OBSS Narrow Bandwidth RU In OFDMA Tolerance:0
I (2424) wifi:Extended Capabilities length:11, operating_mode_notification:1
I (2431) wifi:BSS max idle period:291(1000TU), protected keep alive:FALSE
I (2437) wifi:state: assoc -> run (0x10)
I (2441) wifi:(trc)phytype:CBW20-SGI, snr:51, maxRate:144, highestRateIdx:0
W (2447) wifi:(trc)band:2G, phymode:3, highestRateIdx:0, lowestRateIdx:11, dataSchedTableSize:14
I (2456) wifi:(trc)band:2G, rate(S-MCS7, rateIdx:0), ampdu(rate:S-MCS7, schedIdx(0, stop:8)), snr:51, ampduState:wait operational
I (2467) wifi:ifidx:0, rssi:-35, nf:-86, phytype(0x3, CBW20-SGI), phymode(0x3, 11bgn), max_rate:144, he:0, vht:0, ht:1
I (2478) wifi:(ht)max.RxAMPDULenExponent:3(65535 bytes), MMSS:0(no restriction)
I (2494) wifi:connected with LDKJ, aid = 2, channel 6, BW20, bssid = 50:fe:39:e6:47:92
I (2495) wifi:security: WPA2-PSK, phy: bgn, rssi: -35, cipher(pairwise:0x3, group:0x3), pmf:0
I (2502) wifi:pm start, type: 0, twt_start:0

I (2504) wifi:pm start, type:0, aid:0x2, trans-BSSID:50:fe:39:e6:47:92, BSSID[5]:0x92, mbssid(max-indicator:0, index:0), he:0
I (2516) wifi:dp: 1, bi: 102400, li: 3, scale listen interval from 307200 us to 307200 us
I (2524) wifi:set rx beacon pti, rx_bcn_pti: 10, bcn_timeout: 25000, mt_pti: 10, mt_time: 10000
I (2532) wifi:AP's beacon interval = 102400 us, DTIM period = 1
[0;32mI (3566) esp_netif_handlers: sta ip: 192.168.31.147, mask: 255.255.255.0, gw: 192.168.31.1[0m
[0;32mI (3566) clara_net: DHCP DNS main=192.168.31.1[0m

```

- 记录结束: 2026-08-26 22:06:07

## 会议采集会话

- 开始时间: 2026-08-26 22:11:04
- 用户操作: 点击 Start 开始会议

```

```

- 结束时间: 2026-08-26 22:16:04

## 复位后启动日志

- 说明: 会议采集会话为空，执行硬复位后重新采集启动日志

```
[0;33mW (1554) sh8601: The 3Ah command has been used and will be overwritten by external initialization sequence[0m
[0;33mW (1554) sh8601: The 36h command has been used and will be overwritten by external initialization sequence[0m
[0;32mI (1738) CST9217: Checkcode: 0x204ECACA[0m
[0;32mI (1740) CST9217: Resolution X: 480, Y: 480[0m
[0;32mI (1742) CST9217: Chip Type: 0x9220, ProjectID: 0x542F[0m
[0;32mI (1742) esp_lvgl:adapter: LVGL adapter initialized successfully[0m
[0;32mI (1752) esp_lvgl:touch: Touch input device registered successfully (IRQ mode: enabled)[0m
[0;32mI (1760) esp_lvgl:adapter: LVGL task started successfully[0m
i2c: {sda: 8, scl: 7}
i2s: {mclk: 19, bclk: 20, ws: 22, din: 21, dout: 23}
out: {codec: ES8311, pa: -1, pa_gain: 6, use_mclk: 1, pa_gain: 6}
Codec 0 dir 2 type:1
in: {codec: ES7210}
Codec 1 dir 1 type:2
[0;32mI (1782) CODEC_INIT: in:1 out:1 port: 1[0m
[0;32mI (1786) CODEC_INIT: Success to int i2c: 0[0m
[0;32mI (1792) CODEC_INIT: Init i2s 0 type: 3 mclk:19 bclk:20 ws:22 din:21 dout:23[0m
[0;32mI (1799) CODEC_INIT: tx:0x4087e71c rx:0x4087e8d8[0m
[0;33mW (1804) i2s_tdm: the current mclk multiple is too small, adjust the mclk multiple to 384[0m
[0;32mI (1813) CODEC_INIT: output init tdm ret 0[0m
[0;33mW (1817) i2s_common: the rx channel on I2S0 is switched from master to slave for full-duplex mode[0m
[0;33mW (1827) i2s_tdm: the current mclk multiple is too small, adjust the mclk multiple to 384[0m
[0;33mW (1836) i2s_tdm: the mclk/bclk ratio 3 is too small for the full-duplex rx slave (min 4), data might be sampled incorrectly, please increase mclk_multiple[0m
[0;32mI (1851) CODEC_INIT: Input init tdm ret 0[0m
[0;32mI (1855) CODEC_INIT: Init i2s 0 ok[0m
[0;32mI (1859) CODEC_INIT: Success to init i2s: 0[0m
[0;32mI (1864) CODEC_INIT: Success to int i2c: 0[0m
[0;32mI (1869) CODEC_INIT: Success to init i2s: 0[0m
[0;32mI (1874) CODEC_INIT: Get out handle 0x4087e71c port 0[0m
[0;32mI (1885) ES8311: Work in Slave mode[0m
[0;32mI (1890) ES7210: Work in Slave mode[0m
[0;32mI (1897) ES7210: Enable ES7210_INPUT_MIC1[0m
[0;32mI (1899) ES7210: Enable ES7210_INPUT_MIC2[0m
[0;32mI (1902) ES7210: Enable ES7210_INPUT_MIC3[0m
[0;32mI (1905) ES7210: Enable ES7210_INPUT_MIC4[0m
[0;32mI (1909) ES7210: Enable TDM mode[0m
[0;32mI (1918) I2S_IF: channel mode 2 bits:16/16 channel:2 mask:3[0m
[0;32mI (1918) I2S_IF: TDM Mode 1 bits:16/16 channel:2 sample_rate:16000 mask:3[0m
[0;32mI (1939) Adev_Codec: Open codec device OK[0m
[0;32mI (1939) I2S_IF: channel mode 2 bits:16/16 channel:2 mask:3[0m
[0;32mI (1939) I2S_IF: TDM Mode 0 bits:16/16 channel:2 sample_rate:16000 mask:3[0m
[0;32mI (1947) ES7210: Bits 8[0m
[0;32mI (1956) ES7210: Enable ES7210_INPUT_MIC1[0m
[0;32mI (1959) ES7210: Enable ES7210_INPUT_MIC2[0m
[0;32mI (1961) ES7210: Enable ES7210_INPUT_MIC3[0m
[0;32mI (1966) ES7210: Enable ES7210_INPUT_MIC4[0m
[0;32mI (1971) ES7210: Enable TDM mode[0m
[0;32mI (1977) ES7210: Unmuted[0m
[0;32mI (1977) Adev_Codec: Open codec device OK[0m
[0;32mI (1980) Adev_Codec: Input already open[0m
[0;32mI (1984) Adev_Codec: Input already open[0m
[0;32mI (2174) pp: pp rom version: 5b8dcfa[0m
[0;32mI (2175) net80211: net80211 rom version: 5b8dcfa[0m
I (2176) wifi:wifi driver task: 40868e1c, prio:23, stack:6656, core=0
I (2185) wifi:wifi firmware version: b9f67df
I (2185) wifi:wifi certification version: v7.0
I (2187) wifi:config NVS flash: enabled
I (2191) wifi:config nano formatting: disabled
I (2195) wifi:mac_version:HAL_MAC_ESP32AX_761,ut_version:N, band mode:0x1
I (2202) wifi:Init data frame dynamic rx buffer num: 16
I (2206) wifi:Init static rx mgmt buffer num: 5
I (2210) wifi:Init management short buffer num: 32
I (2215) wifi:Init static tx buffer num: 16
I (2219) wifi:Init static tx FG buffer num: 2
I (2223) wifi:Init static rx buffer size: 1700 (rxctrl:92, csi:512)
I (2229) wifi:Init static rx buffer num: 10
I (2233) wifi:Init dynamic rx buffer num: 16
[0;32mI (2238) wifi_init: accept mbox: 6[0m
[0;32mI (2241) wifi_init: tcpip mbox: 32[0m
[0;32mI (2245) wifi_init: udp mbox: 6[0m
[0;32mI (2249) wifi_init: tcp mbox: 6[0m
[0;32mI (2253) wifi_init: tcp tx win: 5760[0m
[0;32mI (2257) wifi_init: tcp rx win: 5760[0m
[0;32mI (2261) wifi_init: tcp mss: 1240[0m
[0;32mI (2265) wifi_init: WiFi IRAM OP enabled[0m
[0;32mI (2270) wifi_init: WiFi RX IRAM OP enabled[0m
[0;32mI (2275) wifi_init: WiFi SLP IRAM OP enabled[0m
W (2280) wifi:Password length matches WPA2 standards, authmode threshold changes from OPEN to WPA2
[0;32mI (2289) phy_init: phy_version 345,552b044,Jul 14 2026,10:37:00[0m
I (2356) wifi:11ax coex: WDEVAX_PTI0(0x55777555), WDEVAX_PTI1(0x00003377).

I (2356) wifi:mode : sta (20:6e:f1:16:e0:a4)
I (2357) wifi:enable tsf
I (2371) wifi:new:<6,0>, old:<1,0>, ap:<255,255>, sta:<6,0>, prof:1, snd_ch_cfg:0x0
I (2372) wifi:(connect)dot11_authmode:0x3, pairwise_cipher:0x3, group_cipher:0x3
I (2374) wifi:state: init -> auth (0xb0)
[0;32mI (2386) clara_net: Wi-Fi protocols limited to 11b/g/n mask=0x07[0m
I (2386) wifi:Set ps type: 0, coexist: 0

I (2388) wifi:state: auth -> assoc (0x0)
[0;32mI (2393) clara_c6: Clara UI ready; touch the Clara tile[0m
[0;32mI (2399) main_task: Returned from app_main()[0m
I (2404) wifi:(assoc)RESP, Extended Capabilities length:11, operating_mode_notification:0
I (2411) wifi:(assoc)RESP, Extended Capabilities, MBSSID:0, TWT Responder:0, OBSS Narrow Bandwidth RU In OFDMA Tolerance:0
I (2422) wifi:Extended Capabilities length:11, operating_mode_notification:1
I (2429) wifi:BSS max idle period:291(1000TU), protected keep alive:FALSE
I (2435) wifi:state: assoc -> run (0x10)
I (2439) wifi:(trc)phytype:CBW20-SGI, snr:49, maxRate:144, highestRateIdx:0
W (2445) wifi:(trc)band:2G, phymode:3, highestRateIdx:0, lowestRateIdx:11, dataSchedTableSize:14
I (2454) wifi:(trc)band:2G, rate(S-MCS7, rateIdx:0), ampdu(rate:S-MCS7, schedIdx(0, stop:8)), snr:49, ampduState:wait operational
I (2465) wifi:ifidx:0, rssi:-37, nf:-86, phytype(0x3, CBW20-SGI), phymode(0x3, 11bgn), max_rate:144, he:0, vht:0, ht:1
I (2476) wifi:(ht)max.RxAMPDULenExponent:3(65535 bytes), MMSS:0(no restriction)
I (2503) wifi:connected with LDKJ, aid = 3, channel 6, BW20, bssid = 50:fe:39:e6:47:92
I (2503) wifi:security: WPA2-PSK, phy: bgn, rssi: -37, cipher(pairwise:0x3, group:0x3), pmf:0
I (2509) wifi:pm start, type: 0, twt_start:0

I (2512) wifi:pm start, type:0, aid:0x3, trans-BSSID:50:fe:39:e6:47:92, BSSID[5]:0x92, mbssid(max-indicator:0, index:0), he:0
I (2523) wifi:dp: 1, bi: 102400, li: 3, scale listen interval from 307200 us to 307200 us
I (2531) wifi:set rx beacon pti, rx_bcn_pti: 10, bcn_timeout: 25000, mt_pti: 10, mt_time: 10000
I (2598) wifi:AP's beacon interval = 102400 us, DTIM period = 1
[0;32mI (3568) esp_netif_handlers: sta ip: 192.168.31.147, mask: 255.255.255.0, gw: 192.168.31.1[0m
[0;32mI (3568) clara_net: DHCP DNS main=192.168.31.1[0m

```


## 会议采集会话 2

- 开始时间: 2026-08-26 22:18:34
- 用户操作: 请在此窗口内点击 Start 开始会议

```

```

- 结束时间: 2026-08-26 22:23:34

## 用户实测会话

- 开始时间: 2026-08-26 22:42:36
- 用户操作: 正在进行会议测试

```

```

- 结束时间: 2026-08-26 22:52:36
