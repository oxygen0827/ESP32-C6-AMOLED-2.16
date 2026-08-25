// Clara network transport for ESP32-C6.
//
// The implementation intentionally keeps all credentials and meeting data out
// of logs. HTTP uses the ESP-IDF client with the certificate bundle; the
// transcription channel sends binary PCM and the host channel uses the
// documented Base64 JSON messages.

#include "clara_net.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#ifndef CONFIG_CLARA_WIFI_SSID
#define CONFIG_CLARA_WIFI_SSID ""
#endif
#ifndef CONFIG_CLARA_WIFI_PASSWORD
#define CONFIG_CLARA_WIFI_PASSWORD ""
#endif
#ifndef CONFIG_CLARA_API_BASE_URL
#define CONFIG_CLARA_API_BASE_URL ""
#endif
#ifndef CONFIG_CLARA_TOPIC
#define CONFIG_CLARA_TOPIC ""
#endif

namespace {

static const char *TAG = "clara_net";

constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;
constexpr EventBits_t WIFI_FAILED_BIT = BIT1;
constexpr int WIFI_MAX_RETRIES = 8;
constexpr size_t SESSION_ID_MAX = 96;
constexpr size_t URL_MAX = 384;
constexpr size_t HTTP_RESPONSE_MAX = 16384;
constexpr size_t WS_RX_MAX = 16384;
constexpr size_t HOST_AUDIO_MAX = 12000;

enum class WsKind : uint8_t {
    Transcribe,
    Host,
};

struct WsContext {
    WsKind kind;
    esp_websocket_client_handle_t client;
    EventGroupHandle_t events;
    char *rx_buf;
    size_t rx_len;
    size_t rx_expected;
    volatile bool connected;
};

struct HttpResponse {
    char *buf;
    size_t cap;
    size_t len;
    bool truncated;
};

static SemaphoreHandle_t s_lock = nullptr;
static EventGroupHandle_t s_wifi_events = nullptr;
static esp_netif_t *s_sta_netif = nullptr;
static esp_event_handler_instance_t s_wifi_handler = nullptr;
static esp_event_handler_instance_t s_ip_handler = nullptr;
static bool s_initialized = false;
static bool s_wifi_started = false;
static volatile bool s_wifi_connected = false;
static int s_wifi_retries = 0;
static clara_net_event_cb_t s_event_cb = nullptr;
static void *s_event_ctx = nullptr;
static char s_session_id[SESSION_ID_MAX] = {};
static WsContext s_transcribe = {WsKind::Transcribe, nullptr, nullptr, nullptr, 0, 0, false};
static WsContext s_host = {WsKind::Host, nullptr, nullptr, nullptr, 0, 0, false};

static bool take_lock(TickType_t timeout = pdMS_TO_TICKS(1000))
{
    return s_lock != nullptr && xSemaphoreTake(s_lock, timeout) == pdTRUE;
}

static void give_lock()
{
    if (s_lock != nullptr) {
        xSemaphoreGive(s_lock);
    }
}

static void emit_event(clara_net_event_type_t type,
                       const char *text = nullptr,
                       const uint8_t *binary = nullptr,
                       size_t binary_len = 0,
                       bool is_final = false,
                       int status_code = 0,
                       esp_err_t error = ESP_OK)
{
    clara_net_event_cb_t cb = nullptr;
    void *ctx = nullptr;
    if (take_lock()) {
        cb = s_event_cb;
        ctx = s_event_ctx;
        give_lock();
    }
    if (!cb) {
        return;
    }
    clara_net_event_t event = {
        .type = type,
        .text = text,
        .binary = binary,
        .binary_len = binary_len,
        .is_final = is_final,
        .status_code = status_code,
        .error = error,
    };
    cb(&event, ctx);
}

static bool valid_session_id(const char *id)
{
    if (!id || !id[0]) {
        return false;
    }
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(id); *p; ++p) {
        if (!(isalnum(*p) || *p == '-' || *p == '_' || *p == '.')) {
            return false;
        }
    }
    return true;
}

