/*
 * Copyright (c) 2025 Blynk Technologies Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_random.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <iot_button.h>
#include <button_gpio.h>
#include "blynk_edgent.h"
#include "console.h"
#include "weather.h"

static const char* TAG = "example";

static EventGroupHandle_t evt_group;
#define EVT_CONNECTED       BIT0
#define PUBLISH_INTERVAL_MS      (5 * 60 * 1000U)   // other_task 전송 주기
#define MAIN_PUBLISH_INTERVAL_MS (5 * 60 * 1000U)   // app_main 전송 주기

static volatile bool s_power_on = false;

// ── Blynk Terminal ────────────────────────────────────────────────────────────

static void terminal_send(const char* fmt, ...) {
   char buf[256];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   edgent_publish_ds_str("Terminal", buf);
}

static void terminal_handle_cmd(const char* cmd) {
   ESP_LOGI(TAG, "Terminal cmd: %s", cmd);

   if (strcasecmp(cmd, "help") == 0) {
      terminal_send(
         "Commands:\n"
         "  power on   - Turn power ON\n"
         "  power off  - Turn power OFF\n"
         "  status     - Show device status\n"
         "  weather    - Show Seoul weather\n"
         "  restart    - Restart device\n"
         "  ver        - Show firmware version\n"
         "  help       - This message\n"
      );
   } else if (strcasecmp(cmd, "weather") == 0) {
      terminal_send("날씨 조회 중...");
      weather_data_t wx;
      if (weather_fetch(&wx) == ESP_OK && wx.valid) {
         terminal_send(
            "서울 날씨\n"
            "기온: %.1f°C\n"
            "하늘: %s\n"
            "강수: %s\n"
            "강수확률: %d%%\n"
            "풍속: %.1fm/s\n"
            "습도: %d%%",
            wx.temperature,
            weather_sky_str(wx.sky),
            weather_pty_str(wx.pty),
            wx.pop,
            wx.wsd,
            wx.reh
         );
      } else {
         terminal_send("날씨 조회 실패 (시각 미동기화 또는 네트워크 오류)");
      }
   } else if (strcasecmp(cmd, "power on") == 0) {
      s_power_on = true;
      terminal_send("Power -> ON");
   } else if (strcasecmp(cmd, "power off") == 0) {
      s_power_on = false;
      terminal_send("Power -> OFF");
   } else if (strcasecmp(cmd, "status") == 0) {
      uint32_t uptime_ms = pdTICKS_TO_MS(xTaskGetTickCount());
      terminal_send(
         "Power  : %s\nUptime : %lu s\nHeap   : %lu bytes\n",
         s_power_on ? "ON" : "OFF",
         (unsigned long)(uptime_ms / 1000),
         (unsigned long)esp_get_free_heap_size()
      );
   } else if (strcasecmp(cmd, "ver") == 0) {
      terminal_send("Firmware: v" CONFIG_BLYNK_FIRMWARE_VERSION);
   } else if (strcasecmp(cmd, "restart") == 0) {
      terminal_send("Restarting...");
      vTaskDelay(pdMS_TO_TICKS(500));
      esp_restart();
   } else {
      terminal_send("Unknown command: '%s'  (type 'help')", cmd);
   }
}

#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S3
#   define BOOT_BUTTON_NUM 0
#elif CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32C2 || CONFIG_IDF_TARGET_ESP32C5
#   define BOOT_BUTTON_NUM 9
#endif
#define BUTTON_ACTIVE_LEVEL 0

static void button_event_cb(void* arg, void* data) {
   iot_button_print_event((button_handle_t)arg);
   button_event_t event = iot_button_get_event((button_handle_t)arg);
   if (BUTTON_LONG_PRESS_START == event) {
      edgent_config_reset();
   } else if (BUTTON_DOUBLE_CLICK == event) {
      edgent_config_start();
   }
}

static void button_init(uint32_t button_num) {
   button_config_t btn_cfg = {.long_press_time = 5000, .short_press_time = 200};
   button_gpio_config_t gpio_cfg = {.gpio_num = (int32_t)button_num,
                                    .active_level = BUTTON_ACTIVE_LEVEL,
                                    .enable_power_save = true,
                                    .disable_pull = true};

   button_handle_t btn;
   esp_err_t ret = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn);
   assert(ret == ESP_OK);

   ret |= iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL, button_event_cb, NULL);
   ret |= iot_button_register_cb(btn, BUTTON_DOUBLE_CLICK, NULL, button_event_cb, NULL);
   ret |= iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, NULL, button_event_cb, NULL);

   ESP_ERROR_CHECK(ret);
}

static void on_downlink_datastream_callback(const char* topic, int topic_len, const char* data, int data_len) {
   ESP_LOGI(TAG, "Datastream received — topic: %s, data: %s", topic, data);
   if (strstr(topic, "Terminal")) {
      terminal_handle_cmd(data);
   }
}

static void on_downlink_callback(const char* topic, int topic_len, const char* data, int data_len) {
   ESP_LOGI(TAG, "Downlink received — topic: %s, data: %s", topic, data);

   // Blynk UTC 응답 → 시스템 클럭 설정
   // data 예시: {"time":1778400269383,"iso8601":"2026-05-10T17:04:29+09:00",...}
   if (strstr(topic, "utc")) {
      const char *p = strstr(data, "\"time\":");
      if (p) {
         long long ms = 0;
         sscanf(p + 7, "%lld", &ms);
         if (ms > 1000000000000LL) {
            struct timeval tv;
            tv.tv_sec  = (time_t)(ms / 1000);
            tv.tv_usec = (suseconds_t)((ms % 1000) * 1000);
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "시스템 시각 동기화 완료: %lld", (long long)tv.tv_sec);
         }
      }
   }
}

static void on_state_change(void) {
   ESP_LOGI(TAG, "State change event received");
}

static void on_initial_connection(void) {
   ESP_LOGI(TAG, "Initial connection established");
   edgent_get_ds_all();
   edgent_get_utc();
   edgent_get_location();
}

static void on_config_change(void) {
   ESP_LOGI(TAG, "Configuration change detected");
}

void on_reboot_request(void) {
   xTaskCreate([](void*){ esp_restart(); }, "rst", 2048, NULL, configMAX_PRIORITIES - 1, NULL);
}

// Event handler
static void blynk_edgent_event_handler(void* handler_arg, esp_event_base_t base, int32_t id, void* event_data) {
   if (id == EDGENT_EVENT_STATE_CHANGED && event_data) {
      edgent_state_evt_t* evt = (edgent_state_evt_t*)event_data;
      ESP_LOGD(TAG, "Blynk.Edgent state changed: %d -> %d", evt->prev, evt->curr);
      switch (evt->curr) {
      case EDGENT_STATE_CONNECTED:
         xEventGroupSetBits(evt_group, EVT_CONNECTED);
         break;
      case EDGENT_STATE_ERROR:
         ESP_LOGW(TAG, "Device entered ERROR state, take action!");
         break;
      default:
         break;
      }
   }
}

void other_task(void* arg) {
   EventBits_t bits = xEventGroupWaitBits(evt_group, EVT_CONNECTED, pdFALSE, pdFALSE, portMAX_DELAY);
   if (bits & EVT_CONNECTED) {
      ESP_LOGI(TAG, "[%s] Cloud connected event received! Continuing...", pcTaskGetName(NULL));
   }

   bool         wx_fetched    = false;
   TickType_t   wx_last_tick  = 0;
   const TickType_t WX_INTERVAL = pdMS_TO_TICKS(30UL * 60 * 1000); // 30분마다 갱신

   while (true) {
      double rnd = 10.0 + (esp_random() % 2501) / 100.0;
      edgent_publish_ds_float("Set Temperature", rnd, 2);
      edgent_publish_ds_int("Status", rand() % 5);

      TickType_t now = xTaskGetTickCount();
      if (!wx_fetched || (now - wx_last_tick) >= WX_INTERVAL) {
         weather_data_t wx;
         if (weather_fetch(&wx) == ESP_OK && wx.valid) {
            edgent_publish_ds_float("Weather Temp", wx.temperature, 1);
            edgent_publish_ds_int("Weather POP",      wx.pop);
            edgent_publish_ds_int("Weather Humidity", wx.reh);
            wx_last_tick = now;
            wx_fetched   = true;
         }
         // 실패 시(시각 미동기화·네트워크 오류) wx_fetched 그대로 → 10초 후 재시도
      }

      vTaskDelay(pdMS_TO_TICKS(PUBLISH_INTERVAL_MS));
   }
}

extern "C" void app_main(void) {
   ESP_LOGI(TAG, "app_main() entered");

   /* Suppress log pollution */
   esp_log_level_set("phy_init", ESP_LOG_WARN);
   esp_log_level_set("wifi", ESP_LOG_ERROR);
   esp_log_level_set("wifi_init", ESP_LOG_WARN);
   esp_log_level_set("pp", ESP_LOG_WARN);
   esp_log_level_set("net80211", ESP_LOG_WARN);
   esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
   esp_log_level_set("NimBLE", ESP_LOG_WARN);
   esp_log_level_set("mqtt_client", ESP_LOG_WARN);
   esp_log_level_set("esp_image", ESP_LOG_WARN);
   esp_log_level_set("esp_https_ota", ESP_LOG_WARN);

   /* Initialize NVS — it is used to store PHY calibration data */
   esp_err_t ret = nvs_flash_init();
   if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
   }
   ESP_ERROR_CHECK(ret);

   console_init();

   ESP_ERROR_CHECK(esp_event_loop_create_default());
   ESP_ERROR_CHECK(esp_event_handler_instance_register(
      EDGENT_EVENT_BASE, EDGENT_EVENT_STATE_CHANGED, blynk_edgent_event_handler, NULL, NULL));

   // Create the event group
   evt_group = xEventGroupCreate();
   assert(evt_group);

   // initialize button
   button_init(BOOT_BUTTON_NUM);

   // initialize Edgent
   edgent_config_t config = {};
   config.downlink_ds_callback = on_downlink_datastream_callback;
   config.downlink_callback = on_downlink_callback;
   config.state_change_callback = on_state_change;
   config.config_change_callback = on_config_change;
   config.initial_connection_callback = on_initial_connection;
   config.reboot_request_callback = on_reboot_request;
   edgent_init(&config);

   // start Edgent
   edgent_start();

   // create parallel task
   xTaskCreate(other_task,
               "other_task",
               1024*8,
               NULL,
               tskIDLE_PRIORITY + 1,
               NULL);

   ESP_LOGI(TAG, "Awaiting Cloud connection...");
   EventBits_t bits = xEventGroupWaitBits(evt_group, EVT_CONNECTED, pdFALSE, pdFALSE, portMAX_DELAY);
   if (bits & EVT_CONNECTED) {
      ESP_LOGI(TAG, "[%s] Cloud connected event received! Continuing...", pcTaskGetName(NULL));
   }

   static bool bON = false;
   while (true) {
      // Publish Power state
      edgent_publish_ds_int("Power", (int64_t)bON);

      // Publish random Current Temperature
      double rnd = 10.0 + (esp_random() % 2501) / 100.0;
      edgent_publish_ds_float("Current Temperature", rnd, 2);

      // Publish uptime
      uint32_t uptime_sec = xTaskGetTickCount();
      edgent_publish_ds_int("uptime", uptime_sec);

      // Request metadata
      edgent_get_metadata("Device Name");

      // Toggle widget state
      edgent_set_property("uptime", "isDisabled", bON ? "false" : "true");

      bON = !bON;
      vTaskDelay(pdMS_TO_TICKS(MAIN_PUBLISH_INTERVAL_MS));
   }
}
