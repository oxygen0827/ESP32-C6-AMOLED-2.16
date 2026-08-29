// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "esp_err.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

// Connect wss://.../ws/transcribe/{session_id}, open recorder, start feed task
esp_err_t transcribe_ws_connect(const char *session_id);

// Keep the TLS/WebSocket session alive while Host temporarily owns the mic.
esp_err_t transcribe_ws_pause(void);
esp_err_t transcribe_ws_resume(void);

// Send {"type":"end"} to signal end of meeting recording
esp_err_t transcribe_ws_send_end(void);

// Stop feed task, close recorder, disconnect WS
esp_err_t transcribe_ws_disconnect(void);

// True only while the underlying TLS/WebSocket transport is established.
bool transcribe_ws_is_connected(void);

#ifdef __cplusplus
}
#endif
