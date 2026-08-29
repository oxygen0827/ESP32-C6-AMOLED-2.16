// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#include "api_client.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "api_client";

// Enough for the session create response
#define RESP_BUF_SIZE 512
#define UNDERSTANDING_INITIAL_BUF (16 * 1024)
#define UNDERSTANDING_MAX_BUF     (192 * 1024)

// Accumulate HTTP response into a fixed buffer
typedef struct {
    char   buf[RESP_BUF_SIZE];
    int    len;
} resp_ctx_t;

typedef struct {
    char   *buf;
    size_t  len;
    size_t  capacity;
    bool    overflow;
} dynamic_resp_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_ctx_t *ctx = (resp_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx) {
        int remaining = RESP_BUF_SIZE - ctx->len - 1;
        if (remaining > 0) {
            int copy = evt->data_len < remaining ? evt->data_len : remaining;
            memcpy(ctx->buf + ctx->len, evt->data, copy);
            ctx->len += copy;
            ctx->buf[ctx->len] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t dynamic_http_event_handler(esp_http_client_event_t *evt)
{
    dynamic_resp_ctx_t *ctx = (dynamic_resp_ctx_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !ctx || evt->data_len <= 0) {
        return ESP_OK;
    }

    size_t needed = ctx->len + (size_t)evt->data_len + 1;
    if (needed > UNDERSTANDING_MAX_BUF) {
        ctx->overflow = true;
        return ESP_FAIL;
    }
    if (needed > ctx->capacity) {
        size_t capacity = ctx->capacity ? ctx->capacity : UNDERSTANDING_INITIAL_BUF;
        while (capacity < needed && capacity < UNDERSTANDING_MAX_BUF) capacity *= 2;
        if (capacity > UNDERSTANDING_MAX_BUF) capacity = UNDERSTANDING_MAX_BUF;
        char *grown = heap_caps_realloc(ctx->buf, capacity,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!grown) {
            ctx->overflow = true;
            return ESP_ERR_NO_MEM;
        }
        ctx->buf = grown;
        ctx->capacity = capacity;
    }
    memcpy(ctx->buf + ctx->len, evt->data, (size_t)evt->data_len);
    ctx->len += (size_t)evt->data_len;
    ctx->buf[ctx->len] = '\0';
    return ESP_OK;
}

static void trim_incomplete_utf8(char *text)
{
    size_t len = strlen(text);
    size_t offset = 0;
    size_t valid = 0;
    while (offset < len) {
        unsigned char c = (unsigned char)text[offset];
        size_t width = c < 0x80 ? 1 :
                       (c & 0xE0) == 0xC0 ? 2 :
                       (c & 0xF0) == 0xE0 ? 3 :
                       (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!width || offset + width > len) break;
        bool complete = true;
        for (size_t i = 1; i < width; ++i) {
            if (((unsigned char)text[offset + i] & 0xC0) != 0x80) {
                complete = false;
                break;
            }
        }
        if (!complete) break;
        offset += width;
        valid = offset;
    }
    text[valid] = '\0';
}

static void copy_json_string(cJSON *parent, const char *key,
                             char *dst, size_t dst_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(dst, item->valuestring, dst_len);
        trim_incomplete_utf8(dst);
    }
}

static uint32_t json_u32(cJSON *parent, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    return cJSON_IsNumber(item) && item->valuedouble > 0
        ? (uint32_t)item->valuedouble : 0;
}

static bool string_array_contains(cJSON *array, const char *value)
{
    if (!cJSON_IsArray(array) || !value || !value[0]) return false;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (cJSON_IsString(item) && item->valuestring &&
            strcmp(item->valuestring, value) == 0) return true;
    }
    return false;
}

static uint8_t copy_string_array(cJSON *parent, const char *key,
                                 char dst[][160], uint8_t max_items)
{
    uint8_t count = 0;
    cJSON *array = cJSON_GetObjectItemCaseSensitive(parent, key);
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (count >= max_items) break;
        if (!cJSON_IsString(item) || !item->valuestring) continue;
        strlcpy(dst[count], item->valuestring, 160);
        trim_incomplete_utf8(dst[count]);
        count++;
    }
    return count;
}

