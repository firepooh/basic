#include "weather.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <esp_log.h>
#include <esp_http_client.h>
#include <cJSON.h>

#define TAG "weather"

// 서울 기상청 격자 좌표
#define KMA_NX 60
#define KMA_NY 127

#define KMA_API_KEY  "6defc18b29fab5d86d2b88ac4e6bf439ec25ac1ef300d3846eaa00a2607f979a"
#define KMA_BASE_URL "http://apis.data.go.kr/1360000/VilageFcstInfoService_2.0/getVilageFcst"
#define KMA_NUM_ROWS 20

#define HTTP_BUF_SIZE 8192

// HTTP 응답 수집용 정적 버퍼 (힙/스택 부하 방지)
static char  s_rx_buf[HTTP_BUF_SIZE];
static int   s_rx_len;

// --------------------------------------------------------------------------
// HTTP 이벤트 핸들러
// --------------------------------------------------------------------------
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int space = HTTP_BUF_SIZE - s_rx_len - 1;
        int copy  = (evt->data_len < space) ? evt->data_len : space;
        if (copy > 0) {
            memcpy(s_rx_buf + s_rx_len, evt->data, copy);
            s_rx_len += copy;
            s_rx_buf[s_rx_len] = '\0';
        }
    } else if (evt->event_id == HTTP_EVENT_ON_FINISH) {
        ESP_LOGD(TAG, "HTTP 응답 완료: %d bytes", s_rx_len);
    }
    return ESP_OK;
}

// --------------------------------------------------------------------------
// KMA 발표시각 계산 (KST = UTC+9)
//   발표시각: 0200, 0500, 0800, 1100, 1400, 1700, 2000, 2300
//   데이터 준비에 ~10분 소요 → 30분 여유를 두고 직전 회차 사용
// --------------------------------------------------------------------------
static void get_kma_base_datetime(char date_out[12], char time_out[5])
{
    time_t utc_now;
    time(&utc_now);

    // KST 변환
    time_t kst_now = utc_now + 9 * 3600;
    struct tm t;
    gmtime_r(&kst_now, &t);

    static const int bth[] = {2, 5, 8, 11, 14, 17, 20, 23};

    int cur_min = t.tm_hour * 60 + t.tm_min;
    int adj_min = cur_min - 30;  // 30분 여유

    int   base_hour;
    struct tm date_t = t;

    if (adj_min < 2 * 60) {
        // 02:30 이전이면 전날 2300 회차 사용
        base_hour = 23;
        time_t prev = kst_now - 24 * 3600;
        gmtime_r(&prev, &date_t);
    } else {
        base_hour = bth[0];
        for (int i = 1; i < 8; i++) {
            if (adj_min >= bth[i] * 60) {
                base_hour = bth[i];
            }
        }
    }

    unsigned y = (unsigned)(date_t.tm_year + 1900) % 10000u;
    unsigned m = (unsigned)(date_t.tm_mon  + 1)    % 100u;
    unsigned d = (unsigned)(date_t.tm_mday)         % 100u;
    snprintf(date_out, 12, "%04u%02u%02u", y, m, d);
    snprintf(time_out, 5, "%02d00", base_hour);
}

