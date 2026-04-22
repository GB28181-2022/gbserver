# ZLMediaKit（流媒体）配置

- **`config.ini`**：从本机 ZLMediaKit `release/linux/Debug/config.ini` 归档，便于与国标服务（`nginx/`、`8080` hook 等）对照部署。
- 部署前请将 **`[api] secret`** 改为生产值，并与调用 ZLM HTTP API 的组件配置一致。
- 相对源文件已加强日志：**`enable_ffmpeg_log=1`**（FFmpeg 侧），**`apiDebug=1`**（HTTP API 请求日志）。
- **`[hook]`**：请将 `on_stream_changed`、`on_stream_none_reader`、`on_rtp_server_timeout` 指向 **gb_service** 的 HTTP 地址（默认 `http://127.0.0.1:8080/api/zlm/hook/...`）。**收流超时**（`openRtpServer` 长期无 RTP）发 SIP BYE 依赖 **`on_rtp_server_timeout`**；修改后需重启或按 ZLM 文档重载配置。