static void parse_topic(cJSON *json, understanding_topic_t *topic,
                        const char *primary_id, const char *latest_id)
{
    copy_json_string(json, "id", topic->id, sizeof(topic->id));
    copy_json_string(json, "title", topic->title, sizeof(topic->title));
    copy_json_string(json, "status", topic->status, sizeof(topic->status));
    copy_json_string(json, "intensity", topic->intensity, sizeof(topic->intensity));
    copy_json_string(json, "decisionQuestion", topic->decision_question,
                     sizeof(topic->decision_question));
    copy_json_string(json, "decisionObject", topic->decision_object,
                     sizeof(topic->decision_object));
    copy_json_string(json, "blocker", topic->blocker, sizeof(topic->blocker));
    topic->is_primary = primary_id && strcmp(topic->id, primary_id) == 0;
    topic->is_latest = latest_id && strcmp(topic->id, latest_id) == 0;

    cJSON *consensus = cJSON_GetObjectItemCaseSensitive(json, "consensus");
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, consensus) {
        if (topic->consensus_count >= UNDERSTANDING_MAX_CONSENSUS) break;
        if (cJSON_IsString(item) && item->valuestring) {
            strlcpy(topic->consensus[topic->consensus_count++], item->valuestring,
                    sizeof(topic->consensus[0]));
            trim_incomplete_utf8(topic->consensus[topic->consensus_count - 1]);
        }
    }

    cJSON *comparisons = cJSON_GetObjectItemCaseSensitive(json, "comparison");
    cJSON_ArrayForEach(item, comparisons) {
        if (topic->comparison_count >= UNDERSTANDING_MAX_COMPARISONS) break;
        if (!cJSON_IsObject(item)) continue;
        understanding_comparison_t *comparison =
            &topic->comparisons[topic->comparison_count++];
        copy_json_string(item, "plan", comparison->plan, sizeof(comparison->plan));
        copy_json_string(item, "pros", comparison->pros, sizeof(comparison->pros));
        copy_json_string(item, "cons", comparison->cons, sizeof(comparison->cons));
        comparison->recommended = cJSON_IsTrue(
            cJSON_GetObjectItemCaseSensitive(item, "recommended"));
    }

    cJSON *todos = cJSON_GetObjectItemCaseSensitive(json, "todos");
    cJSON_ArrayForEach(item, todos) {
        if (topic->todo_count >= UNDERSTANDING_MAX_TODOS) break;
        if (!cJSON_IsObject(item)) continue;
        understanding_todo_t *todo = &topic->todos[topic->todo_count++];
        copy_json_string(item, "action", todo->action, sizeof(todo->action));
        copy_json_string(item, "owner", todo->owner, sizeof(todo->owner));
        copy_json_string(item, "deadline", todo->deadline, sizeof(todo->deadline));
    }
    topic->sub_decision_count = copy_string_array(
        json, "subDecisions", topic->sub_decisions, UNDERSTANDING_MAX_DETAILS);
    topic->constraint_count = copy_string_array(
        json, "constraints", topic->constraints, UNDERSTANDING_MAX_DETAILS);
    topic->risk_count = copy_string_array(
        json, "risks", topic->risks, UNDERSTANDING_MAX_DETAILS);
    topic->dependency_count = copy_string_array(
        json, "dependencies", topic->dependencies, UNDERSTANDING_MAX_DETAILS);
}

