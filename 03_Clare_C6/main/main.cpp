#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include <arpa/inet.h>
#include "esp_codec_dev.h"

#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include "esp_tls.h"

#include "lvgl_bsp.h"
#include "power_bsp.h"
#include "display_bsp.h"
#include "codec_bsp.h"
#include "clare_audio.h"
#include "clare_net.h"
#include "clare_ui.h"

#ifndef CONFIG_CLARE_BOOT_SELF_TEST
#define CONFIG_CLARE_BOOT_SELF_TEST 0
#endif
#ifndef CONFIG_CLARE_BOOT_SELF_TEST_RETRIES
#define CONFIG_CLARE_BOOT_SELF_TEST_RETRIES 3
#endif
#ifndef CONFIG_CLARE_BOOT_SELF_TEST_RETRY_DELAY_MS
#define CONFIG_CLARE_BOOT_SELF_TEST_RETRY_DELAY_MS 5000
#endif

static const char *TAG = "clare_c6";
static I2cMasterBus s_i2c(7, 8, 0);
static DisplayPort *s_display = nullptr;
static CodecPort *s_codec = nullptr;
static char s_session_id[96] = {};
static volatile bool s_meeting_active = false;
static volatile bool s_host_connected = false;
static volatile bool s_host_recording = false;
static volatile bool s_audio_task_run = false;
static TaskHandle_t s_audio_task = nullptr;
enum class Action : uint8_t {
    StartMeeting,
    StopMeeting,
    HandleMeetingDisconnect,
    HandleHostRejected,
    ToggleHost,
    RefreshSummary,
    FinishHost,
#if CONFIG_CLARE_BOOT_SELF_TEST
    BootNetworkSelfTest,
#endif
};
static QueueHandle_t s_action_queue = nullptr;
static constexpr size_t kAudioFrameSamples = 320;  // 20 ms at 16 kHz
// ALC digital gain applied to the 16 kHz mono capture stream.  The codec's
// analog mic gain (35 dB) handles the baseline level; ALC adds a clipping-safe
// boost for quiet voices.  Range: (-64, 63] dB.
static constexpr int8_t kAlcGainDb = 6;
static int16_t s_audio_stereo[kAudioFrameSamples * 2] = {};
static int16_t s_audio_mono[kAudioFrameSamples] = {};

static void enqueue_action(Action action);

static void ui_status(const char *text) { clare_ui_set_status(text); ESP_LOGI(TAG, "%s", text); }

static void net_event(const clare_net_event_t *event, void *)
{
    if (!event) return;
    switch (event->type) {
    case CLARE_NET_EVENT_WIFI_CONNECTING: clare_ui_set_wifi("Wi-Fi: connecting"); break;
    case CLARE_NET_EVENT_WIFI_CONNECTED: clare_ui_set_wifi("Wi-Fi: online"); break;
    case CLARE_NET_EVENT_WIFI_DISCONNECTED: clare_ui_set_wifi("Wi-Fi: offline"); break;
    case CLARE_NET_EVENT_WIFI_FAILED: clare_ui_set_wifi("Wi-Fi: failed"); break;
    case CLARE_NET_EVENT_TRANSCRIBE_CONNECTED:
        if (s_meeting_active) ui_status("Listening - live notes on");
        break;
    case CLARE_NET_EVENT_TRANSCRIBE_DISCONNECTED:
        if (s_meeting_active) enqueue_action(Action::HandleMeetingDisconnect);
        break;
    case CLARE_NET_EVENT_TRANSCRIPT: if (event->text) clare_ui_append_transcript(event->text, event->is_final); break;
    case CLARE_NET_EVENT_HOST_CONNECTED: s_host_connected = true; ui_status("Ask Clare a question"); break;
    case CLARE_NET_EVENT_HOST_DISCONNECTED:
        s_host_connected = false; s_host_recording = false; clare_ui_set_host_active(false); break;
    case CLARE_NET_EVENT_HOST_TRANSCRIPTION:
        if (event->text) {
            char question[640] = {};
            snprintf(question, sizeof(question), "You: %s\nClare: ", event->text);
            clare_ui_set_answer(question);
            ui_status("Question received");
        }
        break;
    case CLARE_NET_EVENT_HOST_ANSWER_TEXT:
        if (event->text) {
            if (event->is_delta) clare_ui_append_answer_delta(event->text, event->is_final);
            else clare_ui_append_answer(event->text, event->is_final);
        }
        break;
    case CLARE_NET_EVENT_HOST_TTS_START: (void)clare_audio_mp3_start(); break;
    case CLARE_NET_EVENT_HOST_ANSWER_AUDIO:
        if (event->binary && event->binary_len) {
            (void)clare_audio_mp3_write(event->binary, event->binary_len, false);
        }
        break;
    case CLARE_NET_EVENT_HOST_TTS_END: (void)clare_audio_mp3_end(); break;
    case CLARE_NET_EVENT_HOST_SESSION_REJECTED:
        ui_status("Session expired - recreating");
        enqueue_action(Action::HandleHostRejected);
        break;
    case CLARE_NET_EVENT_HOST_DONE: enqueue_action(Action::FinishHost); break;
    case CLARE_NET_EVENT_ERROR: ui_status("Network error - check Wi-Fi/API"); break;
    default: break;
    }
}

