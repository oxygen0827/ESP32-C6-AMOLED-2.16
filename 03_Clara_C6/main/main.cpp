#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_codec_dev.h"

#include "lvgl_bsp.h"
#include "power_bsp.h"
#include "display_bsp.h"
#include "codec_bsp.h"
#include "clara_audio.h"
#include "clara_net.h"
#include "clara_ui.h"

#ifndef CONFIG_CLARA_BOOT_SELF_TEST
#define CONFIG_CLARA_BOOT_SELF_TEST 0
#endif
#ifndef CONFIG_CLARA_BOOT_SELF_TEST_RETRIES
#define CONFIG_CLARA_BOOT_SELF_TEST_RETRIES 3
#endif
#ifndef CONFIG_CLARA_BOOT_SELF_TEST_RETRY_DELAY_MS
#define CONFIG_CLARA_BOOT_SELF_TEST_RETRY_DELAY_MS 5000
#endif

static const char *TAG = "clara_c6";
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
    ToggleHost,
    RefreshSummary,
    FinishHost,
#if CONFIG_CLARA_BOOT_SELF_TEST
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

static void ui_status(const char *text) { clara_ui_set_status(text); ESP_LOGI(TAG, "%s", text); }

static void net_event(const clara_net_event_t *event, void *)
{
    if (!event) return;
    switch (event->type) {
    case CLARA_NET_EVENT_WIFI_CONNECTING: clara_ui_set_wifi("Wi-Fi: connecting"); break;
    case CLARA_NET_EVENT_WIFI_CONNECTED: clara_ui_set_wifi("Wi-Fi: online"); break;
    case CLARA_NET_EVENT_WIFI_DISCONNECTED: clara_ui_set_wifi("Wi-Fi: offline"); break;
    case CLARA_NET_EVENT_WIFI_FAILED: clara_ui_set_wifi("Wi-Fi: failed"); break;
    case CLARA_NET_EVENT_TRANSCRIBE_CONNECTED:
        if (s_meeting_active) ui_status("Listening - live notes on");
        break;
    case CLARA_NET_EVENT_TRANSCRIBE_DISCONNECTED:
        if (s_meeting_active) enqueue_action(Action::HandleMeetingDisconnect);
        break;
    case CLARA_NET_EVENT_TRANSCRIPT: if (event->text) clara_ui_append_transcript(event->text, event->is_final); break;
    case CLARA_NET_EVENT_HOST_CONNECTED: s_host_connected = true; ui_status("Ask Clara a question"); break;
    case CLARA_NET_EVENT_HOST_DISCONNECTED:
        s_host_connected = false; s_host_recording = false; clara_ui_set_host_active(false); break;
    case CLARA_NET_EVENT_HOST_TRANSCRIPTION:
        if (event->text) {
            char question[640] = {};
            snprintf(question, sizeof(question), "You: %s\nClara: ", event->text);
            clara_ui_set_answer(question);
            ui_status("Question received");
        }
        break;
    case CLARA_NET_EVENT_HOST_ANSWER_TEXT:
        if (event->text) {
            if (event->is_delta) clara_ui_append_answer_delta(event->text, event->is_final);
            else clara_ui_append_answer(event->text, event->is_final);
        }
        break;
    case CLARA_NET_EVENT_HOST_DONE: enqueue_action(Action::FinishHost); break;
    case CLARA_NET_EVENT_ERROR: ui_status("Network error - check Wi-Fi/API"); break;
    default: break;
    }
}

