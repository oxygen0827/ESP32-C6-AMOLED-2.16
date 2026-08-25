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
#include "clara_net.h"
#include "clara_ui.h"

static const char *TAG = "clara_c6";
static I2cMasterBus s_i2c(7, 8, 0);
static DisplayPort *s_display = nullptr;
static CodecPort *s_codec = nullptr;
static char s_session_id[96] = {};
static volatile bool s_meeting_active = false;
static volatile bool s_host_active = false;
static volatile bool s_audio_task_run = false;
static TaskHandle_t s_audio_task = nullptr;
enum class Action : uint8_t { StartMeeting, StopMeeting, ToggleHost, RefreshSummary };
static QueueHandle_t s_action_queue = nullptr;
static constexpr size_t kAudioFrameSamples = 320;  // 20 ms at 16 kHz
static int16_t s_audio_stereo[kAudioFrameSamples * 2] = {};
static int16_t s_audio_mono[kAudioFrameSamples] = {};

static void ui_status(const char *text) { clara_ui_set_status(text); ESP_LOGI(TAG, "%s", text); }

static void net_event(const clara_net_event_t *event, void *)
{
    if (!event) return;
    switch (event->type) {
    case CLARA_NET_EVENT_WIFI_CONNECTING: clara_ui_set_wifi("Wi-Fi: connecting"); break;
    case CLARA_NET_EVENT_WIFI_CONNECTED: clara_ui_set_wifi("Wi-Fi: online"); break;
    case CLARA_NET_EVENT_WIFI_DISCONNECTED: clara_ui_set_wifi("Wi-Fi: offline"); break;
    case CLARA_NET_EVENT_WIFI_FAILED: clara_ui_set_wifi("Wi-Fi: failed"); break;
    case CLARA_NET_EVENT_TRANSCRIBE_CONNECTED: ui_status("Listening  •  live notes on"); break;
    case CLARA_NET_EVENT_TRANSCRIBE_DISCONNECTED: ui_status("Meeting link closed"); break;
    case CLARA_NET_EVENT_TRANSCRIPT: if (event->text) clara_ui_set_transcript(event->text); break;
    case CLARA_NET_EVENT_HOST_CONNECTED: s_host_active = true; clara_ui_set_host_active(true); ui_status("Ask Clara a question"); break;
    case CLARA_NET_EVENT_HOST_DISCONNECTED: s_host_active = false; clara_ui_set_host_active(false); break;
    case CLARA_NET_EVENT_HOST_TRANSCRIPTION: if (event->text) clara_ui_set_transcript(event->text); break;
    case CLARA_NET_EVENT_HOST_ANSWER_TEXT: if (event->text) clara_ui_set_answer(event->text); break;
    case CLARA_NET_EVENT_ERROR: ui_status("Network error  •  check Wi-Fi/API"); break;
    default: break;
    }
}

static void audio_task(void *)
{
    while (s_audio_task_run) {
        if (!s_meeting_active || !s_codec) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        esp_codec_dev_handle_t mic = s_codec->Get_audio_codec_microphone();
        if (!mic) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        int ret = esp_codec_dev_read(mic, s_audio_stereo, sizeof(s_audio_stereo));
        if (ret != ESP_CODEC_DEV_OK) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        for (size_t i = 0; i < kAudioFrameSamples; ++i) {
            s_audio_mono[i] = (int16_t)(((int32_t)s_audio_stereo[i * 2] + s_audio_stereo[i * 2 + 1]) / 2);
        }
        esp_err_t err = s_host_active ? clara_net_host_send_audio(s_audio_mono, sizeof(s_audio_mono)) : clara_net_transcribe_send_audio(s_audio_mono, sizeof(s_audio_mono));
        if (err != ESP_OK) vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_audio_task = nullptr;
    vTaskDelete(nullptr);
}

static void start_audio_task(void)
{
    if (s_audio_task_run) return;
    s_audio_task_run = true;
    xTaskCreate(audio_task, "clara_audio", 6144, nullptr, 5, &s_audio_task);
}

static void stop_audio_task(void)
{
    s_audio_task_run = false;
    for (int i = 0; i < 20 && s_audio_task; ++i) vTaskDelay(pdMS_TO_TICKS(10));
}

static void start_meeting_impl(void *)
{
    if (s_meeting_active) return;
    if (!clara_net_wifi_is_connected()) {
        ui_status("Connecting Wi-Fi...");
        if (clara_net_wifi_connect(15000) != ESP_OK) { ui_status("Wi-Fi not ready"); return; }
    }
    ui_status("Creating meeting session...");
    if (clara_net_create_session(CONFIG_CLARA_TOPIC, s_session_id, sizeof(s_session_id)) != ESP_OK) { ui_status("Meeting service unavailable"); return; }
    if (clara_net_transcribe_connect(s_session_id) != ESP_OK) {
        ui_status("Transcription connection failed"); clara_net_end_session(s_session_id); s_session_id[0] = 0; return;
    }
    s_meeting_active = true;
    clara_ui_set_meeting_active(true);
    clara_ui_set_transcript("Listening for the meeting...");
    start_audio_task();
}

static void stop_meeting_impl(void *)
{
    if (!s_meeting_active) return;
    stop_audio_task();
    clara_net_transcribe_send_end(); clara_net_transcribe_disconnect();
    if (s_host_active) { clara_net_host_disconnect(); s_host_active = false; }
    if (s_session_id[0]) clara_net_end_session(s_session_id);
    s_session_id[0] = 0; s_meeting_active = false;
    clara_ui_set_meeting_active(false); clara_ui_set_host_active(false); ui_status("Meeting ended");
}

static void toggle_host_impl(void *)
{
    if (!s_meeting_active) { ui_status("Start a meeting first"); return; }
    if (s_host_active) {
        clara_net_host_send_stop(); clara_net_host_disconnect(); s_host_active = false;
        clara_ui_set_host_active(false); ui_status("Listening  •  live notes on"); return;
    }
    if (clara_net_host_connect(s_session_id) != ESP_OK) { ui_status("Clara Q&A unavailable"); return; }
    s_host_active = true; clara_ui_set_host_active(true); ui_status("Ask Clara a question");
}

static void refresh_summary_impl(void *)
{
    if (!s_session_id[0]) { ui_status("No active meeting"); return; }
    char json[2048] = {};
    if (clara_net_get_understanding(s_session_id, json, sizeof(json)) == ESP_OK) { clara_ui_set_answer(json); ui_status("Summary refreshed"); }
    else ui_status("Summary not ready");
}

static void open_clara(void *) { clara_ui_set_page(CLARA_UI_CLARA); }
static void close_clara(void *) { if (!s_meeting_active) clara_ui_set_page(CLARA_UI_HOME); }

static void action_task(void *)
{
    Action action;
    while (xQueueReceive(s_action_queue, &action, portMAX_DELAY) == pdTRUE) {
        switch (action) {
        case Action::StartMeeting: start_meeting_impl(nullptr); break;
        case Action::StopMeeting: stop_meeting_impl(nullptr); break;
        case Action::ToggleHost: toggle_host_impl(nullptr); break;
        case Action::RefreshSummary: refresh_summary_impl(nullptr); break;
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
    start_audio_task();
    ESP_LOGI(TAG, "Clara UI ready; touch the Clara tile");
}