static void audio_task(void *)
{
    bool read_error_logged = false;
    bool send_ok_logged = false;
    bool send_error_logged = false;
    uint32_t send_fail_streak = 0;
    while (s_audio_task_run) {
        if ((!s_meeting_active && !s_host_recording) || !s_codec) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        esp_codec_dev_handle_t mic = s_codec->Get_audio_codec_microphone();
        if (!mic) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        int ret = esp_codec_dev_read(mic, s_audio_stereo, sizeof(s_audio_stereo));
        if (ret != ESP_CODEC_DEV_OK) {
            if (!read_error_logged) {
                ESP_LOGW(TAG, "Microphone read failed ret=%d", ret);
                read_error_logged = true;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        for (size_t i = 0; i < kAudioFrameSamples; ++i) {
            s_audio_mono[i] = (int16_t)(((int32_t)s_audio_stereo[i * 2] + s_audio_stereo[i * 2 + 1]) / 2);
        }
        (void)clare_audio_alc_process(s_audio_mono, kAudioFrameSamples);
        esp_err_t err = ESP_OK;
        if (s_meeting_active) err = clare_net_transcribe_send_audio(s_audio_mono, sizeof(s_audio_mono));
        if (s_host_recording) {
            esp_err_t host_err = clare_net_host_send_audio(s_audio_mono, sizeof(s_audio_mono));
            if (err == ESP_OK) err = host_err;
        }
        if (err == ESP_OK && !send_ok_logged) {
            ESP_LOGI(TAG, "Audio stream started");
            send_ok_logged = true;
            send_fail_streak = 0;
        } else if (err != ESP_OK) {
            if (!send_error_logged) {
                ESP_LOGW(TAG, "Audio stream send failed err=%d", static_cast<int>(err));
                send_error_logged = true;
            }
            // Watchdog: a silently stalled transport must not keep the meeting
            // in a fake "listening" state. After ~6 s of continuous failures,
            // trigger the same recovery path as a disconnect event.
            if (++send_fail_streak >= 300) {
                send_fail_streak = 0;
                if (s_meeting_active) enqueue_action(Action::HandleMeetingDisconnect);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        } else {
            send_fail_streak = 0;
        }
    }
    s_audio_task = nullptr;
    vTaskDelete(nullptr);
}

static void start_audio_task(void)
{
    if (s_audio_task_run) return;
    s_audio_task_run = true;
    BaseType_t result = xTaskCreate(audio_task, "clare_audio", 6144, nullptr, 5, &s_audio_task);
    if (result != pdPASS) {
        s_audio_task_run = false;
        ui_status("Microphone unavailable");
    }
}

static void stop_audio_task(void)
{
    s_audio_task_run = false;
    for (int i = 0; i < 20 && s_audio_task; ++i) vTaskDelay(pdMS_TO_TICKS(10));
}

static void start_meeting_impl(void *)
{
    if (s_meeting_active) return;
    if (s_host_connected) {
        (void)clare_net_host_send_stop();
        (void)clare_net_host_disconnect();
        s_host_connected = false;
        s_host_recording = false;
    }
    if (s_session_id[0]) {
        (void)clare_net_end_session(s_session_id);
        s_session_id[0] = 0;
    }
    if (!clare_net_wifi_is_connected()) {
        ui_status("Connecting Wi-Fi...");
        if (clare_net_wifi_connect(15000) != ESP_OK) { ui_status("Wi-Fi not ready"); return; }
    }
    ui_status("Creating meeting session...");
    if (clare_net_create_session(CONFIG_CLARE_TOPIC, s_session_id, sizeof(s_session_id)) != ESP_OK) { ui_status("Meeting service unavailable"); return; }
    // Mark the user-requested meeting active before the WSS wait. This closes
    // the narrow race where a socket connects and drops before connect() has
    // returned; the disconnect handler can then reliably recover the UI.
    s_meeting_active = true;
    clare_ui_set_meeting_active(true);
    ui_status("Connecting transcription...");
    if (clare_net_transcribe_connect(s_session_id) != ESP_OK) {
        s_meeting_active = false;
        clare_ui_set_meeting_active(false);
        ui_status("Transcription connection failed");
        clare_net_end_session(s_session_id);
        s_session_id[0] = 0;
        return;
    }
    ui_status("Listening - live notes on");
    clare_ui_reset_transcript();
    clare_ui_reset_answer();
    clare_ui_set_transcript("");
    start_audio_task();
}

static void stop_meeting_impl(void *)
{
    if (!s_meeting_active) return;
    stop_audio_task();
    s_meeting_active = false;
    (void)clare_audio_mp3_end();
    clare_ui_set_meeting_active(false);
    clare_net_transcribe_send_end(); clare_net_transcribe_disconnect();
    if (s_host_recording) {
        (void)clare_net_host_send_end_of_speech();
        s_host_recording = false;
    }
    clare_ui_set_host_active(false);
    ui_status("Meeting ended - Ask Clare or refresh summary");
}

static void handle_meeting_disconnect_impl(void *)
{
    if (!s_meeting_active) return;
    s_meeting_active = false;
    if (!s_host_recording) stop_audio_task();
    (void)clare_net_transcribe_disconnect();
    clare_ui_set_meeting_active(false);
    ui_status("Connection lost - tap Start to retry");
}

/*
 * The server rejected the host channel (403): the session id is no longer
 * valid. Recreate the session transparently and re-enter host mode so the
 * user's question round-trips instead of dying on an expired id.
 */
static void handle_host_rejected_impl(void *)
{
    (void)clare_audio_mp3_end();
    s_host_recording = false;
    s_host_connected = false;
    (void)clare_net_host_disconnect();
    clare_ui_set_host_active(false);
    if (s_session_id[0]) {
        (void)clare_net_end_session(s_session_id);
        s_session_id[0] = 0;
    }
    ui_status("Reconnecting...");
    if (clare_net_create_session(CONFIG_CLARE_TOPIC, s_session_id, sizeof(s_session_id)) != ESP_OK) {
        ui_status("Recreate failed - check network");
        return;
    }
    if (clare_net_host_connect(s_session_id) != ESP_OK) {
        ui_status("Reconnect failed - tap Ask Clare to retry");
        return;
    }
    s_host_recording = true;
    start_audio_task();
    clare_ui_set_host_active(true);
    ui_status("Ask Clare a question");
}

static void toggle_host_impl(void *)
{
    if (!s_session_id[0]) { ui_status("Start a meeting first"); return; }
    if (s_host_recording) {
        (void)clare_net_host_send_end_of_speech();
        s_host_recording = false;
        if (!s_meeting_active) stop_audio_task();
        clare_ui_set_host_active(false); ui_status("Clare is answering..."); return;
    }
    if (s_host_connected) { ui_status("Clare is still answering..."); return; }
    clare_ui_reset_answer();
    if (clare_net_host_connect(s_session_id) != ESP_OK) { ui_status("Clare Q&A unavailable"); return; }
    s_host_recording = true; start_audio_task(); clare_ui_set_host_active(true); ui_status("Ask Clare a question");
}

static void finish_host_impl(void *)
{
    // Let the last sentence finish decoding before tearing the stream down.
    (void)clare_audio_mp3_end();
    s_host_recording = false;
    if (!s_meeting_active) stop_audio_task();
    if (s_host_connected) (void)clare_net_host_disconnect();
    s_host_connected = false;
    clare_ui_set_host_active(false);
    ui_status(s_meeting_active ? "Listening - live notes on" : "Answer ready");
}

static void refresh_summary_impl(void *)
{
    if (!s_session_id[0]) { ui_status("No meeting to summarize"); return; }
    char json[2048] = {};
    char summary[2048] = {};
    if (clare_net_get_understanding(s_session_id, json, sizeof(json)) == ESP_OK &&
        clare_net_format_understanding(json, summary, sizeof(summary)) == ESP_OK) {
        clare_ui_set_answer(summary); ui_status("Summary refreshed");
    }
    else ui_status("Summary not ready");
}

static void open_clare(void *) { clare_ui_set_page(CLARE_UI_CLARE); }
static void close_clare(void *) { if (!s_meeting_active) clare_ui_set_page(CLARE_UI_HOME); }

#if CONFIG_CLARE_BOOT_SELF_TEST
static void boot_network_self_test(void)
{
    ESP_LOGI(TAG, "Boot network self-test begin");
    if (clare_net_wifi_connect(15000) != ESP_OK) {
        ESP_LOGW(TAG, "Boot network self-test Wi-Fi unavailable");
        return;
    }
    // Socket-path probe: isolate DNS vs socket() vs connect() failures.
    {
        struct addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = nullptr;
        int rc = getaddrinfo("clare.vinex.top", "443", &hints, &res);
        // TEMP DIAGNOSTIC: show which IP DNS picked (remove after use)
        if (rc == 0 && res && res->ai_family == AF_INET) {
            char ipbuf[16] = {};
            auto *sa = reinterpret_cast<struct sockaddr_in *>(res->ai_addr);
            ESP_LOGI(TAG, "probe dns ip=%s", inet_ntop(AF_INET, &sa->sin_addr, ipbuf, sizeof(ipbuf)));
        }
        ESP_LOGI(TAG, "probe dns rc=%d", rc);
        if (rc == 0 && res) {
            int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            ESP_LOGI(TAG, "probe socket fd=%d errno=%d", fd, errno);
            if (fd >= 0) {
                int cr = connect(fd, res->ai_addr, res->ai_addrlen);
                ESP_LOGI(TAG, "probe connect rc=%d errno=%d", cr, errno);
                close(fd);
            }
            freeaddrinfo(res);
        }
        // Full HTTPS attempt through esp-tls to surface the real TLS error.
        {
            esp_tls_t *tls = esp_tls_init();
            if (tls) {
                esp_tls_cfg_t cfg = {};
                cfg.timeout_ms = 10000;
                int rc = esp_tls_conn_http_new_sync("https://clare.vinex.top/voice-api/health", &cfg, tls);
                esp_tls_error_handle_t h = nullptr;
                int last = 0, flags = 0;
                if (esp_tls_get_error_handle(tls, &h) == ESP_OK && h) {
                    esp_tls_get_and_clear_last_error(h, &last, &flags);
                }
                ESP_LOGI(TAG, "probe esp-tls rc=%d last=0x%x flags=0x%x",
                         rc, last, flags);
                esp_tls_conn_destroy(tls);
            }
        }
    }

    const int max_attempts = CONFIG_CLARE_BOOT_SELF_TEST_RETRIES + 1;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        char session_id[96] = {};
        esp_err_t err = clare_net_create_session(CONFIG_CLARE_TOPIC, session_id,
                                                  sizeof(session_id));
        ESP_LOGI(TAG, "Boot network self-test session err=%d (attempt %d/%d)",
                 static_cast<int>(err), attempt, max_attempts);
        if (err == ESP_OK) {
            // TEMP EXPERIMENT (hotspot): probe2/probe3 removed so the
            // transcribe WSS is the first new TLS connection after the HTTP
            // session — mirroring real usage. Tests whether rapid successive
            // TLS connections to the CF edge trigger its connection-churn
            // protection (kind=0 FIN'd at ~18s on the hotspot while kind=1,
            // dialed 60s later, succeeds).
            err = clare_net_transcribe_connect(session_id);
            ESP_LOGI(TAG, "Boot network self-test transcribe err=%d", static_cast<int>(err));
            if (err == ESP_OK) {
                (void)clare_net_transcribe_disconnect();
            }

            err = clare_net_host_connect(session_id);
            ESP_LOGI(TAG, "Boot network self-test host err=%d", static_cast<int>(err));
            if (err == ESP_OK) {
                (void)clare_net_host_disconnect();
            }
            (void)clare_net_end_session(session_id);
            ESP_LOGI(TAG, "Boot network self-test complete");
            return;
        }

        if (attempt < max_attempts) {
            ESP_LOGW(TAG, "Boot network self-test attempt %d failed, retrying in %d ms",
                     attempt, CONFIG_CLARE_BOOT_SELF_TEST_RETRY_DELAY_MS);
            if (!clare_net_wifi_is_connected()) {
                ESP_LOGW(TAG, "Wi-Fi down after self-test failure, reconnecting...");
                (void)clare_net_wifi_connect(15000);
            }
            vTaskDelay(pdMS_TO_TICKS(CONFIG_CLARE_BOOT_SELF_TEST_RETRY_DELAY_MS));
        }
    }
    ESP_LOGW(TAG, "Boot network self-test failed after %d attempts", max_attempts);
}
#endif

static void action_task(void *)
{
    Action action;
    while (xQueueReceive(s_action_queue, &action, portMAX_DELAY) == pdTRUE) {
        switch (action) {
        case Action::StartMeeting: start_meeting_impl(nullptr); break;
        case Action::StopMeeting: stop_meeting_impl(nullptr); break;
        case Action::HandleMeetingDisconnect: handle_meeting_disconnect_impl(nullptr); break;
        case Action::HandleHostRejected: handle_host_rejected_impl(nullptr); break;
        case Action::ToggleHost: toggle_host_impl(nullptr); break;
        case Action::RefreshSummary: refresh_summary_impl(nullptr); break;
        case Action::FinishHost: finish_host_impl(nullptr); break;
#if CONFIG_CLARE_BOOT_SELF_TEST
        case Action::BootNetworkSelfTest:
            vTaskDelay(pdMS_TO_TICKS(8000));
            boot_network_self_test();
            break;
#endif
        }
    }
    vTaskDelete(nullptr);
}

static void enqueue_action(Action action)
{
    if (!s_action_queue || xQueueSend(s_action_queue, &action, 0) != pdTRUE) {
        ui_status("Clare is still working...");
    }
}

static void start_meeting(void *) { enqueue_action(Action::StartMeeting); }
static void stop_meeting(void *) { enqueue_action(Action::StopMeeting); }
static void toggle_host(void *) { enqueue_action(Action::ToggleHost); }
static void refresh_summary(void *) { enqueue_action(Action::RefreshSummary); }

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Clare C6 starting");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) { ESP_ERROR_CHECK(nvs_flash_erase()); ret = nvs_flash_init(); }
    ESP_ERROR_CHECK(ret);

    Custom_PmicPortInit(&s_i2c, 0x34);
    s_display = new DisplayPort(s_i2c, 480, 480);
    s_display->DisplayPort_TouchInit();
    Lvgl_PortInit(*s_display);
    s_codec = new CodecPort(s_i2c, "C6_AMOLED_2_16");
    s_codec->CodecPort_SetInfo("es8311 & es7210", 1, 16000, 2, 16);
    s_codec->CodecPort_SetSpeakerVol(70);
    s_codec->CodecPort_SetMicGain(35.0f);
    if (clare_audio_alc_init(16000, kAlcGainDb) != ESP_OK) {
        ESP_LOGW(TAG, "ALC unavailable; capture continues without digital gain");
    }

    s_action_queue = xQueueCreate(4, sizeof(Action));
    ESP_ERROR_CHECK(s_action_queue ? ESP_OK : ESP_ERR_NO_MEM);
    BaseType_t action_task_result = xTaskCreate(action_task, "clare_action", 8192,
                                                nullptr, 5, nullptr);
    ESP_ERROR_CHECK(action_task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    clare_ui_callbacks_t callbacks = {
        .open_clare = open_clare, .close_clare = close_clare, .start_meeting = start_meeting,
        .stop_meeting = stop_meeting, .toggle_host = toggle_host, .refresh_summary = refresh_summary, .ctx = nullptr,
    };
    clare_ui_init(&callbacks);
    clare_ui_set_wifi("Wi-Fi: starting");
    clare_net_config_t net_config = {.event_cb = net_event, .ctx = nullptr};
    if (clare_net_init(&net_config) == ESP_OK) {
        ret = clare_net_wifi_start();
        if (ret != ESP_OK) clare_ui_set_wifi("Wi-Fi: configure locally");
    } else clare_ui_set_wifi("Wi-Fi: unavailable");
#if CONFIG_CLARE_BOOT_SELF_TEST
    enqueue_action(Action::BootNetworkSelfTest);
#endif
    ESP_LOGI(TAG, "Clare UI ready; touch the Clare tile");
}
