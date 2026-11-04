/* SPDX-License-Identifier: Apache-2.0 */
#include "zv_platform.h"

#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

void app_main(void)
{
    /* Numeric reset cause and monotonic uptime only: no NVS writes or payloads. */
    ESP_LOGI("zv_diag", "boot reset_reason=%d uptime_ms=%llu",
             (int)esp_reset_reason(), (unsigned long long)(esp_timer_get_time() / 1000));
    /*
     * Every rendered clock shows New York time. SNTP syncs the system clock
     * to UTC; this POSIX TZ string makes localtime_r() render EST (UTC-5)
     * with automatic US daylight time (EDT) from the second Sunday in March
     * to the first Sunday in November.
     */
    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
    tzset();

    const zv_platform_report_t report = zv_platform_start();
    ESP_LOGI("zv", "platform %s: status=%d detail=%s", report.ready ? "ready" : "gated", report.status,
             report.detail);
}
