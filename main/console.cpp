#include "console.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_console.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <argtable3/argtable3.h>

static const char* TAG = "console";

// ── restart ──────────────────────────────────────────────────────────────────

static int cmd_restart(int argc, char** argv) {
    ESP_LOGI(TAG, "Restarting...");
    esp_restart();
    return 0;
}

// ── free ─────────────────────────────────────────────────────────────────────

static int cmd_free(int argc, char** argv) {
    printf("Free heap     : %" PRIu32 " bytes\n", esp_get_free_heap_size());
    printf("Min free heap : %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());
    return 0;
}

// ── tasks ─────────────────────────────────────────────────────────────────────

static int cmd_tasks(int argc, char** argv) {
#if (configUSE_TRACE_FACILITY == 1) && (configUSE_STATS_FORMATTING_FUNCTIONS == 1)
    static char buf[2048];
    vTaskList(buf);
    printf("%-16s %-10s %5s %10s %4s\n", "Name", "State", "Prio", "Stack", "Num");
    printf("------------------------------------------------------------\n");
    printf("%s", buf);
#else
    printf("Enable CONFIG_FREERTOS_USE_TRACE_FACILITY and\n"
           "       CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS in sdkconfig\n");
#endif
    return 0;
}

// ── log_level ────────────────────────────────────────────────────────────────

static struct {
    struct arg_str* tag;
    struct arg_str* level;
    struct arg_end* end;
} s_log_args;

static int cmd_log_level(int argc, char** argv) {
    int errors = arg_parse(argc, argv, (void**)&s_log_args);
    if (errors != 0) {
        arg_print_errors(stderr, s_log_args.end, argv[0]);
        return 1;
    }

    static const struct {
        const char*     name;
        esp_log_level_t level;
    } kLevels[] = {
        {"none",    ESP_LOG_NONE},
        {"error",   ESP_LOG_ERROR},
        {"warn",    ESP_LOG_WARN},
        {"info",    ESP_LOG_INFO},
        {"debug",   ESP_LOG_DEBUG},
        {"verbose", ESP_LOG_VERBOSE},
    };

    const char* level_str = s_log_args.level->sval[0];
    for (size_t i = 0; i < sizeof(kLevels) / sizeof(kLevels[0]); i++) {
        if (strcmp(level_str, kLevels[i].name) == 0) {
            esp_log_level_set(s_log_args.tag->sval[0], kLevels[i].level);
            printf("'%s' log level -> %s\n", s_log_args.tag->sval[0], level_str);
            return 0;
        }
    }

    printf("Invalid level '%s'  (none/error/warn/info/debug/verbose)\n", level_str);
    return 1;
}

// ── public ───────────────────────────────────────────────────────────────────

void console_init(void) {
    esp_console_repl_t* repl = NULL;

    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt            = "esp32> ";
    repl_cfg.max_cmdline_length = 256;

    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl));

    esp_console_register_help_command();

    // restart
    {
        esp_console_cmd_t cmd = {};
        cmd.command = "restart";
        cmd.help    = "Restart the device";
        cmd.func    = cmd_restart;
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    }

    // free
    {
        esp_console_cmd_t cmd = {};
        cmd.command = "free";
        cmd.help    = "Show free heap memory";
        cmd.func    = cmd_free;
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    }

    // tasks
    {
        esp_console_cmd_t cmd = {};
        cmd.command = "tasks";
        cmd.help    = "Show FreeRTOS task list (requires TRACE_FACILITY in sdkconfig)";
        cmd.func    = cmd_tasks;
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    }

    // log_level
    {
        s_log_args.tag   = arg_str1(NULL, NULL, "<tag>",   "Log tag (* for all)");
        s_log_args.level = arg_str1(NULL, NULL, "<level>", "none/error/warn/info/debug/verbose");
        s_log_args.end   = arg_end(2);

        esp_console_cmd_t cmd = {};
        cmd.command  = "log_level";
        cmd.help     = "Set log level: log_level <tag> <level>";
        cmd.func     = cmd_log_level;
        cmd.argtable = &s_log_args;
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    }

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Console ready — type 'help'");
}