// --------------------------------------------------------------------------
// 날씨 데이터 조회
// --------------------------------------------------------------------------
esp_err_t weather_fetch(weather_data_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    // 시간이 동기화 되어 있는지 확인
    time_t utc_now;
    time(&utc_now);
    if (utc_now < 1000000000L) {
        ESP_LOGW(TAG, "시스템 시각 미동기화 — SNTP 후 재시도");
        return ESP_ERR_INVALID_STATE;
    }

    char date_str[12];
    char time_str[5];
    get_kma_base_datetime(date_str, time_str);

    char url[512];
    snprintf(url, sizeof(url),
             "%s?serviceKey=%s&numOfRows=%d&pageNo=1&dataType=JSON"
             "&base_date=%s&base_time=%s&nx=%d&ny=%d",
             KMA_BASE_URL, KMA_API_KEY, KMA_NUM_ROWS,
             date_str, time_str, KMA_NX, KMA_NY);

    ESP_LOGI(TAG, "기상청 API 조회: base_date=%s base_time=%s", date_str, time_str);

    s_rx_len = 0;
    s_rx_buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url           = url,
        .event_handler = http_event_handler,
        .timeout_ms    = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client 초기화 실패");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 요청 실패: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP 상태코드: %d", status);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "응답 크기: %d bytes", s_rx_len);

    // JSON 파싱
    cJSON *root = cJSON_Parse(s_rx_buf);
    if (!root) {
        ESP_LOGE(TAG, "JSON 파싱 실패");
        return ESP_FAIL;
    }

    esp_err_t result = ESP_FAIL;

    cJSON *response = cJSON_GetObjectItem(root, "response");
    cJSON *header   = cJSON_GetObjectItem(response, "header");
    if (header) {
        cJSON *rc = cJSON_GetObjectItem(header, "resultCode");
        if (rc && rc->valuestring && strcmp(rc->valuestring, "00") != 0) {
            cJSON *msg = cJSON_GetObjectItem(header, "resultMsg");
            ESP_LOGE(TAG, "API 오류 코드=%s 메시지=%s",
                     rc->valuestring,
                     msg && msg->valuestring ? msg->valuestring : "?");
            goto cleanup;
        }
    }

    {
        cJSON *body     = cJSON_GetObjectItem(response, "body");
        cJSON *items    = cJSON_GetObjectItem(body, "items");
        cJSON *item_arr = cJSON_GetObjectItem(items, "item");

        if (!item_arr || !cJSON_IsArray(item_arr)) {
            ESP_LOGE(TAG, "items 배열 없음");
            goto cleanup;
        }

        // 첫 번째 예보 시각(fcstTime) 확인
        char first_ftime[5] = {0};
        cJSON *first = cJSON_GetArrayItem(item_arr, 0);
        if (first) {
            cJSON *ft = cJSON_GetObjectItem(first, "fcstTime");
            if (ft && ft->valuestring) {
                strncpy(first_ftime, ft->valuestring, 4);
            }
        }

        // 첫 번째 예보 시각 항목만 파싱
        cJSON *item;
        cJSON_ArrayForEach(item, item_arr) {
            cJSON *ft = cJSON_GetObjectItem(item, "fcstTime");
            if (ft && ft->valuestring && strcmp(ft->valuestring, first_ftime) != 0) {
                break;  // 다음 시각으로 넘어가면 중단
            }
            cJSON *cat = cJSON_GetObjectItem(item, "category");
            cJSON *val = cJSON_GetObjectItem(item, "fcstValue");
            if (!cat || !val || !cat->valuestring || !val->valuestring) continue;

            const char *c = cat->valuestring;
            const char *v = val->valuestring;

            if      (strcmp(c, "TMP") == 0) out->temperature = (float)atof(v);
            else if (strcmp(c, "WSD") == 0) out->wsd         = (float)atof(v);
            else if (strcmp(c, "SKY") == 0) out->sky         = atoi(v);
            else if (strcmp(c, "PTY") == 0) out->pty         = atoi(v);
            else if (strcmp(c, "POP") == 0) out->pop         = atoi(v);
            else if (strcmp(c, "REH") == 0) out->reh         = atoi(v);
        }

        out->valid = true;
        result = ESP_OK;

        ESP_LOGI(TAG, "서울 날씨 [%s %s] → %.1f°C 하늘=%s 강수=%s 강수확률=%d%% 풍속=%.1fm/s 습도=%d%%",
                 date_str, time_str,
                 out->temperature,
                 weather_sky_str(out->sky),
                 weather_pty_str(out->pty),
                 out->pop,
                 out->wsd,
                 out->reh);
    }

cleanup:
    cJSON_Delete(root);
    return result;
}

// --------------------------------------------------------------------------
// 보조 문자열 변환 함수
// --------------------------------------------------------------------------
const char* weather_sky_str(int sky)
{
    switch (sky) {
    case 1:  return "맑음";
    case 3:  return "구름많음";
    case 4:  return "흐림";
    default: return "알수없음";
    }
}

const char* weather_pty_str(int pty)
{
    switch (pty) {
    case 0:  return "강수없음";
    case 1:  return "비";
    case 2:  return "비/눈";
    case 3:  return "눈";
    case 4:  return "소나기";
    default: return "알수없음";
    }
}