esp_err_t api_client_check_reachable(void)
{
    esp_http_client_config_t cfg = {
        .url            = CONFIG_WS_MEETING_API_BASE_URL,
        .method         = HTTP_METHOD_GET,
        .timeout_ms     = 30000,
        .addr_type      = HTTP_ADDR_TYPE_INET,
        .skip_cert_common_name_check = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "[FAIL] connectivity check: http client init failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    int tls_error = 0;
    int tls_flags = 0;
    esp_http_client_get_and_clear_last_tls_error(client, &tls_error, &tls_flags);
    esp_http_client_cleanup(client);

    // Any HTTP response proves DNS, routing, TCP/TLS, and the configured backend
    // are reachable. The API base path itself may legitimately return 404.
    if (err != ESP_OK || status <= 0) {
        ESP_LOGE(TAG, "[FAIL] connectivity check http_err=%d status=%d tls_err=%d tls_flags=0x%x",
                 (int)err, status, tls_error, tls_flags);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[OK] backend reachable, status=%d", status);
    return ESP_OK;
}

esp_err_t api_client_create_session(const char *topic,
                                     char *out_session_id, size_t len)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/api/session", CONFIG_WS_MEETING_API_BASE_URL);

    char body[128];
    snprintf(body, sizeof(body), "{\"topic\":\"%s\"}",
             topic ? topic : CONFIG_WS_MEETING_TOPIC);

    resp_ctx_t resp = {0};

    esp_http_client_config_t cfg = {
        .url            = url,
        .method         = HTTP_METHOD_POST,
        .timeout_ms     = 30000,
        .addr_type      = HTTP_ADDR_TYPE_INET,
        .event_handler  = http_event_handler,
        .user_data      = &resp,
        .skip_cert_common_name_check = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "[FAIL] create_session: http client init failed");
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "[FAIL] create_session http_err=%d status=%d", (int)err, status);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_ParseWithLength(resp.buf, (size_t)resp.len);
    if (!root) {
        ESP_LOGE(TAG, "[FAIL] create_session: JSON parse error");
        return ESP_FAIL;
    }
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    if (!cJSON_IsString(id) || !id->valuestring) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "[FAIL] create_session: no session_id in response");
        return ESP_FAIL;
    }
    strlcpy(out_session_id, id->valuestring, len);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "[OK] session created: %s", out_session_id);
    ESP_LOGI(TAG, "heap_free=%lu", esp_get_free_heap_size());
    return ESP_OK;
}

