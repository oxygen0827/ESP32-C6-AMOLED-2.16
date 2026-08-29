// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#include "transcribe_ws.h"
#include "pipeline_ws.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "transcribe_ws";

// 100ms frame at 16kHz 16bit mono
#define FEED_FRAME_BYTES  3200
#define B64_OUT_SIZE      ((FEED_FRAME_BYTES * 4 / 3) + 8)
#define JSON_BUF_SIZE     (B64_OUT_SIZE + 32)

static esp_websocket_client_handle_t s_ws = NULL;
static volatile bool s_feed_run           = false;
static TaskHandle_t  s_feed_task_handle   = NULL;
// Serializes audio sends with stop/destroy. Without this guard a reconnect can
// free the TLS transport after is_connected() but before send_text(), causing
// an mbedTLS LoadProhibited panic.
static SemaphoreHandle_t s_ws_guard       = NULL;
static void feed_task(void *arg);

static esp_err_t ensure_ws_guard(void)
{
    if (s_ws_guard) return ESP_OK;
    s_ws_guard = xSemaphoreCreateMutex();
    return s_ws_guard ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t start_feed(void)
{
    if (ensure_ws_guard() != ESP_OK) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_ws_guard, portMAX_DELAY);
    bool connected = s_ws && esp_websocket_client_is_connected(s_ws);
    xSemaphoreGive(s_ws_guard);
    if (!connected) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_feed_task_handle) return ESP_OK;
    esp_err_t recorder_err = pipeline_ws_recorder_open();
    if (recorder_err != ESP_OK) return recorder_err;
    s_feed_run = true;
    BaseType_t task_ok = xTaskCreatePinnedToCoreWithCaps(
        feed_task, "transcribe_feed", 12 * 1024, NULL, 5,
        &s_feed_task_handle, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ok != pdPASS) {
        s_feed_run = false;
        pipeline_ws_recorder_close();
        ESP_LOGE(TAG, "[FAIL] feed task creation failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void build_ws_uri(char *out, size_t len, const char *path, const char *session_id)
{
    // Replace "https://" prefix with "wss://"
    const char *base = CONFIG_WS_MEETING_API_BASE_URL;
    const char *host = strstr(base, "://");
    host = host ? host + 3 : base;
    snprintf(out, len, "wss://%s%s/%s", host, path, session_id);
}

static void feed_task(void *arg)
{
    ESP_LOGI(TAG, "[OK] feed task started");

    static uint8_t       pcm_buf[FEED_FRAME_BYTES];
    static unsigned char b64_buf[B64_OUT_SIZE];
    static char          json_buf[JSON_BUF_SIZE];
    uint32_t frame_count = 0;
    uint64_t level_abs_sum = 0;
    uint32_t level_samples = 0;
    uint16_t level_peak = 0;

    while (s_feed_run) {
        int got = pipeline_ws_recorder_read(pcm_buf, FEED_FRAME_BYTES);
        if (got != FEED_FRAME_BYTES) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }

        const int16_t *samples = (const int16_t *)pcm_buf;
        for (size_t i = 0; i < FEED_FRAME_BYTES / sizeof(int16_t); ++i) {
            int32_t sample = samples[i];
            uint16_t magnitude = (uint16_t)(sample < 0 ? -sample : sample);
            level_abs_sum += magnitude;
            level_samples++;
            if (magnitude > level_peak) level_peak = magnitude;
        }
        size_t b64_len = 0;
        mbedtls_base64_encode(b64_buf, sizeof(b64_buf), &b64_len,
                              pcm_buf, FEED_FRAME_BYTES);
        b64_buf[b64_len] = '\0';

        int jlen = snprintf(json_buf, sizeof(json_buf),
                            "{\"type\":\"audio\",\"data\":\"%s\"}", b64_buf);
        xSemaphoreTake(s_ws_guard, portMAX_DELAY);
        esp_websocket_client_handle_t ws = s_ws;
        int sent = -1;
        if (ws && esp_websocket_client_is_connected(ws)) {
            sent = esp_websocket_client_send_text(ws, json_buf, jlen,
                                                   pdMS_TO_TICKS(500));
        }
        xSemaphoreGive(s_ws_guard);
        if (sent < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        frame_count++;
        if (frame_count % 100 == 0) {
            uint32_t mean_abs = level_samples
                ? (uint32_t)(level_abs_sum / level_samples) : 0;
            ESP_LOGI(TAG, "sent #%lu frames total, mic_mean_abs=%lu peak=%u",
                     (unsigned long)frame_count, (unsigned long)mean_abs,
                     (unsigned)level_peak);
            level_abs_sum = 0;
            level_samples = 0;
            level_peak = 0;
        }
    }

    ESP_LOGI(TAG, "[OK] feed task stopped, heap_free=%lu", esp_get_free_heap_size());
    s_feed_task_handle = NULL;
    vTaskDelete(NULL);
}

static void ws_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    (void)arg; (void)base; (void)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "[OK] WS connected");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WS disconnected");
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WS error");
        break;
    default: break;
    }
}