static void audio_task(void *)
{
    bool read_error_logged = false;
    bool send_ok_logged = false;
    bool send_error_logged = false;
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
        (void)clara_audio_alc_process(s_audio_mono, kAudioFrameSamples);
        esp_err_t err = ESP_OK;
        if (s_meeting_active) err = clara_net_transcribe_send_audio(s_audio_mono, sizeof(s_audio_mono));
        if (s_host_recording) {
            esp_err_t host_err = clara_net_host_send_audio(s_audio_mono, sizeof(s_audio_mono));
            if (err == ESP_OK) err = host_err;
        }
        if (err == ESP_OK && !send_ok_logged) {
            ESP_LOGI(TAG, "Audio stream started");
            send_ok_logged = true;
        } else if (err != ESP_OK) {
            if (!send_error_logged) {
                ESP_LOGW(TAG, "Audio stream send failed err=%d", static_cast<int>(err));
                send_error_logged = true;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    s_audio_task = nullptr;
    vTaskDelete(nullptr);
}

static void start_audio_task(void)
{
    if (s_audio_task_run) return;
    s_audio_task_run = true;
    BaseType_t result = xTaskCreate(audio_task, "clara_audio", 6144, nullptr, 5, &s_audio_task);
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
        (void)clara_net_host_send_stop();
        (void)clara_net_host_disconnect();
        s_host_connected = false;
        s_host_recording = false;
    }
    if (s_session_id[0]) {
        (void)clara_net_end_session(s_session_id);
        s_session_id[0] = 0;
    }
    if (!clara_net_wifi_is_connected()) {
        ui_status("Connecting Wi-Fi...");
        if (clara_net_wifi_connect(15000) != ESP_OK) { ui_status("Wi-Fi not ready"); return; }
    }
    ui_status("Creating meeting session...");
    if (clara_net_create_session(CONFIG_CLARA_TOPIC, s_session_id, sizeof(s_session_id)) != ESP_OK) { ui_status("Meeting service unavailable"); return; }
    // Mark the user-requested meeting active before the WSS wait. This closes
    // the narrow race where a socket connects and drops before connect() has
    // returned; the disconnect handler can then reliably recover the UI.
    s_meeting_active = true;
    clara_ui_set_meeting_active(true);
    ui_status("Connecting transcription...");
    if (clara_net_transcribe_connect(s_session_id) != ESP_OK) {
        s_meeting_active = false;
        clara_ui_set_meeting_active(false);
        ui_status("Transcription connection failed");
        clara_net_end_session(s_session_id);
        s_session_id[0] = 0;
        return;
    }
    ui_status("Listening - live notes on");
    clara_ui_reset_transcript();
    clara_ui_reset_answer();
    clara_ui_set_transcript("");
    start_audio_task();
}

static void stop_meeting_impl(void *)
{
    if (!s_meeting_active) return;
    stop_audio_task();
    s_meeting_active = false;
    clara_ui_set_meeting_active(false);
    clara_net_transcribe_send_end(); clara_net_transcribe_disconnect();
    if (s_host_recording) {
        (void)clara_net_host_send_end_of_speech();
        s_host_recording = false;
    }
    clara_ui_set_host_active(false);
    ui_status("Meeting ended - Ask Clara or refresh summary");
}

static void handle_meeting_disconnect_impl(void *)
{
    if (!s_meeting_active) return;
    s_meeting_active = false;
    if (!s_host_recording) stop_audio_task();
    (void)clara_net_transcribe_disconnect();
    clara_ui_set_meeting_active(false);
    ui_status("Connection lost - tap Start to retry");
}

static void toggle_host_impl(void *)
{
    if (!s_session_id[0]) { ui_status("Start a meeting first"); return; }
    if (s_host_recording) {
        (void)clara_net_host_send_end_of_speech();
        s_host_recording = false;
        if (!s_meeting_active) stop_audio_task();
        clara_ui_set_host_active(false); ui_status("Clara is answering..."); return;
    }
    if (s_host_connected) { ui_status("Clara is still answering..."); return; }
    clara_ui_reset_answer();
    if (clara_net_host_connect(s_session_id) != ESP_OK) { ui_status("Clara Q&A unavailable"); return; }
    s_host_recording = true; start_audio_task(); clara_ui_set_host_active(true); ui_status("Ask Clara a question");
}

static void finish_host_impl(void *)
{
    s_host_recording = false;
    if (!s_meeting_active) stop_audio_task();
    if (s_host_connected) (void)clara_net_host_disconnect();
    s_host_connected = false;
    clara_ui_set_host_active(false);
    ui_status(s_meeting_active ? "Listening - live notes on" : "Answer ready");
}

static void refresh_summary_impl(void *)
{
    if (!s_session_id[0]) { ui_status("No meeting to summarize"); return; }
    char json[2048] = {};
    char summary[2048] = {};
    if (clara_net_get_understanding(s_session_id, json, sizeof(json)) == ESP_OK &&
        clara_net_format_understanding(json, summary, sizeof(summary)) == ESP_OK) {
        clara_ui_set_answer(summary); ui_status("Summary refreshed");
    }
    else ui_status("Summary not ready");
}

static void open_clara(void *) { clara_ui_set_page(CLARA_UI_CLARA); }
static void close_clara(void *) { if (!s_meeting_active) clara_ui_set_page(CLARA_UI_HOME); }

#if CONFIG_CLARA_BOOT_SELF_TEST
static void boot_network_self_test(void)
{
    ESP_LOGI(TAG, "Boot network self-test begin");
    if (clara_net_wifi_connect(15000) != ESP_OK) {
        ESP_LOGW(TAG, "Boot network self-test Wi-Fi unavailable");
        return;
    }

    const int max_attempts = CONFIG_CLARA_BOOT_SELF_TEST_RETRIES + 1;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        char session_id[96] = {};
        esp_err_t err = clara_net_create_session(CONFIG_CLARA_TOPIC, session_id,
                                                  sizeof(session_id));
        ESP_LOGI(TAG, "Boot network self-test session err=%d (attempt %d/%d)",
                 static_cast<int>(err), attempt, max_attempts);
        if (err == ESP_OK) {
            err = clara_net_transcribe_connect(session_id);
            ESP_LOGI(TAG, "Boot network self-test transcribe err=%d", static_cast<int>(err));
            if (err == ESP_OK) {
                (void)clara_net_transcribe_disconnect();
            }

            err = clara_net_host_connect(session_id);
            ESP_LOGI(TAG, "Boot network self-test host err=%d", static_cast<int>(err));
            if (err == ESP_OK) {
                (void)clara_net_host_disconnect();
            }
            (void)clara_net_end_session(session_id);
            ESP_LOGI(TAG, "Boot network self-test complete");
            return;
        }

        if (attempt < max_attempts) {
            ESP_LOGW(TAG, "Boot network self-test attempt %d failed, retrying in %d ms",
                     attempt, CONFIG_CLARA_BOOT_SELF_TEST_RETRY_DELAY_MS);
            if (!clara_net_wifi_is_connected()) {
                ESP_LOGW(TAG, "Wi-Fi down after self-test failure, reconnecting...");
                (void)clara_net_wifi_connect(15000);
            }
            vTaskDelay(pdMS_TO_TICKS(CONFIG_CLARA_BOOT_SELF_TEST_RETRY_DELAY_MS));
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
        case Action::ToggleHost: toggle_host_impl(nullptr); break;
        case Action::RefreshSummary: refresh_summary_impl(nullptr); break;
        case Action::FinishHost: finish_host_impl(nullptr); break;
#if CONFIG_CLARA_BOOT_SELF_TEST
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
        ui_status("Clara is still working...");
    }
}

static void start_meeting(void *) { enqueue_action(Action::StartMeeting); }
static void stop_meeting(void *) { enqueue_action(Action::StopMeeting); }
static void toggle_host(void *) { enqueue_action(Action::ToggleHost); }
static void refresh_summary(void *) { enqueue_action(Action::RefreshSummary); }

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Clara C6 starting");
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
    if (clara_audio_alc_init(16000, kAlcGainDb) != ESP_OK) {
        ESP_LOGW(TAG, "ALC unavailable; capture continues without digital gain");
    }

    s_action_queue = xQueueCreate(4, sizeof(Action));
    ESP_ERROR_CHECK(s_action_queue ? ESP_OK : ESP_ERR_NO_MEM);
    BaseType_t action_task_result = xTaskCreate(action_task, "clara_action", 8192,
                                                nullptr, 5, nullptr);
    ESP_ERROR_CHECK(action_task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    clara_ui_callbacks_t callbacks = {
        .open_clara = open_clara, .close_clara = close_clara, .start_meeting = start_meeting,
        .stop_meeting = stop_meeting, .toggle_host = toggle_host, .refresh_summary = refresh_summary, .ctx = nullptr,
    };
    clara_ui_init(&callbacks);
    clara_ui_set_wifi("Wi-Fi: starting");
    clara_net_config_t net_config = {.event_cb = net_event, .ctx = nullptr};
    if (clara_net_init(&net_config) == ESP_OK) {
        ret = clara_net_wifi_start();
        if (ret != ESP_OK) clara_ui_set_wifi("Wi-Fi: configure locally");
    } else clara_ui_set_wifi("Wi-Fi: unavailable");
#if CONFIG_CLARA_BOOT_SELF_TEST
    enqueue_action(Action::BootNetworkSelfTest);
#endif
    ESP_LOGI(TAG, "Clara UI ready; touch the Clara tile");
}