esp_err_t api_client_get_understanding(const char *session_id,
                                       understanding_snapshot_t *out)
{
    if (!session_id || !session_id[0] || !out) return ESP_ERR_INVALID_ARG;

    char url[320];
    snprintf(url, sizeof(url), "%s/api/session/%s/understanding",
             CONFIG_WS_MEETING_API_BASE_URL, session_id);

    dynamic_resp_ctx_t resp = {0};
    esp_http_client_config_t cfg = {
        .url            = url,
        .method         = HTTP_METHOD_GET,
        .timeout_ms     = 20000,
        .addr_type      = HTTP_ADDR_TYPE_INET,
        .event_handler  = dynamic_http_event_handler,
        .user_data      = &resp,
        .skip_cert_common_name_check = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200 || resp.overflow || !resp.buf) {
        ESP_LOGW(TAG, "understanding fetch failed http_err=%d status=%d bytes=%u overflow=%d",
                 (int)err, status, (unsigned)resp.len, resp.overflow);
        free(resp.buf);
        return resp.overflow ? ESP_ERR_NO_MEM : ESP_FAIL;
    }

    cJSON *root = cJSON_ParseWithLength(resp.buf, resp.len);
    free(resp.buf);
    if (!root) {
        ESP_LOGW(TAG, "understanding JSON parse failed (%u bytes)", (unsigned)resp.len);
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(out, 0, sizeof(*out));
    out->revision = json_u32(root, "revision");
    out->snapshot_revision = json_u32(root, "snapshotRevision");
    out->ready = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ready"));
    out->transcript_count = (uint16_t)json_u32(root, "transcriptCount");
    out->committed_transcript_count =
        (uint16_t)json_u32(root, "committedTranscriptCount");

    cJSON *analysis = cJSON_GetObjectItemCaseSensitive(root, "analysis");
    if (cJSON_IsObject(analysis)) {
        copy_json_string(analysis, "state", out->analysis_state,
                         sizeof(out->analysis_state));
        out->pending_transcript_count =
            (uint16_t)json_u32(analysis, "pendingTranscriptCount");
        out->delayed = strcmp(out->analysis_state, "failed") == 0 ||
                       strcmp(out->analysis_state, "final_failed") == 0;
    }

    cJSON *debug = cJSON_GetObjectItemCaseSensitive(root, "debug");
    if (cJSON_IsObject(debug)) {
        copy_json_string(debug, "model", out->model, sizeof(out->model));
        out->summary_interval_sec = (uint16_t)json_u32(debug, "summaryIntervalSec");
        out->min_new_lines = (uint16_t)json_u32(debug, "minNewLines");
        cJSON *latest = cJSON_GetObjectItemCaseSensitive(debug, "latestTranscript");
        if (cJSON_IsObject(latest)) {
            copy_json_string(latest, "text", out->latest_transcript,
                             sizeof(out->latest_transcript));
            copy_json_string(latest, "speaker", out->latest_speaker,
                             sizeof(out->latest_speaker));
            out->latest_transcript_final =
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(latest, "isFinal"));
        }
    }

    char primary_id[64] = {0};
    char latest_id[64] = {0};
    cJSON *overview = cJSON_GetObjectItemCaseSensitive(root, "overview");
    if (cJSON_IsObject(overview)) {
        copy_json_string(overview, "headline", out->headline, sizeof(out->headline));
        copy_json_string(overview, "primaryDecisionId", primary_id, sizeof(primary_id));
        copy_json_string(overview, "latestThreadId", latest_id, sizeof(latest_id));
        cJSON *counts = cJSON_GetObjectItemCaseSensitive(overview, "counts");
        if (cJSON_IsObject(counts)) {
            out->total = (uint16_t)json_u32(counts, "total");
            out->exploring = (uint16_t)json_u32(counts, "exploring");
            out->decided = (uint16_t)json_u32(counts, "decided");
            out->deferred = (uint16_t)json_u32(counts, "deferred");
        }
    }

    cJSON *decision_state = cJSON_GetObjectItemCaseSensitive(root, "decisionState");
    cJSON *focus = cJSON_IsObject(decision_state)
        ? cJSON_GetObjectItemCaseSensitive(decision_state, "focus") : NULL;
    if (!primary_id[0] && cJSON_IsObject(focus)) {
        copy_json_string(focus, "primaryId", primary_id, sizeof(primary_id));
    }
    cJSON *secondary_ids = cJSON_IsObject(focus)
        ? cJSON_GetObjectItemCaseSensitive(focus, "secondaryIds") : NULL;

    cJSON *understanding = cJSON_GetObjectItemCaseSensitive(root, "understanding");
    cJSON *topics = cJSON_IsObject(understanding)
        ? cJSON_GetObjectItemCaseSensitive(understanding, "topics") : NULL;

    // Render order follows the API contract: primary, secondary, then remaining
    // topics. This keeps the most important item on page one without guessing.
    cJSON *topic_json = NULL;
    for (int pass = 0; pass < 3; ++pass) {
        cJSON_ArrayForEach(topic_json, topics) {
            if (out->topic_count >= UNDERSTANDING_MAX_TOPICS || !cJSON_IsObject(topic_json)) break;
            cJSON *id_json = cJSON_GetObjectItemCaseSensitive(topic_json, "id");
            const char *id = cJSON_IsString(id_json) ? id_json->valuestring : "";
            bool is_primary = primary_id[0] && strcmp(id, primary_id) == 0;
            bool is_secondary = string_array_contains(secondary_ids, id);
            if ((pass == 0 && !is_primary) ||
                (pass == 1 && (is_primary || !is_secondary)) ||
                (pass == 2 && (is_primary || is_secondary))) continue;
            parse_topic(topic_json, &out->topics[out->topic_count++],
                        primary_id, latest_id);
        }
    }
    if (!out->total) out->total = (uint16_t)cJSON_GetArraySize(topics);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "understanding snapshot=%lu ready=%d topics=%u/%u state=%s transcript=%u committed=%u pending=%u model=%s interval=%us latest=\"%.80s\"",
             (unsigned long)out->snapshot_revision, out->ready,
             out->topic_count, out->total, out->analysis_state,
             out->transcript_count, out->committed_transcript_count,
             out->pending_transcript_count, out->model,
             out->summary_interval_sec, out->latest_transcript);
    return ESP_OK;
}

esp_err_t api_client_end_session(const char *session_id)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/api/session/%s/end",
             CONFIG_WS_MEETING_API_BASE_URL, session_id);

    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .addr_type  = HTTP_ADDR_TYPE_INET,
        .skip_cert_common_name_check = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "[FAIL] end_session: http client init failed");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status == 200) {
        ESP_LOGI(TAG, "[OK] session ended: %s", session_id);
        ESP_LOGI(TAG, "heap_free=%lu", esp_get_free_heap_size());
        return ESP_OK;
    }
    ESP_LOGE(TAG, "[FAIL] end_session http_err=%d status=%d", (int)err, status);
    return ESP_FAIL;
}