static bool get_base_url(char *out, size_t out_len)
{
    const char *base = CONFIG_CLARA_API_BASE_URL;
    if (!base || !base[0] || !out || out_len < 2) {
        return false;
    }
    size_t n = strlen(base);
    while (n > 0 && base[n - 1] == '/') {
        --n;
    }
    if (n == 0 || n >= out_len) {
        return false;
    }
    memcpy(out, base, n);
    out[n] = '\0';
    return true;
}

static bool make_http_url(char *out, size_t out_len, const char *path)
{
    char base[URL_MAX];
    if (!path || !get_base_url(base, sizeof(base))) {
        return false;
    }
    int written = snprintf(out, out_len, "%s/%s", base, path[0] == '/' ? path + 1 : path);
    return written > 0 && static_cast<size_t>(written) < out_len;
}

static bool make_ws_url(char *out, size_t out_len, const char *path)
{
    char base[URL_MAX];
    if (!path || !get_base_url(base, sizeof(base))) {
        return false;
    }
    const char *scheme = nullptr;
    const char *rest = nullptr;
    if (strncmp(base, "https://", 8) == 0) {
        scheme = "wss://";
        rest = base + 8;
    } else if (strncmp(base, "http://", 7) == 0) {
        scheme = "ws://";
        rest = base + 7;
    } else if (strncmp(base, "wss://", 6) == 0) {
        scheme = "wss://";
        rest = base + 6;
    } else if (strncmp(base, "ws://", 5) == 0) {
        scheme = "ws://";
        rest = base + 5;
    } else {
        return false;
    }
    int written = snprintf(out, out_len, "%s%s/%s", scheme, rest,
                           path[0] == '/' ? path + 1 : path);
    return written > 0 && static_cast<size_t>(written) < out_len;
}

static void report_error(esp_err_t err, int status_code = 0)
{
    emit_event(CLARA_NET_EVENT_ERROR, nullptr, nullptr, 0, false, status_code, err);
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (!event || !event->user_data) {
        return ESP_OK;
    }
    HttpResponse *response = static_cast<HttpResponse *>(event->user_data);
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->data || event->data_len <= 0) {
        return ESP_OK;
    }
    if (response->len >= response->cap - 1) {
        response->truncated = true;
        return ESP_OK;
    }
    size_t remaining = response->cap - response->len - 1;
    size_t copy_len = static_cast<size_t>(event->data_len) < remaining
                          ? static_cast<size_t>(event->data_len)
                          : remaining;
    memcpy(response->buf + response->len, event->data, copy_len);
    response->len += copy_len;
    response->buf[response->len] = '\0';
    if (copy_len < static_cast<size_t>(event->data_len)) {
        response->truncated = true;
    }
    return ESP_OK;
}

static void fill_http_config(esp_http_client_config_t *config,
                             const char *url,
                             esp_http_client_method_t method,
                             HttpResponse *response)
{
    *config = {};
    config->url = url;
    config->method = method;
    config->timeout_ms = 30000;
    config->buffer_size = 4096;
    config->buffer_size_tx = 2048;
    config->event_handler = http_event_handler;
    config->user_data = response;
    config->skip_cert_common_name_check = false;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config->crt_bundle_attach = esp_crt_bundle_attach;
#endif
}

