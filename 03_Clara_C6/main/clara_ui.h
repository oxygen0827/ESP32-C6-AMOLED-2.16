#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLARA_UI_HOME = 0,
    CLARA_UI_CLARA,
    CLARA_UI_DEMO,
} clara_ui_page_t;

typedef struct {
    void (*open_clara)(void *ctx);
    void (*close_clara)(void *ctx);
    void (*start_meeting)(void *ctx);
    void (*stop_meeting)(void *ctx);
    void (*toggle_host)(void *ctx);
    void (*refresh_summary)(void *ctx);
    void *ctx;
} clara_ui_callbacks_t;

void clara_ui_init(const clara_ui_callbacks_t *callbacks);
void clara_ui_set_page(clara_ui_page_t page);
void clara_ui_set_status(const char *text);
void clara_ui_set_transcript(const char *text);
void clara_ui_reset_transcript(void);
void clara_ui_append_transcript(const char *text, bool is_final);
void clara_ui_set_answer(const char *text);
void clara_ui_reset_answer(void);
void clara_ui_append_answer(const char *text, bool is_final);
void clara_ui_append_answer_delta(const char *text, bool is_final);
void clara_ui_set_wifi(const char *text);
void clara_ui_set_meeting_active(bool active);
void clara_ui_set_host_active(bool active);

#ifdef __cplusplus
}
#endif
