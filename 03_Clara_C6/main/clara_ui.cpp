#include "clara_ui.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "lvgl.h"
#include "lvgl_bsp.h"

namespace {

enum class Action : uint8_t {
    OpenClara,
    CloseClara,
    StartMeeting,
    StopMeeting,
    ToggleHost,
    Refresh,
    OpenDemo,
};

clara_ui_callbacks_t s_callbacks = {};
lv_obj_t *s_screen = nullptr;
lv_obj_t *s_home = nullptr;
lv_obj_t *s_clara = nullptr;
lv_obj_t *s_demo = nullptr;
lv_obj_t *s_wifi = nullptr;
lv_obj_t *s_clara_wifi = nullptr;
lv_obj_t *s_status = nullptr;
lv_obj_t *s_transcript = nullptr;
lv_obj_t *s_answer = nullptr;
lv_obj_t *s_start_btn = nullptr;
lv_obj_t *s_stop_btn = nullptr;
lv_obj_t *s_host_btn = nullptr;
lv_obj_t *s_start_label = nullptr;
lv_obj_t *s_stop_label = nullptr;
lv_obj_t *s_host_label = nullptr;
bool s_ui_initialized = false;
bool s_meeting_active = false;
bool s_host_active = false;

static void set_page_locked(clara_ui_page_t page);

static lv_color_t color(uint32_t value) { return lv_color_hex(value); }

static void set_card_style(lv_obj_t *obj, uint32_t bg, uint32_t border)
{
    lv_obj_set_style_radius(obj, 18, 0);
    lv_obj_set_style_bg_color(obj, color(bg), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, color(border), 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(obj, 16, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, uint32_t fg,
                            lv_text_align_t align = LV_TEXT_ALIGN_LEFT)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color(fg), 0);
    lv_obj_set_style_text_align(label, align, 0);
    return label;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *title, Action action,
                             uint32_t bg = 0x243247, uint32_t fg = 0xF4F7FB)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, LV_SIZE_CONTENT, 54);
    lv_obj_set_style_min_width(button, 128, 0);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_bg_color(button, color(bg), 0);
    lv_obj_set_style_bg_color(button, color(0x334867), LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(button, 18, 0);
    lv_obj_set_style_pad_ver(button, 8, 0);
    lv_obj_add_event_cb(button, [](lv_event_t *event) {
        if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
        Action action = static_cast<Action>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
        switch (action) {
        case Action::OpenClara:
            set_page_locked(CLARA_UI_CLARA);
            if (s_callbacks.open_clara) s_callbacks.open_clara(s_callbacks.ctx);
            break;
        case Action::CloseClara:
            set_page_locked(CLARA_UI_HOME);
            if (s_callbacks.close_clara) s_callbacks.close_clara(s_callbacks.ctx);
            break;
        case Action::StartMeeting:
            if (s_callbacks.start_meeting) s_callbacks.start_meeting(s_callbacks.ctx);
            break;
        case Action::StopMeeting:
            if (s_callbacks.stop_meeting) s_callbacks.stop_meeting(s_callbacks.ctx);
            break;
        case Action::ToggleHost:
            if (s_callbacks.toggle_host) s_callbacks.toggle_host(s_callbacks.ctx);
            break;
        case Action::Refresh:
            if (s_callbacks.refresh_summary) s_callbacks.refresh_summary(s_callbacks.ctx);
            break;
        case Action::OpenDemo:
            set_page_locked(CLARA_UI_DEMO);
            break;
        }
    }, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(action)));
    make_label(button, title, fg, LV_TEXT_ALIGN_CENTER);
    return button;
}

static lv_obj_t *make_header(lv_obj_t *page, const char *title, bool back)
{
    lv_obj_t *header = lv_obj_create(page);
    lv_obj_set_size(header, LV_PCT(100), 62);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (back) {
        lv_obj_t *back_btn = make_button(header, LV_SYMBOL_LEFT, Action::CloseClara, 0x1D2939);
        lv_obj_set_size(back_btn, 52, 44);
        lv_obj_set_style_min_width(back_btn, 52, 0);
    }
    lv_obj_t *heading = make_label(header, title, 0xF4F7FB);
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_24, 0);
    lv_obj_set_flex_grow(heading, 1);
    return header;
}