static esp_err_t http_perform(const char *url,
                              esp_http_client_method_t method,
                              const char *content_type,
                              const char *body,
                              char *response_buf,
                              size_t response_buf_len,
                              int *status_code,
                              bool *response_truncated)
{
    if (!url || !response_buf || response_buf_len < 2 || !status_code) {
        return ESP_ERR_INVALID_ARG;
    }
    HttpResponse response = {
        .buf = response_buf,
        .cap = response_buf_len,
        .len = 0,
        .truncated = false,
    };
    response_buf[0] = '\0';
    esp_http_client_config_t config;
    fill_http_config(&config, url, method, &response);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    if (content_type) {
        err = esp_http_client_set_header(client, "Content-Type", content_type);
    }
    if (err == ESP_OK && body) {
        err = esp_http_client_set_post_field(client, body, static_cast<int>(strlen(body)));
    }
    if (err == ESP_OK) {
        err = esp_http_client_perform(client);
    }
    *status_code = esp_http_client_get_status_code(client);
    int tls_error = 0;
    int tls_flags = 0;
    if (err != ESP_OK) {
        (void)esp_http_client_get_and_clear_last_tls_error(client, &tls_error, &tls_flags);
        // Keep diagnostics numeric; never print a response body or credential.
        ESP_LOGW(TAG, "HTTP request failed err=%d status=%d tls=%d flags=0x%x",
                 static_cast<int>(err), *status_code, tls_error, tls_flags);
    }
    esp_http_client_cleanup(client);
    if (response_truncated) {
        *response_truncated = response.truncated;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (*status_code < 200 || *status_code >= 300) {
        ESP_LOGW(TAG, "HTTP response status=%d", *status_code);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void wifi_event_handler(void *, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_wifi_retries = 0;
        if (s_wifi_events) {
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
        }
        (void)esp_wifi_connect();
        emit_event(CLARA_NET_EVENT_WIFI_CONNECTING);
        return;
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto *disconnect = static_cast<const wifi_event_sta_disconnected_t *>(event_data);
        s_wifi_connected = false;
        if (s_wifi_events) {
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
        if (s_wifi_started && s_wifi_retries < WIFI_MAX_RETRIES) {
            ++s_wifi_retries;
            (void)esp_wifi_connect();
        } else if (s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
        }
        ESP_LOGW(TAG, "Wi-Fi disconnected reason=%d retry=%d",
                 disconnect ? static_cast<int>(disconnect->reason) : -1, s_wifi_retries);
        emit_event(CLARA_NET_EVENT_WIFI_DISCONNECTED);
        return;
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_retries = 0;
        s_wifi_connected = true;
        if (s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
        emit_event(CLARA_NET_EVENT_WIFI_CONNECTED);
    }
}

static esp_err_t ensure_netif(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (!s_sta_netif) {
            s_sta_netif = esp_netif_create_default_wifi_sta();
        }
    }
    return s_sta_netif ? ESP_OK : ESP_FAIL;
}

static void reset_ws_rx(WsContext *ctx)
{
    if (!ctx) {
        return;
    }
    free(ctx->rx_buf);
    ctx->rx_buf = nullptr;
    ctx->rx_len = 0;
    ctx->rx_expected = 0;
}

static void emit_ws_state(WsContext *ctx, clara_net_event_type_t type)
{
    emit_event(type);
    if (ctx && ctx->events) {
        if (type == CLARA_NET_EVENT_TRANSCRIBE_CONNECTED ||
            type == CLARA_NET_EVENT_HOST_CONNECTED) {
            xEventGroupSetBits(ctx->events, BIT0);
        } else if (type == CLARA_NET_EVENT_TRANSCRIBE_DISCONNECTED ||
                   type == CLARA_NET_EVENT_HOST_DISCONNECTED) {
            xEventGroupSetBits(ctx->events, BIT1);
        }
    }
}

static void dispatch_ws_json(WsContext *ctx, const char *payload, size_t payload_len)
{
    if (!ctx || !payload || payload_len == 0) {
        return;
    }
    cJSON *root = cJSON_ParseWithLength(payload, payload_len);
    if (!root) {
        report_error(ESP_ERR_INVALID_RESPONSE);
        return;
    }
    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *type = cJSON_IsString(type_item) ? type_item->valuestring : nullptr;
    if (!type) {
        cJSON_Delete(root);
        return;
    }
    if (ctx->kind == WsKind::Transcribe &&
        (strcmp(type, "transcript") == 0 || strcmp(type, "transcription") == 0)) {
        cJSON *text_item = cJSON_GetObjectItemCaseSensitive(root, "text");
        cJSON *final_item = cJSON_GetObjectItemCaseSensitive(root, "is_final");
        const char *text = cJSON_IsString(text_item) ? text_item->valuestring : "";
        emit_event(CLARA_NET_EVENT_TRANSCRIPT, text, nullptr, 0,
                   cJSON_IsTrue(final_item));
    } else if (ctx->kind == WsKind::Host) {
        if (strcmp(type, "transcription") == 0) {
            cJSON *text_item = cJSON_GetObjectItemCaseSensitive(root, "text");
            const char *text = cJSON_IsString(text_item) ? text_item->valuestring : "";
            emit_event(CLARA_NET_EVENT_HOST_TRANSCRIPTION, text);
        } else if (strcmp(type, "answer_text") == 0) {
            cJSON *text_item = cJSON_GetObjectItemCaseSensitive(root, "text");
            cJSON *done_item = cJSON_GetObjectItemCaseSensitive(root, "done");
            const char *text = cJSON_IsString(text_item) ? text_item->valuestring : "";
            emit_event(CLARA_NET_EVENT_HOST_ANSWER_TEXT, text, nullptr, 0,
                       cJSON_IsTrue(done_item));
        } else if (strcmp(type, "answer_audio") == 0) {
            cJSON *data_item = cJSON_GetObjectItemCaseSensitive(root, "data");
            if (cJSON_IsString(data_item) && data_item->valuestring) {
                const char *encoded = data_item->valuestring;
                size_t encoded_len = strlen(encoded);
                size_t decoded_cap = encoded_len / 4U * 3U + 4U;
                uint8_t *decoded = static_cast<uint8_t *>(malloc(decoded_cap));
                if (decoded) {
                    size_t decoded_len = 0;
                    int rc = mbedtls_base64_decode(decoded, decoded_cap, &decoded_len,
                                                   reinterpret_cast<const unsigned char *>(encoded),
                                                   encoded_len);
                    if (rc == 0 && decoded_len > 0) {
                        emit_event(CLARA_NET_EVENT_HOST_ANSWER_AUDIO, nullptr,
                                   decoded, decoded_len);
                    } else {
                        report_error(ESP_ERR_INVALID_RESPONSE);
                    }
                    free(decoded);
                } else {
                    report_error(ESP_ERR_NO_MEM);
                }
            }
        } else if (strcmp(type, "done") == 0) {
            emit_event(CLARA_NET_EVENT_HOST_DONE);
        }
    } else if (strcmp(type, "error") == 0) {
        // The server message is passed to the UI callback but never logged.
        cJSON *message_item = cJSON_GetObjectItemCaseSensitive(root, "message");
        const char *message = cJSON_IsString(message_item) ? message_item->valuestring : "";
        emit_event(CLARA_NET_EVENT_ERROR, message, nullptr, 0, false, 0, ESP_FAIL);
    }
    cJSON_Delete(root);
}

static void ws_event_handler(void *arg, esp_event_base_t, int32_t event_id, void *event_data)
{
    WsContext *ctx = static_cast<WsContext *>(arg);
    if (!ctx) {
        return;
    }
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        ctx->connected = true;
        if (ctx->events) {
            xEventGroupClearBits(ctx->events, BIT1);
        }
        emit_ws_state(ctx, ctx->kind == WsKind::Transcribe
                              ? CLARA_NET_EVENT_TRANSCRIBE_CONNECTED
                              : CLARA_NET_EVENT_HOST_CONNECTED);
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_CLOSED) {
        ctx->connected = false;
        if (ctx->events) {
            xEventGroupSetBits(ctx->events, BIT1);
        }
        emit_ws_state(ctx, ctx->kind == WsKind::Transcribe
                              ? CLARA_NET_EVENT_TRANSCRIBE_DISCONNECTED
                              : CLARA_NET_EVENT_HOST_DISCONNECTED);
        return;
    }
    if (event_id == WEBSOCKET_EVENT_ERROR) {
        esp_err_t err = ESP_FAIL;
        int status = 0;
        if (event_data) {
            esp_websocket_event_data_t *data = static_cast<esp_websocket_event_data_t *>(event_data);
            err = data->error_handle.esp_tls_last_esp_err;
            if (err == ESP_OK) {
                err = ESP_FAIL;
            }
            status = data->error_handle.esp_ws_handshake_status_code;
        }
        if (ctx->events) {
            xEventGroupSetBits(ctx->events, BIT1);
        }
        report_error(err, status);
        return;
    }
    if (event_id != WEBSOCKET_EVENT_DATA || !event_data) {
        return;
    }
    esp_websocket_event_data_t *data = static_cast<esp_websocket_event_data_t *>(event_data);
    if (data->op_code != 0x1 || !data->data_ptr || data->data_len <= 0) {
        return;
    }
    size_t offset = data->payload_offset > 0 ? static_cast<size_t>(data->payload_offset) : 0;
    size_t frame_len = static_cast<size_t>(data->data_len);
    size_t expected = data->payload_len > 0 ? static_cast<size_t>(data->payload_len) : frame_len;
    if (offset == 0) {
        ctx->rx_len = 0;
        ctx->rx_expected = expected;
    }
    if (expected > WS_RX_MAX || offset > WS_RX_MAX || frame_len > WS_RX_MAX - offset) {
        reset_ws_rx(ctx);
        report_error(ESP_ERR_NO_MEM);
        return;
    }
    if (!ctx->rx_buf) {
        ctx->rx_buf = static_cast<char *>(malloc(WS_RX_MAX + 1));
        if (!ctx->rx_buf) {
            report_error(ESP_ERR_NO_MEM);
            return;
        }
    }
    memcpy(ctx->rx_buf + offset, data->data_ptr, frame_len);
    if (offset + frame_len > ctx->rx_len) {
        ctx->rx_len = offset + frame_len;
    }
    bool complete = data->fin && (ctx->rx_len >= ctx->rx_expected || ctx->rx_expected == 0);
    if (complete) {
        ctx->rx_buf[ctx->rx_len] = '\0';
        dispatch_ws_json(ctx, ctx->rx_buf, ctx->rx_len);
        ctx->rx_len = 0;
        ctx->rx_expected = 0;
    }
}

static esp_err_t ws_send_text(WsContext *ctx, const char *text)
{
    if (!ctx || !text) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_websocket_client_handle_t client = nullptr;
    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    client = ctx->client;
    bool connected = ctx->connected;
    give_lock();
    if (!client || !connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int len = static_cast<int>(strlen(text));
    int sent = esp_websocket_client_send_text(client, text, len, pdMS_TO_TICKS(1000));
    return sent == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t ws_send_binary(WsContext *ctx, const void *data, size_t len)
{
    if (!ctx || !data || len == 0 || len > INT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_websocket_client_handle_t client = nullptr;
    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    client = ctx->client;
    bool connected = ctx->connected;
    give_lock();
    if (!client || !connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int sent = esp_websocket_client_send_bin(client, static_cast<const char *>(data),
                                             static_cast<int>(len), pdMS_TO_TICKS(1000));
    return sent == static_cast<int>(len) ? ESP_OK : ESP_FAIL;
}

static esp_err_t ws_cleanup(WsContext *ctx)
{
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_websocket_client_handle_t client = nullptr;
    if (take_lock()) {
        client = ctx->client;
        ctx->client = nullptr;
        ctx->connected = false;
        give_lock();
    }
    if (client) {
        // stop/destroy are intentionally called outside the event callback.
        (void)esp_websocket_client_stop(client);
        (void)esp_websocket_client_destroy(client);
    }
    reset_ws_rx(ctx);
    if (ctx->events) {
        xEventGroupClearBits(ctx->events, BIT0 | BIT1);
    }
    return ESP_OK;
}

static esp_err_t ws_connect(WsContext *ctx, const char *session_id)
{
    if (!ctx || !valid_session_id(session_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || !s_wifi_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[160];
    int path_len = snprintf(path, sizeof(path), "%s/%s",
                            ctx->kind == WsKind::Transcribe ? "ws/transcribe" : "ws/host",
                            session_id);
    if (path_len <= 0 || static_cast<size_t>(path_len) >= sizeof(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    char uri[URL_MAX];
    if (!make_ws_url(uri, sizeof(uri), path)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ctx->events) {
        ctx->events = xEventGroupCreate();
        if (!ctx->events) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    if (ctx->client) {
        give_lock();
        return ESP_ERR_INVALID_STATE;
    }
    ctx->connected = false;
    xEventGroupClearBits(ctx->events, BIT0 | BIT1);
    give_lock();

    esp_websocket_client_config_t config = {};
    config.uri = uri;
    config.buffer_size = 16384;
    config.task_stack = 12288;
    config.task_prio = 5;
    config.network_timeout_ms = 30000;
    config.reconnect_timeout_ms = 10000;
    config.disable_auto_reconnect = true;
    config.skip_cert_common_name_check = false;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    esp_websocket_client_handle_t client = esp_websocket_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY,
                                                  ws_event_handler, ctx);
    if (err == ESP_OK && take_lock()) {
        ctx->client = client;
        give_lock();
    } else if (err == ESP_OK) {
        err = ESP_ERR_TIMEOUT;
    }
    if (err == ESP_OK) {
        err = esp_websocket_client_start(client);
    }
    if (err != ESP_OK) {
        if (take_lock()) {
            if (ctx->client == client) {
                ctx->client = nullptr;
            }
            give_lock();
        }
        (void)esp_websocket_client_destroy(client);
        report_error(err);
        return err;
    }
    EventBits_t bits = xEventGroupWaitBits(ctx->events, BIT0 | BIT1, pdFALSE,
                                           pdFALSE, pdMS_TO_TICKS(15000));
    if ((bits & BIT0) == 0) {
        ESP_LOGW(TAG, "WebSocket connect timeout kind=%d bits=0x%lx",
                 static_cast<int>(ctx->kind), static_cast<unsigned long>(bits));
        (void)ws_cleanup(ctx);
        return (bits & BIT1) ? ESP_FAIL : ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

} // namespace

extern "C" esp_err_t clara_net_init(const clara_net_config_t *config)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (take_lock()) {
        if (config) {
            s_event_cb = config->event_cb;
            s_event_ctx = config->ctx;
        }
        give_lock();
    }
    if (s_initialized) {
        return ESP_OK;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        return err;
    }
    err = ensure_netif();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_wifi_events) {
        s_wifi_events = xEventGroupCreate();
        if (!s_wifi_events) {
            return ESP_ERR_NO_MEM;
        }
    }
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) {
        return err;
    }
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, nullptr,
                                              &s_wifi_handler);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &wifi_event_handler, nullptr,
                                              &s_ip_handler);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_initialized = true;
    return ESP_OK;
}

extern "C" esp_err_t clara_net_wifi_start(void)
{
    esp_err_t err = clara_net_init(nullptr);
    if (err != ESP_OK) {
        return err;
    }
    if (CONFIG_CLARA_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "Wi-Fi SSID is not configured");
        emit_event(CLARA_NET_EVENT_WIFI_FAILED, nullptr, nullptr, 0, false, 0,
                   ESP_ERR_INVALID_ARG);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_wifi_started) {
        if (!s_wifi_connected) {
            s_wifi_retries = 0;
            if (s_wifi_events) {
                xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
            }
            (void)esp_wifi_connect();
            emit_event(CLARA_NET_EVENT_WIFI_CONNECTING);
        }
        return ESP_OK;
    }
    wifi_config_t wifi_cfg = {};
    strlcpy(reinterpret_cast<char *>(wifi_cfg.sta.ssid), CONFIG_CLARA_WIFI_SSID,
            sizeof(wifi_cfg.sta.ssid));
    strlcpy(reinterpret_cast<char *>(wifi_cfg.sta.password), CONFIG_CLARA_WIFI_PASSWORD,
            sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        return err;
    }
    s_wifi_started = true;
    s_wifi_retries = 0;
    s_wifi_connected = false;
    emit_event(CLARA_NET_EVENT_WIFI_CONNECTING);
    // WIFI_EVENT_STA_START owns the first connect call. Calling it here too
    // races the event callback and produces ESP_ERR_WIFI_CONN on boot.
    return ESP_OK;
}

extern "C" esp_err_t clara_net_wifi_connect(uint32_t timeout_ms)
{
    esp_err_t err = clara_net_wifi_start();
    if (err != ESP_OK) {
        return err;
    }
    if (s_wifi_connected) {
        return ESP_OK;
    }
    if (!s_wifi_events) {
        return ESP_ERR_INVALID_STATE;
    }
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms ? timeout_ms : 30000));
    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    if (bits & WIFI_FAILED_BIT) {
        emit_event(CLARA_NET_EVENT_WIFI_FAILED, nullptr, nullptr, 0, false, 0, ESP_FAIL);
        return ESP_FAIL;
    }
    emit_event(CLARA_NET_EVENT_WIFI_FAILED, nullptr, nullptr, 0, false, 0, ESP_ERR_TIMEOUT);
    return ESP_ERR_TIMEOUT;
}

extern "C" bool clara_net_wifi_is_connected(void)
{
    return s_wifi_connected;
}

extern "C" esp_err_t clara_net_create_session(const char *topic,
                                                char *out_session_id,
                                                size_t out_session_id_len)
{
    if (!out_session_id || out_session_id_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *effective_topic = (topic && topic[0]) ? topic : CONFIG_CLARA_TOPIC;
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    if (effective_topic && effective_topic[0] &&
        !cJSON_AddStringToObject(root, "topic", effective_topic)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    char url[URL_MAX];
    char *response = static_cast<char *>(malloc(HTTP_RESPONSE_MAX));
    if (!response) {
        cJSON_free(body);
        return ESP_ERR_NO_MEM;
    }
    int status = 0;
    bool truncated = false;
    esp_err_t err = make_http_url(url, sizeof(url), "api/session")
                        ? http_perform(url, HTTP_METHOD_POST, "application/json", body,
                                       response, HTTP_RESPONSE_MAX, &status, &truncated)
                        : ESP_ERR_INVALID_ARG;
    cJSON_free(body);
    if (err != ESP_OK) {
        free(response);
        report_error(err, status);
        return err;
    }
    if (truncated) {
        free(response);
        report_error(ESP_ERR_NO_MEM, status);
        return ESP_ERR_NO_MEM;
    }
    cJSON *reply = cJSON_ParseWithLength(response, strlen(response));
    if (!reply) {
        free(response);
        report_error(ESP_ERR_INVALID_RESPONSE, status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(reply, "session_id");
    const char *id = cJSON_IsString(id_item) ? id_item->valuestring : nullptr;
    if (!valid_session_id(id)) {
        cJSON_Delete(reply);
        free(response);
        report_error(ESP_ERR_INVALID_RESPONSE, status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t id_len = strlen(id);
    if (id_len >= out_session_id_len || id_len >= sizeof(s_session_id)) {
        cJSON_Delete(reply);
        free(response);
        report_error(ESP_ERR_NO_MEM, status);
        return ESP_ERR_NO_MEM;
    }
    strlcpy(out_session_id, id, out_session_id_len);
    if (take_lock()) {
        strlcpy(s_session_id, id, sizeof(s_session_id));
        give_lock();
    }
    cJSON_Delete(reply);
    free(response);
    emit_event(CLARA_NET_EVENT_SESSION_CREATED);
    return ESP_OK;
}

extern "C" esp_err_t clara_net_end_session(const char *session_id)
{
    const char *id = session_id;
    if (!id || !id[0]) {
        id = s_session_id;
    }
    if (!valid_session_id(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[160];
    int path_len = snprintf(path, sizeof(path), "api/session/%s/end", id);
    if (path_len <= 0 || static_cast<size_t>(path_len) >= sizeof(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    char url[URL_MAX];
    if (!make_http_url(url, sizeof(url), path)) {
        return ESP_ERR_INVALID_ARG;
    }
    char *response = static_cast<char *>(malloc(HTTP_RESPONSE_MAX));
    if (!response) {
        return ESP_ERR_NO_MEM;
    }
    int status = 0;
    esp_err_t err = http_perform(url, HTTP_METHOD_POST, nullptr, nullptr,
                                 response, HTTP_RESPONSE_MAX, &status, nullptr);
    if (err != ESP_OK) {
        free(response);
        report_error(err, status);
        return err;
    }
    free(response);
    emit_event(CLARA_NET_EVENT_SESSION_ENDED, nullptr, nullptr, 0, false, status);
    return ESP_OK;
}

extern "C" esp_err_t clara_net_get_understanding(const char *session_id,
                                                   char *out_json,
                                                   size_t out_json_len)
{
    const char *id = session_id;
    if (!id || !id[0]) {
        id = s_session_id;
    }
    if (!valid_session_id(id) || !out_json || out_json_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[192];
    int path_len = snprintf(path, sizeof(path), "api/session/%s/understanding", id);
    if (path_len <= 0 || static_cast<size_t>(path_len) >= sizeof(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    char url[URL_MAX];
    if (!make_http_url(url, sizeof(url), path)) {
        return ESP_ERR_INVALID_ARG;
    }
    char *response = static_cast<char *>(malloc(HTTP_RESPONSE_MAX));
    if (!response) {
        return ESP_ERR_NO_MEM;
    }
    int status = 0;
    bool truncated = false;
    esp_err_t err = http_perform(url, HTTP_METHOD_GET, nullptr, nullptr,
                                 response, HTTP_RESPONSE_MAX, &status, &truncated);
    if (err != ESP_OK) {
        free(response);
        report_error(err, status);
        return err;
    }
    size_t n = strlen(response);
    if (truncated || n + 1 > out_json_len) {
        free(response);
        report_error(ESP_ERR_NO_MEM, status);
        return ESP_ERR_NO_MEM;
    }
    memcpy(out_json, response, n + 1);
    free(response);
    return ESP_OK;
}

extern "C" esp_err_t clara_net_transcribe_connect(const char *session_id)
{
    return ws_connect(&s_transcribe, session_id ? session_id : s_session_id);
}

extern "C" esp_err_t clara_net_transcribe_send_audio(const void *pcm, size_t pcm_len)
{
    return ws_send_binary(&s_transcribe, pcm, pcm_len);
}

extern "C" esp_err_t clara_net_transcribe_send_end(void)
{
    return ws_send_text(&s_transcribe, "{\"type\":\"end\"}");
}

extern "C" esp_err_t clara_net_transcribe_disconnect(void)
{
    return ws_cleanup(&s_transcribe);
}

extern "C" esp_err_t clara_net_host_connect(const char *session_id)
{
    return ws_connect(&s_host, session_id ? session_id : s_session_id);
}

extern "C" esp_err_t clara_net_host_send_audio(const void *pcm, size_t pcm_len)
{
    if (!pcm || pcm_len == 0 || pcm_len > HOST_AUDIO_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t encoded_cap = ((pcm_len + 2U) / 3U) * 4U + 1U;
    size_t message_cap = encoded_cap + 64U;
    char *message = static_cast<char *>(malloc(message_cap));
    if (!message) {
        return ESP_ERR_NO_MEM;
    }
    // Encode at the beginning and insert the fixed JSON prefix with memmove;
    // this avoids a second large temporary buffer and keeps -Wrestrict happy.
    size_t encoded_len = 0;
    int rc = mbedtls_base64_encode(reinterpret_cast<unsigned char *>(message),
                                    encoded_cap, &encoded_len,
                                    static_cast<const unsigned char *>(pcm), pcm_len);
    esp_err_t err = ESP_OK;
    if (rc != 0) {
        err = ESP_ERR_INVALID_ARG;
    } else {
        static const char prefix[] = "{\"type\":\"audio\",\"data\":\"";
        constexpr size_t suffix_len = 2; // closing quote and brace
        const size_t prefix_len = sizeof(prefix) - 1;
        if (prefix_len + encoded_len + suffix_len + 1 > message_cap) {
            err = ESP_ERR_NO_MEM;
        } else {
            memmove(message + prefix_len, message, encoded_len);
            memcpy(message, prefix, prefix_len);
            message[prefix_len + encoded_len] = '\"';
            message[prefix_len + encoded_len + 1] = '}';
            message[prefix_len + encoded_len + suffix_len] = '\0';
            err = ws_send_text(&s_host, message);
        }
    }
    free(message);
    return err;
}

extern "C" esp_err_t clara_net_host_send_end_of_speech(void)
{
    return ws_send_text(&s_host, "{\"type\":\"end_of_speech\"}");
}

extern "C" esp_err_t clara_net_host_send_stop(void)
{
    return ws_send_text(&s_host, "{\"type\":\"stop\"}");
}

extern "C" esp_err_t clara_net_host_disconnect(void)
{
    return ws_cleanup(&s_host);
}