esp_err_t transcribe_ws_connect(const char *session_id)
{
    esp_err_t guard_err = ensure_ws_guard();
    if (guard_err != ESP_OK) return guard_err;
    char uri[256];
    build_ws_uri(uri, sizeof(uri), "/ws/transcribe", session_id);
    ESP_LOGI(TAG, "connecting %s", uri);

    esp_websocket_client_config_t cfg = {
        .uri                         = uri,
        .buffer_size                 = 16384,
        .task_stack                  = 12288,   // 12KB: TLS ops (mbedtls_ssl_read/write) need ~4KB stack
        .task_prio                   = 5,
        .skip_cert_common_name_check = false,
        // Reconnection is owned by ws_session's supervisor. Running the
        // component's reconnect worker at the same time as that supervisor
        // races TLS writes with transport replacement.
        .disable_auto_reconnect      = true,
        .network_timeout_ms          = 5000,
    };
    esp_websocket_client_handle_t ws = esp_websocket_client_init(&cfg);
    if (!ws) {
        ESP_LOGE(TAG, "[FAIL] websocket client init failed");
        return ESP_ERR_NO_MEM;
    }
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_err_t start_err = esp_websocket_client_start(ws);
    if (start_err != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] websocket start failed: %s", esp_err_to_name(start_err));
        esp_websocket_client_destroy(ws);
        return start_err;
    }

    // Wait up to 15s for connection (TLS handshake can take >5s)
    for (int i = 0; i < 150; i++) {
        if (esp_websocket_client_is_connected(ws)) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!esp_websocket_client_is_connected(ws)) {
        ESP_LOGE(TAG, "[FAIL] connect timeout");
        esp_websocket_client_stop(ws);
        esp_websocket_client_destroy(ws);
        return ESP_FAIL;
    }
    xSemaphoreTake(s_ws_guard, portMAX_DELAY);
    s_ws = ws;
    xSemaphoreGive(s_ws_guard);
    ESP_LOGI(TAG, "[OK] connected wss://.../ws/transcribe/%s", session_id);

    esp_err_t feed_err = start_feed();
    if (feed_err != ESP_OK) {
        xSemaphoreTake(s_ws_guard, portMAX_DELAY);
        s_ws = NULL;
        xSemaphoreGive(s_ws_guard);
        esp_websocket_client_stop(ws);
        esp_websocket_client_destroy(ws);
        return feed_err;
    }
    return ESP_OK;
}

esp_err_t transcribe_ws_pause(void)
{
    s_feed_run = false;
    // The client network timeout is 5s. Wait longer than that so a blocked TLS
    // write has definitely returned before the recorder or transport is freed.
    for (int i = 0; i < 80 && s_feed_task_handle != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_feed_task_handle != NULL) {
        ESP_LOGE(TAG, "[FAIL] feed task did not stop; keeping transport alive");
        return ESP_ERR_TIMEOUT;
    }
    pipeline_ws_recorder_close();
    ESP_LOGI(TAG, "[OK] paused; transcription WS kept warm");
    return ESP_OK;
}

esp_err_t transcribe_ws_resume(void)
{
    esp_err_t err = start_feed();
    if (err == ESP_OK) ESP_LOGI(TAG, "[OK] resumed warm transcription WS");
    return err;
}

esp_err_t transcribe_ws_send_end(void)
{
    if (ensure_ws_guard() != ESP_OK) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_ws_guard, portMAX_DELAY);
    esp_websocket_client_handle_t ws = s_ws;
    if (!ws || !esp_websocket_client_is_connected(ws)) {
        xSemaphoreGive(s_ws_guard);
        return ESP_ERR_INVALID_STATE;
    }
    const char *msg = "{\"type\":\"end\"}";
    int sent = esp_websocket_client_send_text(ws, msg, (int)strlen(msg),
                                               pdMS_TO_TICKS(1000));
    xSemaphoreGive(s_ws_guard);
    if (sent < 0) return ESP_FAIL;
    ESP_LOGI(TAG, "[OK] sent end signal");
    return ESP_OK;
}

esp_err_t transcribe_ws_disconnect(void)
{
    esp_err_t pause_err = transcribe_ws_pause();
    if (pause_err != ESP_OK) return pause_err;
    if (ensure_ws_guard() != ESP_OK) return ESP_ERR_NO_MEM;
    // Null out s_ws so feed_task won't use a destroyed handle
    xSemaphoreTake(s_ws_guard, portMAX_DELAY);
    esp_websocket_client_handle_t ws = s_ws;
    s_ws = NULL;
    if (ws) {
        esp_websocket_client_stop(ws);
        esp_websocket_client_destroy(ws);
    }
    xSemaphoreGive(s_ws_guard);
    ESP_LOGI(TAG, "[OK] disconnected");
    return ESP_OK;
}

bool transcribe_ws_is_connected(void)
{
    if (ensure_ws_guard() != ESP_OK) return false;
    xSemaphoreTake(s_ws_guard, portMAX_DELAY);
    bool connected = s_ws && esp_websocket_client_is_connected(s_ws);
    xSemaphoreGive(s_ws_guard);
    return connected;
}
