#pragma once
#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float temperature;  // TMP: 기온 (°C)
    float wsd;          // WSD: 풍속 (m/s)
    int   sky;          // SKY: 하늘상태  1=맑음 3=구름많음 4=흐림
    int   pty;          // PTY: 강수형태  0=없음 1=비 2=비/눈 3=눈 4=소나기
    int   pop;          // POP: 강수확률 (%)
    int   reh;          // REH: 습도 (%)
    bool  valid;
} weather_data_t;

esp_err_t   weather_fetch(weather_data_t *out);
const char* weather_sky_str(int sky);
const char* weather_pty_str(int pty);

#ifdef __cplusplus
}
#endif
