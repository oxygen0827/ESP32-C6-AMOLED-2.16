// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

// POST /api/session → writes session_id into out_session_id (max len bytes)
esp_err_t api_client_create_session(const char *topic,
                                     char *out_session_id, size_t len);
esp_err_t api_client_check_reachable(void);

#define UNDERSTANDING_MAX_TOPICS       24
#define UNDERSTANDING_MAX_COMPARISONS   3
#define UNDERSTANDING_MAX_CONSENSUS     3
#define UNDERSTANDING_MAX_TODOS         3
#define UNDERSTANDING_MAX_DETAILS       2

typedef struct {
    char plan[96];
    char pros[128];
    char cons[128];
    bool recommended;
} understanding_comparison_t;

typedef struct {
    char action[128];
    char owner[64];
    char deadline[48];
} understanding_todo_t;

typedef struct {
    char id[64];
    char title[128];
    char status[32];
    char intensity[24];
    char decision_question[192];
    char decision_object[128];
    char blocker[128];
    char consensus[UNDERSTANDING_MAX_CONSENSUS][160];
    uint8_t consensus_count;
    understanding_comparison_t comparisons[UNDERSTANDING_MAX_COMPARISONS];
    uint8_t comparison_count;
    understanding_todo_t todos[UNDERSTANDING_MAX_TODOS];
    uint8_t todo_count;
    char sub_decisions[UNDERSTANDING_MAX_DETAILS][160];
    uint8_t sub_decision_count;
    char constraints[UNDERSTANDING_MAX_DETAILS][160];
    uint8_t constraint_count;
    char risks[UNDERSTANDING_MAX_DETAILS][160];
    uint8_t risk_count;
    char dependencies[UNDERSTANDING_MAX_DETAILS][160];
    uint8_t dependency_count;
    bool is_primary;
    bool is_latest;
} understanding_topic_t;

typedef struct {
    uint32_t revision;
    uint32_t snapshot_revision;
    bool ready;
    bool delayed;
    char analysis_state[32];
    char model[48];
    char latest_transcript[512];
    char latest_speaker[64];
    bool latest_transcript_final;
    uint16_t summary_interval_sec;
    uint16_t min_new_lines;
    char headline[256];
    uint16_t transcript_count;
    uint16_t committed_transcript_count;
    uint16_t pending_transcript_count;
    uint16_t total;
    uint16_t exploring;
    uint16_t decided;
    uint16_t deferred;
    uint8_t topic_count;
    understanding_topic_t topics[UNDERSTANDING_MAX_TOPICS];
} understanding_snapshot_t;

// GET /api/session/{session_id}/understanding. The response is a full snapshot;
// callers replace their previous model only when this function succeeds.
esp_err_t api_client_get_understanding(const char *session_id,
                                       understanding_snapshot_t *out);

// POST /api/session/{session_id}/end
esp_err_t api_client_end_session(const char *session_id);

#ifdef __cplusplus
}
#endif