static void create_home(void)
{
    s_home = lv_obj_create(s_screen);
    lv_obj_set_size(s_home, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_home, color(0x101722), 0);
    lv_obj_set_style_bg_opa(s_home, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_home, 0, 0);
    lv_obj_set_style_pad_all(s_home, 22, 0);
    lv_obj_set_flex_flow(s_home, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_home, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *header = lv_obj_create(s_home);
    lv_obj_set_size(header, LV_PCT(100), 58);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *title = make_label(header, "Clara Home", 0xF4F7FB);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_flex_grow(title, 1);
    s_wifi = make_label(header, "Wi-Fi: checking", 0x9EB2C9, LV_TEXT_ALIGN_RIGHT);

    lv_obj_t *subtitle = make_label(s_home, "Your meeting workspace", 0x9EB2C9);
    lv_obj_set_width(subtitle, LV_PCT(100));
    lv_obj_set_style_pad_bottom(subtitle, 12, 0);

    lv_obj_t *grid = lv_obj_create(s_home);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_row(grid, 14, 0);
    lv_obj_set_style_pad_column(grid, 14, 0);
    lv_obj_set_layout(grid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *clara_card = lv_obj_create(grid);
    lv_obj_set_size(clara_card, 205, 150);
    set_card_style(clara_card, 0x1B3851, 0x3B89B4);
    lv_obj_set_flex_flow(clara_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(clara_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(clara_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(clara_card, [](lv_event_t *e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            set_page_locked(CLARA_UI_CLARA);
            if (s_callbacks.open_clara) s_callbacks.open_clara(s_callbacks.ctx);
        }
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *avatar = lv_obj_create(clara_card);
    lv_obj_set_size(avatar, 54, 54);
    lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(avatar, color(0x57B4D9), 0);
    lv_obj_set_style_border_width(avatar, 0, 0);
    lv_obj_set_style_pad_all(avatar, 0, 0);
    lv_obj_t *c = make_label(avatar, "C", 0xFFFFFF, LV_TEXT_ALIGN_CENTER);
    lv_obj_center(c);
    lv_obj_set_style_text_font(c, &lv_font_montserrat_24, 0);
    lv_obj_t *clara_title = make_label(clara_card, "Clara", 0xFFFFFF, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_font(clara_title, &lv_font_montserrat_20, 0);
    make_label(clara_card, "Meeting notes", 0xB8D6E8, LV_TEXT_ALIGN_CENTER);

    lv_obj_t *audio_card = lv_obj_create(grid);
    lv_obj_set_size(audio_card, 205, 150);
    set_card_style(audio_card, 0x202B3B, 0x394B63);
    lv_obj_set_flex_flow(audio_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(audio_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_label(audio_card, LV_SYMBOL_VOLUME_MAX, 0xE7B86B, LV_TEXT_ALIGN_CENTER);
    make_label(audio_card, "Audio", 0xF4F7FB, LV_TEXT_ALIGN_CENTER);
    make_label(audio_card, "Mic & speaker test", 0x9EB2C9, LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(audio_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(audio_card, [](lv_event_t *e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) set_page_locked(CLARA_UI_DEMO);
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *display_card = lv_obj_create(grid);
    lv_obj_set_size(display_card, 205, 150);
    set_card_style(display_card, 0x202B3B, 0x394B63);
    lv_obj_set_flex_flow(display_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(display_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_label(display_card, LV_SYMBOL_IMAGE, 0x9FD28C, LV_TEXT_ALIGN_CENTER);
    make_label(display_card, "Display", 0xF4F7FB, LV_TEXT_ALIGN_CENTER);
    make_label(display_card, "Touch & LVGL demo", 0x9EB2C9, LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(display_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(display_card, [](lv_event_t *e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) set_page_locked(CLARA_UI_DEMO);
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *settings_card = lv_obj_create(grid);
    lv_obj_set_size(settings_card, 205, 150);
    set_card_style(settings_card, 0x202B3B, 0x394B63);
    lv_obj_set_flex_flow(settings_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(settings_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_label(settings_card, LV_SYMBOL_SETTINGS, 0xC5A5E8, LV_TEXT_ALIGN_CENTER);
    make_label(settings_card, "Settings", 0xF4F7FB, LV_TEXT_ALIGN_CENTER);
    make_label(settings_card, "Device status", 0x9EB2C9, LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(settings_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(settings_card, [](lv_event_t *e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) set_page_locked(CLARA_UI_DEMO);
    }, LV_EVENT_CLICKED, nullptr);
}

static void create_clara(void)
{
    s_clara = lv_obj_create(s_screen);
    lv_obj_set_size(s_clara, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_clara, color(0x101722), 0);
    lv_obj_set_style_border_width(s_clara, 0, 0);
    lv_obj_set_style_pad_all(s_clara, 20, 0);
    lv_obj_set_flex_flow(s_clara, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_clara, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    make_header(s_clara, "Clara", true);
    lv_obj_t *identity = lv_obj_create(s_clara);
    lv_obj_set_width(identity, LV_PCT(100));
    lv_obj_set_height(identity, 70);
    lv_obj_set_style_bg_opa(identity, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(identity, 0, 0);
    lv_obj_set_style_pad_all(identity, 0, 0);
    lv_obj_set_flex_flow(identity, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(identity, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *avatar = lv_obj_create(identity);
    lv_obj_set_size(avatar, 58, 58);
    lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(avatar, color(0x57B4D9), 0);
    lv_obj_set_style_border_width(avatar, 0, 0);
    lv_obj_t *initial = make_label(avatar, "C", 0xFFFFFF, LV_TEXT_ALIGN_CENTER);
    lv_obj_center(initial);
    lv_obj_set_style_text_font(initial, &lv_font_montserrat_24, 0);
    lv_obj_t *identity_text = lv_obj_create(identity);
    lv_obj_set_size(identity_text, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_style_bg_opa(identity_text, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(identity_text, 0, 0);
    lv_obj_set_style_pad_left(identity_text, 14, 0);
    lv_obj_set_flex_flow(identity_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(identity_text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    make_label(identity_text, "Your meeting companion", 0xF4F7FB);
    s_status = make_label(identity_text, "Ready when you are", 0x8ED1B2);
    s_clara_wifi = make_label(identity_text, "Wi-Fi: checking", 0x9EB2C9);

    lv_obj_t *notes = lv_obj_create(s_clara);
    lv_obj_set_width(notes, LV_PCT(100));
    lv_obj_set_flex_grow(notes, 1);
    set_card_style(notes, 0x182231, 0x2A3B52);
    lv_obj_set_scroll_dir(notes, LV_DIR_VER);
    lv_obj_set_flex_flow(notes, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(notes, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    make_label(notes, "LIVE NOTES", 0x70B8D5);
    s_transcript = make_label(notes, "Start a meeting to capture the conversation.", 0xD5E2EE);
    lv_label_set_long_mode(s_transcript, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_transcript, LV_PCT(100));
    lv_obj_set_style_pad_top(s_transcript, 8, 0);
    make_label(notes, "CLARA", 0xD9AA6A);
    s_answer = make_label(notes, "Ask Clara about the meeting after you start.", 0xD5E2EE);
    lv_label_set_long_mode(s_answer, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_answer, LV_PCT(100));
    lv_obj_set_style_pad_top(s_answer, 8, 0);

    lv_obj_t *controls = lv_obj_create(s_clara);
    lv_obj_set_width(controls, LV_PCT(100));
    lv_obj_set_height(controls, 60);
    lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controls, 0, 0);
    lv_obj_set_style_pad_all(controls, 0, 0);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_start_btn = make_button(controls, "Start", Action::StartMeeting, 0x247C68);
    lv_obj_set_style_min_width(s_start_btn, 96, 0);
    s_start_label = lv_obj_get_child(s_start_btn, 0);
    s_stop_btn = make_button(controls, "Stop", Action::StopMeeting, 0x7C3B4A);
    lv_obj_set_style_min_width(s_stop_btn, 96, 0);
    s_stop_label = lv_obj_get_child(s_stop_btn, 0);
    s_host_btn = make_button(controls, "Ask Clara", Action::ToggleHost, 0x38517A);
    lv_obj_set_style_min_width(s_host_btn, 106, 0);
    s_host_label = lv_obj_get_child(s_host_btn, 0);
    lv_obj_t *refresh_btn = make_button(controls, LV_SYMBOL_REFRESH, Action::Refresh, 0x26354A);
    lv_obj_set_style_min_width(refresh_btn, 96, 0);
}

static void create_demo(void)
{
    s_demo = lv_obj_create(s_screen);
    lv_obj_set_size(s_demo, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_demo, color(0x101722), 0);
    lv_obj_set_style_border_width(s_demo, 0, 0);
    lv_obj_set_style_pad_all(s_demo, 22, 0);
    lv_obj_set_flex_flow(s_demo, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_demo, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_header(s_demo, "Device demo", true);
    lv_obj_t *panel = lv_obj_create(s_demo);
    lv_obj_set_width(panel, LV_PCT(100));
    lv_obj_set_flex_grow(panel, 1);
    set_card_style(panel, 0x182231, 0x2A3B52);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_label(panel, LV_SYMBOL_OK, 0x8ED1B2, LV_TEXT_ALIGN_CENTER);
    make_label(panel, "C6 hardware online", 0xF4F7FB, LV_TEXT_ALIGN_CENTER);
    make_label(panel, "SH8601 480x480  /  CST9217 touch", 0x9EB2C9, LV_TEXT_ALIGN_CENTER);
    make_label(panel, "ES8311 + ES7210 audio", 0x9EB2C9, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *back = make_button(s_demo, "Back to apps", Action::CloseClara, 0x243247);
    (void)back;
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/* Called by LVGL event handlers and by locked callers.  It intentionally does
 * not acquire the adapter lock, which avoids recursive-lock deadlocks when a
 * card callback changes pages. */
static void set_page_locked(clara_ui_page_t page)
{
    if (!s_screen) return;
    set_hidden(s_home, page != CLARA_UI_HOME);
    set_hidden(s_clara, page != CLARA_UI_CLARA);
    set_hidden(s_demo, page != CLARA_UI_DEMO);
}

} // namespace

extern "C" void clara_ui_init(const clara_ui_callbacks_t *callbacks)
{
    if (callbacks) s_callbacks = *callbacks;
    if (s_ui_initialized) return;
    if (Lvgl_lock(-1) != ESP_OK) return;
    s_screen = lv_obj_create(nullptr);
    lv_obj_set_size(s_screen, 480, 480);
    lv_obj_set_style_bg_color(s_screen, color(0x101722), 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_screen_load(s_screen);
    create_home();
    create_clara();
    create_demo();
    s_ui_initialized = true;
    set_page_locked(CLARA_UI_HOME);
    s_meeting_active = false;
    s_host_active = false;
    if (s_stop_btn) lv_obj_add_state(s_stop_btn, LV_STATE_DISABLED);
    Lvgl_unlock();
}

extern "C" void clara_ui_set_page(clara_ui_page_t page)
{
    if (Lvgl_lock(-1) != ESP_OK) return;
    set_page_locked(page);
    Lvgl_unlock();
}

extern "C" void clara_ui_set_status(const char *text)
{
    if (!s_status) return;
    if (Lvgl_lock(-1) != ESP_OK) return;
    lv_label_set_text(s_status, text ? text : "");
    Lvgl_unlock();
}

extern "C" void clara_ui_set_transcript(const char *text)
{
    if (!s_transcript) return;
    if (Lvgl_lock(-1) != ESP_OK) return;
    lv_label_set_text(s_transcript, text ? text : "");
    Lvgl_unlock();
}

extern "C" void clara_ui_set_answer(const char *text)
{
    if (!s_answer) return;
    if (Lvgl_lock(-1) != ESP_OK) return;
    lv_label_set_text(s_answer, text ? text : "");
    Lvgl_unlock();
}

extern "C" void clara_ui_set_wifi(const char *text)
{
    if (Lvgl_lock(-1) != ESP_OK) return;
    const char *value = text ? text : "Wi-Fi";
    if (s_wifi) lv_label_set_text(s_wifi, value);
    if (s_clara_wifi) lv_label_set_text(s_clara_wifi, value);
    Lvgl_unlock();
}

extern "C" void clara_ui_set_meeting_active(bool active)
{
    if (Lvgl_lock(-1) != ESP_OK) return;
    s_meeting_active = active;
    if (s_start_btn) {
        if (active) lv_obj_add_state(s_start_btn, LV_STATE_DISABLED);
        else lv_obj_clear_state(s_start_btn, LV_STATE_DISABLED);
    }
    if (s_stop_btn) {
        if (active) lv_obj_clear_state(s_stop_btn, LV_STATE_DISABLED);
        else lv_obj_add_state(s_stop_btn, LV_STATE_DISABLED);
    }
    if (s_start_label) lv_label_set_text(s_start_label, "Start");
    if (s_stop_label) lv_label_set_text(s_stop_label, "Stop");
    Lvgl_unlock();
}

extern "C" void clara_ui_set_host_active(bool active)
{
    if (Lvgl_lock(-1) != ESP_OK) return;
    s_host_active = active;
    if (s_host_label) lv_label_set_text(s_host_label, active ? "End host" : "Ask Clara");
    Lvgl_unlock();
}
