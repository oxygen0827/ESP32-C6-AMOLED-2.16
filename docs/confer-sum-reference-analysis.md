# confer-sum 参考仓库对照分析（2026-08-27）

## 三个工程的关系

| 仓库 | 角色 | 状态 |
| --- | --- | --- |
| `confer-sum/clare-voice-api` | 后端（FastAPI，uvicorn 单 worker，8000 端口，Cloudflare 前置） | 生产：clare.vinex.top/voice-api |
| `confer-sum/vocat/products/ws_meeting_demo` | ESP32-S3 喵伴板参考实现（transcribe/host WS + VAD + MP3 TTS 播放 + 会议 UI） | 已部署验证 |
| 本仓库 `03_Clara_C6` | ESP32-C6 移植（LVGL9 + 固定 CA + boot self-test） | 板端链路已通，待后端稳定 |

## 后端协议要点（API_DOC v2.1，与 C6 当前实现对齐）

- 输入：16k/mono/16bit PCM；转写通道收原始 binary frame，JSON 通道 Base64。
- 输出：`answer_audio` 为 Base64 MP3 24kHz **按句分片、必须按序播放**——
  S3 版用 minimp3 + ring buffer 闭环；C6 的 HOST_ANSWER_AUDIO 播放路径
  尚未用真实音频验证。
- Session 状态机 active→ending→finalizing→ended；WS 错误码 4004/4009/4500
  （4009=同 session 已有 transcribe 发布者，重连时要处理）。
- understanding 建议轮询 3–5s；`snapshotRevision` 不变可跳过渲染。

## 服务端间歇故障定位（本日新增证据）

1. `/health`（最轻接口）5 次测试 1 次挂 8s+，其余 ~1.7s —— 单 worker
   事件循环间歇被占满或主机资源抖动，而非某条业务路径独有。
2. 生产代码无同步阻塞调用（LLM/TTS 均为 async httpx，无 time.sleep），
   代码层面干净；嫌疑收敛到主机 CPU/内存/容器限额或同机其他负载。
3. CF 522 与"客户端 TLS 通但无响应直至 ~15–30s 超时"是同一故障的两种表现：
   边缘终止客户端 TLS 后等待回源，源站不接。
4. transcribe_ws.accept() 在 ASR 连接前执行，因此"挂死"不可能是该
   endpoint 自身；ASR 失败路径是 close(4500)，可观测。

## C6 后续可借鉴 S3 版的三点

1. `pipeline_ws.c` 的 20ms 帧（640B）录音/放音管线与环形缓冲。
2. MP3 按句播放队列（answer_audio 排序 + minimp3 流式解码）。
3. 断线重连与 4009 发布者冲突处理策略（对照 C6 当前的 disable_auto_reconnect）。

## 排障脚本沉淀（本仓库）

- `scripts/backend_api_probe.py`：后端协议探针（session/WSS 全流程）。
- `scripts/clara_wss_sim.py`：板端配置复刻（固定 CA 三信任对照 + 实发证书指纹），
  用于快速裁定板端 vs 服务端责任。
