/*
 * ============================================================
 * Theme: ThemePark
 * ============================================================
 */
#ifndef USE_WEBSERVER
#define USE_WEBSERVER 0
#endif

/*
 * Set to 1 for the portfolio failure demonstration.
 * The consumer task will intentionally stall once, allowing the watchdog
 * to detect the missing heartbeat and place the ride in FAULT state.
 */
#ifndef ENABLE_FAULT_INJECTION
#define ENABLE_FAULT_INJECTION 1
#endif

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#if USE_WEBSERVER
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#endif

#define BUTTON_GPIO GPIO_NUM_18
#define RIDE_QUEUE_DEPTH 8

#define EV_BIT_DATA_PRODUCED  BIT0
#define EV_BIT_DATA_PROCESSED BIT1
#define EV_BIT_RIDE_SAFE      BIT2
#define EV_BIT_EMERGENCY      BIT3
#define EV_BIT_SYSTEM_FAULT   BIT4

#define WATCHDOG_PERIOD_MS 1000
#define HEARTBEAT_TIMEOUT_CYCLES 3

static const char *TAG = "app5";

/* ---------- Ride data and state ---------- */

typedef struct {
    uint32_t timestamp_ms;
    uint32_t sequence;
    int speed_mph;
    bool restraints_locked;
} RideStatus;

typedef enum {
    RIDE_READY = 0,
    RIDE_RUNNING,
    RIDE_WARNING,
    RIDE_EMERGENCY,
    RIDE_FAULT
} RideState;

/* ---------- IPC objects ---------- */

static QueueHandle_t data_q;
static EventGroupHandle_t evt_group;
static TaskHandle_t responder_handle;
static SemaphoreHandle_t latency_sem;
static SemaphoreHandle_t ride_mutex;

/* ---------- Shared observability data ---------- */

static RideStatus last_ride;
static RideState ride_state = RIDE_READY;

static volatile uint32_t dropped_samples;
static volatile int64_t last_button_signal_us;
static volatile uint32_t last_notify_latency_us;
static volatile uint32_t last_sem_latency_us;
static volatile bool button_event_pending;
static volatile bool system_fault_active;

static volatile uint32_t hb_prod;
static volatile uint32_t hb_cons;
static volatile uint32_t hb_coord;
static volatile uint32_t hb_resp;

static volatile uint32_t wcet_prod_us;
static volatile uint32_t wcet_cons_us;
static volatile uint32_t wcet_coord_us;
static volatile uint32_t wcet_resp_us;

static volatile int64_t last_edge_us;

/* ---------- Utility functions ---------- */

static const char *ride_state_name(RideState state)
{
    switch (state) {
        case RIDE_READY:
            return "READY";
        case RIDE_RUNNING:
            return "RUNNING";
        case RIDE_WARNING:
            return "WARNING";
        case RIDE_EMERGENCY:
            return "EMERGENCY";
        case RIDE_FAULT:
            return "FAULT";
        default:
            return "UNKNOWN";
    }
}

static void update_wcet(volatile uint32_t *maximum_us, int64_t start_us)
{
    uint32_t elapsed_us =
        (uint32_t)(esp_timer_get_time() - start_us);

    if (elapsed_us > *maximum_us) {
        *maximum_us = elapsed_us;
    }
}

static void set_ride_state(RideState new_state)
{
    if (xSemaphoreTake(ride_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        ride_state = new_state;
        xSemaphoreGive(ride_mutex);
    }
}

static void save_ride_snapshot(
    const RideStatus *ride,
    RideState new_state
)
{
    if (xSemaphoreTake(ride_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        last_ride = *ride;
        ride_state = new_state;
        xSemaphoreGive(ride_mutex);
    }
}

static void read_ride_snapshot(
    RideStatus *ride,
    RideState *state
)
{
    if (xSemaphoreTake(ride_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        *ride = last_ride;
        *state = ride_state;
        xSemaphoreGive(ride_mutex);
    } else {
        *ride = last_ride;
        *state = ride_state;
    }
}

/* ---------- Producer task: ride sensor ----------
 * Generates ride data at 20 Hz and sends it through the queue.
 */

static void producer_task(void *arg)
{
    uint32_t tick = 0;

    for (;;) {
        int64_t start_us = esp_timer_get_time();

        RideStatus item = {
            .timestamp_ms =
                (uint32_t)(esp_timer_get_time() / 1000),
            .sequence = tick,
            .speed_mph = 18 + (int)(tick % 9),
            .restraints_locked = ((tick % 15) != 0)
        };

        if (xQueueSend(data_q, &item, 0) == pdPASS) {
            xEventGroupSetBits(
                evt_group,
                EV_BIT_DATA_PRODUCED
            );
        } else {
            dropped_samples++;

            ESP_LOGW(
                TAG,
                "[ride-sensor] queue full; dropped newest sample seq=%lu",
                (unsigned long)item.sequence
            );
        }

        tick++;
        hb_prod++;
        update_wcet(&wcet_prod_us, start_us);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---------- Consumer task: safety controller ----------
 * Receives queued ride data and evaluates the safety conditions.
 */

static void consumer_task(void *arg)
{
    RideStatus item;

#if ENABLE_FAULT_INJECTION
    bool fault_injected = false;
#endif

    for (;;) {
        if (xQueueReceive(
                data_q,
                &item,
                pdMS_TO_TICKS(250)
            ) == pdPASS) {

            int64_t start_us = esp_timer_get_time();

#if ENABLE_FAULT_INJECTION
            if (!fault_injected && item.sequence >= 100) {
                fault_injected = true;

                ESP_LOGE(
                    TAG,
                    "[fault-injection] safety controller stalled for 5 seconds"
                );

                vTaskDelay(pdMS_TO_TICKS(5000));
            }
#endif

            bool ride_safe =
                item.restraints_locked &&
                item.speed_mph <= 25;

            RideState new_state;

            if (system_fault_active) {
                new_state = RIDE_FAULT;
            } else if (button_event_pending ||
                       (xEventGroupGetBits(evt_group) &
                        EV_BIT_EMERGENCY)) {
                new_state = RIDE_EMERGENCY;
            } else if (ride_safe) {
                new_state = RIDE_RUNNING;
            } else {
                new_state = RIDE_WARNING;
            }

            save_ride_snapshot(&item, new_state);

            if (ride_safe) {
                xEventGroupSetBits(
                    evt_group,
                    EV_BIT_RIDE_SAFE
                );
            } else {
                xEventGroupClearBits(
                    evt_group,
                    EV_BIT_RIDE_SAFE
                );

                ESP_LOGW(
                    TAG,
                    "[safety-controller] unsafe sample seq=%lu speed=%d restraints=%s",
                    (unsigned long)item.sequence,
                    item.speed_mph,
                    item.restraints_locked
                        ? "LOCKED"
                        : "OPEN"
                );
            }

            xEventGroupSetBits(
                evt_group,
                EV_BIT_DATA_PROCESSED
            );

            hb_cons++;
            update_wcet(&wcet_cons_us, start_us);
        } else {
            ESP_LOGW(
                TAG,
                "[safety-controller] queue receive timed out"
            );
        }
    }
}

/* ---------- Coordinator task ----------
 * Waits until a sample has been produced and processed, then signals the
 * responder using a direct task notification.
 */

static void coordinator_task(void *arg)
{
    const EventBits_t wait_mask =
        EV_BIT_DATA_PRODUCED |
        EV_BIT_DATA_PROCESSED;

    for (;;) {
        EventBits_t got =
            xEventGroupWaitBits(
                evt_group,
                wait_mask,
                pdTRUE,
                pdTRUE,
                portMAX_DELAY
            );

        int64_t start_us = esp_timer_get_time();

        if ((got & wait_mask) == wait_mask) {
            if (!system_fault_active) {
                ESP_LOGI(
                    TAG,
                    "[ride-coordinator] sensor-to-safety cycle complete"
                );

                if (responder_handle != NULL) {
                    xTaskNotifyGive(responder_handle);
                }
            } else {
                ESP_LOGE(
                    TAG,
                    "[ride-coordinator] dispatch blocked because system is in FAULT"
                );
            }

            hb_coord++;
        }

        update_wcet(&wcet_coord_us, start_us);
    }
}

/* ---------- Responder task ----------
 * Responds to coordinator notifications and emergency-button notifications.
 */

static void responder_task(void *arg)
{
    for (;;) {
        uint32_t notification_count =
            ulTaskNotifyTake(
                pdTRUE,
                portMAX_DELAY
            );

        if (notification_count == 0) {
            continue;
        }

        int64_t start_us = esp_timer_get_time();

        ESP_LOGI(
            TAG,
            "[responder] notified (count=%lu)",
            (unsigned long)notification_count
        );

        if (button_event_pending) {
            last_notify_latency_us =
                (uint32_t)(
                    esp_timer_get_time() -
                    last_button_signal_us
                );

            button_event_pending = false;

            xEventGroupSetBits(
                evt_group,
                EV_BIT_EMERGENCY
            );

            set_ride_state(RIDE_EMERGENCY);
            printf(
                "\n[EMERGENCY] Operator E-stop handled; notification latency=%lu us\n",
                (unsigned long)last_notify_latency_us
            );
            fflush(stdout);
        } else if (!system_fault_active) {
            ESP_LOGI(
                TAG,
                "[operator-display] ride pipeline complete; ride_safe=%s",
                (xEventGroupGetBits(evt_group) &
                 EV_BIT_RIDE_SAFE)
                    ? "YES"
                    : "NO"
            );
        } else {
            ESP_LOGE(
                TAG,
                "[operator-display] response suppressed while system fault is active"
            );
        }

        hb_resp++;
        update_wcet(&wcet_resp_us, start_us);
    }
}

/* ---------- Emergency button ISR ----------
 * Direct notification is used for the fastest ISR-to-task wake-up path.
 * A binary semaphore is also given for the latency comparison task.
 */

static void IRAM_ATTR button_isr(void *arg)
{
    int64_t now_us = esp_timer_get_time();

    /* 200 ms debounce interval. */
    if (now_us - last_edge_us < 200000) {
        return;
    }

    last_edge_us = now_us;
    last_button_signal_us = now_us;
    button_event_pending = true;

    BaseType_t higher_priority_task_woken = pdFALSE;

    if (responder_handle != NULL) {
        vTaskNotifyGiveFromISR(
            responder_handle,
            &higher_priority_task_woken
        );
    }

    if (latency_sem != NULL) {
        xSemaphoreGiveFromISR(
            latency_sem,
            &higher_priority_task_woken
        );
    }

    portYIELD_FROM_ISR(
        higher_priority_task_woken
    );
}

/* ---------- Binary semaphore latency comparison ---------- */

static void semaphore_latency_task(void *arg)
{
    for (;;) {
        if (xSemaphoreTake(
                latency_sem,
                portMAX_DELAY
            ) == pdTRUE) {

            last_sem_latency_us =
                (uint32_t)(
                    esp_timer_get_time() -
                    last_button_signal_us
                );
            printf(
                "[LATENCY] Binary semaphore wake=%lu us\n",
                (unsigned long)last_sem_latency_us
            );
            fflush(stdout);
        }
    }
}

/* ---------- Heartbeat watchdog ----------
 * Detects a task whose heartbeat has stopped changing for three checks.
 */

static void watchdog_task(void *arg)
{
    uint32_t previous_prod = 0;
    uint32_t previous_cons = 0;
    uint32_t previous_coord = 0;
    uint32_t previous_resp = 0;

    uint32_t stale_prod = 0;
    uint32_t stale_cons = 0;
    uint32_t stale_coord = 0;
    uint32_t stale_resp = 0;

    /* Allow all tasks to start before heartbeat evaluation begins. */
    vTaskDelay(pdMS_TO_TICKS(2000));

    previous_prod = hb_prod;
    previous_cons = hb_cons;
    previous_coord = hb_coord;
    previous_resp = hb_resp;

    for (;;) {
        vTaskDelay(
            pdMS_TO_TICKS(WATCHDOG_PERIOD_MS)
        );

        stale_prod =
            (hb_prod == previous_prod)
                ? stale_prod + 1
                : 0;

        stale_cons =
            (hb_cons == previous_cons)
                ? stale_cons + 1
                : 0;

        stale_coord =
            (hb_coord == previous_coord)
                ? stale_coord + 1
                : 0;

        /*
         * The responder normally receives frequent coordinator notifications.
         * Therefore its heartbeat should also continue changing.
         */
        stale_resp =
            (hb_resp == previous_resp)
                ? stale_resp + 1
                : 0;

        previous_prod = hb_prod;
        previous_cons = hb_cons;
        previous_coord = hb_coord;
        previous_resp = hb_resp;

        bool fault_detected =
            stale_prod >= HEARTBEAT_TIMEOUT_CYCLES ||
            stale_cons >= HEARTBEAT_TIMEOUT_CYCLES ||
            stale_coord >= HEARTBEAT_TIMEOUT_CYCLES ||
            stale_resp >= HEARTBEAT_TIMEOUT_CYCLES;

        if (fault_detected && !system_fault_active) {
            system_fault_active = true;

            xEventGroupSetBits(
                evt_group,
                EV_BIT_SYSTEM_FAULT
            );

            set_ride_state(RIDE_FAULT);

            ESP_LOGE(
                TAG,
                "[watchdog] task heartbeat failure detected: prod=%lu cons=%lu coord=%lu resp=%lu",
                (unsigned long)stale_prod,
                (unsigned long)stale_cons,
                (unsigned long)stale_coord,
                (unsigned long)stale_resp
            );

            ESP_LOGE(
                TAG,
                "[watchdog] ride dispatch disabled and system state set to FAULT"
            );
        }
    }
}

#if USE_WEBSERVER

/* ---------- Web monitor: Core 0 ---------- */

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        esp_wifi_connect();

    } else if (
        event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED
    ) {
        esp_wifi_connect();

        ESP_LOGW(
            TAG,
            "[webmon] Wi-Fi disconnected; retrying"
        );

    } else if (
        event_base == IP_EVENT &&
        event_id == IP_EVENT_STA_GOT_IP
    ) {
        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        ESP_LOGI(
            TAG,
            "[webmon] Got IP: " IPSTR,
            IP2STR(&event->ip_info.ip)
        );
    }
}

static void wifi_init_sta(void)
{
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t config =
        WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&config)
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL
        )
    );

    wifi_config_t wifi_config = {0};

    snprintf(
        (char *)wifi_config.sta.ssid,
        sizeof(wifi_config.sta.ssid),
        "Wokwi-GUEST"
    );

    wifi_config.sta.threshold.authmode =
        WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_STA)
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config
        )
    );

    ESP_ERROR_CHECK(
        esp_wifi_start()
    );
}

static esp_err_t handle_root(httpd_req_t *req)
{
    char page[2400];

    UBaseType_t depth =
        uxQueueMessagesWaiting(data_q);

    EventBits_t bits =
        xEventGroupGetBits(evt_group);

    RideStatus ride = {0};
    RideState state = RIDE_READY;

    read_ride_snapshot(&ride, &state);

    int length =
        snprintf(
            page,
            sizeof(page),

            "<!doctype html>"
            "<html><head>"
            "<meta charset='utf-8'>"
            "<meta http-equiv='refresh' content='1'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>ThemePark Final Project</title>"
            "<style>"
            "body{font-family:Arial;margin:2rem;background:#f4f6f8}"
            ".card{background:white;padding:1rem 1.25rem;border-radius:12px;"
            "max-width:820px;box-shadow:0 2px 10px #0002}"
            "dt{font-weight:bold}dd{margin-bottom:.6rem}"
            "</style></head>"
            "<body><div class='card'>"
            "<h1>ThemePark Ride Safety Monitor</h1><dl>"

            "<dt>System state</dt><dd>%s</dd>"
            "<dt>Queue depth</dt><dd>%u / %u</dd>"
            "<dt>Dropped samples</dt><dd>%lu</dd>"
            "<dt>Event bits</dt><dd>0x%02x</dd>"
            "<dt>Last sample</dt>"
            "<dd>seq=%lu, speed=%d mph, restraints=%s</dd>"
            "<dt>Ride safe</dt><dd>%s</dd>"
            "<dt>Emergency</dt><dd>%s</dd>"
            "<dt>System fault</dt><dd>%s</dd>"

            "<dt>Heartbeats</dt>"
            "<dd>sensor=%lu, safety=%lu, coordinator=%lu, responder=%lu</dd>"

            "<dt>Measured WCET</dt>"
            "<dd>sensor=%lu us, safety=%lu us, coordinator=%lu us, responder=%lu us</dd>"

            "<dt>ISR wake latency</dt>"
            "<dd>notification=%lu us, binary semaphore=%lu us</dd>"

            "</dl></div></body></html>",

            ride_state_name(state),
            (unsigned)depth,
            RIDE_QUEUE_DEPTH,
            (unsigned long)dropped_samples,
            (unsigned)bits,
            (unsigned long)ride.sequence,
            ride.speed_mph,
            ride.restraints_locked
                ? "LOCKED"
                : "OPEN",
            (bits & EV_BIT_RIDE_SAFE)
                ? "YES"
                : "NO",
            (bits & EV_BIT_EMERGENCY)
                ? "ACTIVE"
                : "CLEAR",
            (bits & EV_BIT_SYSTEM_FAULT)
                ? "ACTIVE"
                : "CLEAR",
            (unsigned long)hb_prod,
            (unsigned long)hb_cons,
            (unsigned long)hb_coord,
            (unsigned long)hb_resp,
            (unsigned long)wcet_prod_us,
            (unsigned long)wcet_cons_us,
            (unsigned long)wcet_coord_us,
            (unsigned long)wcet_resp_us,
            (unsigned long)last_notify_latency_us,
            (unsigned long)last_sem_latency_us
        );

    httpd_resp_set_type(
        req,
        "text/html"
    );

    return httpd_resp_send(
        req,
        page,
        length
    );
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config =
        HTTPD_DEFAULT_CONFIG();

    httpd_handle_t server = NULL;

    if (httpd_start(
            &server,
            &config
        ) == ESP_OK) {

        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = handle_root,
            .user_ctx = NULL
        };

        ESP_ERROR_CHECK(
            httpd_register_uri_handler(
                server,
                &root
            )
        );

        ESP_LOGI(
            TAG,
            "[webmon] HTTP server started on port 80"
        );
    }

    return server;
}

static void webmonitor_task(void *arg)
{
    esp_err_t nvs_status =
        nvs_flash_init();

    if (
        nvs_status ==
            ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_status ==
            ESP_ERR_NVS_NEW_VERSION_FOUND
    ) {
        ESP_ERROR_CHECK(
            nvs_flash_erase()
        );

        nvs_status =
            nvs_flash_init();
    }

    ESP_ERROR_CHECK(nvs_status);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(
        esp_event_loop_create_default()
    );

    wifi_init_sta();
    start_webserver();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#else

/* ---------- Serial monitor: Core 0 ---------- */

static void serial_monitor_task(void *arg)
{
    for (;;) {
        UBaseType_t depth =
            uxQueueMessagesWaiting(data_q);

        EventBits_t bits =
            xEventGroupGetBits(evt_group);

        RideStatus ride = {0};
        RideState state = RIDE_READY;

        read_ride_snapshot(&ride, &state);

        printf(
            "[monitor] state=%s q=%u/%u drops=%lu evt=0x%02x "
            "last[seq=%lu speed=%d restraints=%s] safe=%s emergency=%s fault=%s "
            "hb[prod=%lu cons=%lu coord=%lu resp=%lu] "
            "wcet_us[prod=%lu cons=%lu coord=%lu resp=%lu] "
            "latency_us[notify=%lu sem=%lu]\n",

            ride_state_name(state),
            (unsigned)depth,
            RIDE_QUEUE_DEPTH,
            (unsigned long)dropped_samples,
            (unsigned)bits,
            (unsigned long)ride.sequence,
            ride.speed_mph,
            ride.restraints_locked
                ? "LOCKED"
                : "OPEN",
            (bits & EV_BIT_RIDE_SAFE)
                ? "YES"
                : "NO",
            (bits & EV_BIT_EMERGENCY)
                ? "ACTIVE"
                : "CLEAR",
            (bits & EV_BIT_SYSTEM_FAULT)
                ? "ACTIVE"
                : "CLEAR",
            (unsigned long)hb_prod,
            (unsigned long)hb_cons,
            (unsigned long)hb_coord,
            (unsigned long)hb_resp,
            (unsigned long)wcet_prod_us,
            (unsigned long)wcet_cons_us,
            (unsigned long)wcet_coord_us,
            (unsigned long)wcet_resp_us,
            (unsigned long)last_notify_latency_us,
            (unsigned long)last_sem_latency_us
        );
        fflush(stdout);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#endif

/* ---------- Application startup ---------- */

void app_main(void)
{
    printf("\n==== App 5 Final [ThemePark] starting ====\n");
    printf("Fault injection: %s\n",
           ENABLE_FAULT_INJECTION ? "ENABLED" : "DISABLED");
#if USE_WEBSERVER
    printf("Monitor: WEB on Core 0\n");
#else
    printf("Monitor: SERIAL on Core 0\n");
#endif
    fflush(stdout);

    esp_log_level_set(
        TAG,
        ESP_LOG_INFO
    );

    ESP_LOGI(
        TAG,
        "==== App 5 Final [ThemePark] starting ===="
    );

#if ENABLE_FAULT_INJECTION
    ESP_LOGW(
        TAG,
        "Fault injection: ENABLED"
    );
#else
    ESP_LOGI(
        TAG,
        "Fault injection: DISABLED"
    );
#endif

#if USE_WEBSERVER
    ESP_LOGI(
        TAG,
        "Monitor: WEB on Core 0"
    );
#else
    ESP_LOGI(
        TAG,
        "Monitor: SERIAL on Core 0"
    );
#endif

    data_q =
        xQueueCreate(
            RIDE_QUEUE_DEPTH,
            sizeof(RideStatus)
        );

    evt_group =
        xEventGroupCreate();

    latency_sem =
        xSemaphoreCreateBinary();

    ride_mutex =
        xSemaphoreCreateMutex();

    if (
        data_q == NULL ||
        evt_group == NULL ||
        latency_sem == NULL ||
        ride_mutex == NULL
    ) {
        ESP_LOGE(
            TAG,
            "Failed to create IPC objects"
        );

        return;
    }

    /*
     * The responder is created first so the notification handle exists
     * before the coordinator and GPIO ISR can use it.
     */
    if (xTaskCreatePinnedToCore(
            responder_task,
            "emergency_resp",
            4096,
            NULL,
            12,
            &responder_handle,
            APP_CPU_NUM
        ) != pdPASS) {

        ESP_LOGE(TAG, "Failed to create responder task");
        return;
    }

    if (xTaskCreatePinnedToCore(
            semaphore_latency_task,
            "sem_latency",
            4096,
            NULL,
            11,
            NULL,
            APP_CPU_NUM
        ) != pdPASS) {

        ESP_LOGE(TAG, "Failed to create semaphore task");
        return;
    }

    if (xTaskCreatePinnedToCore(
            coordinator_task,
            "ride_coord",
            4096,
            NULL,
            9,
            NULL,
            APP_CPU_NUM
        ) != pdPASS) {

        ESP_LOGE(TAG, "Failed to create coordinator task");
        return;
    }

    if (xTaskCreatePinnedToCore(
            producer_task,
            "ride_sensor",
            4096,
            NULL,
            8,
            NULL,
            APP_CPU_NUM
        ) != pdPASS) {

        ESP_LOGE(TAG, "Failed to create producer task");
        return;
    }

    if (xTaskCreatePinnedToCore(
            consumer_task,
            "safety_ctrl",
            4096,
            NULL,
            8,
            NULL,
            APP_CPU_NUM
        ) != pdPASS) {

        ESP_LOGE(TAG, "Failed to create consumer task");
        return;
    }

    if (xTaskCreatePinnedToCore(
            watchdog_task,
            "heartbeat_wd",
            4096,
            NULL,
            6,
            NULL,
            PRO_CPU_NUM
        ) != pdPASS) {

        ESP_LOGE(TAG, "Failed to create watchdog task");
        return;
    }

#if USE_WEBSERVER

    if (xTaskCreatePinnedToCore(
            webmonitor_task,
            "webmon",
            4096,
            NULL,
            4,
            NULL,
            PRO_CPU_NUM
        ) != pdPASS) {

        ESP_LOGE(TAG, "Failed to create web monitor task");
        return;
    }

#else

    if (xTaskCreatePinnedToCore(
            serial_monitor_task,
            "monitor",
            4096,
            NULL,
            4,
            NULL,
            PRO_CPU_NUM
        ) != pdPASS) {

        ESP_LOGE(TAG, "Failed to create serial monitor task");
        return;
    }

#endif

    gpio_config_t  button_cfg = {
        .pin_bit_mask =
            1ULL << BUTTON_GPIO,
        .mode =
            GPIO_MODE_INPUT,
        .pull_up_en =
            GPIO_PULLUP_ENABLE,
        .pull_down_en =
            GPIO_PULLDOWN_DISABLE,
        .intr_type =
            GPIO_INTR_NEGEDGE
    };

    ESP_ERROR_CHECK(
        gpio_config(&button_cfg)
    );

    ESP_ERROR_CHECK(
        gpio_install_isr_service(0)
    );

    ESP_ERROR_CHECK(
        gpio_isr_handler_add(
            BUTTON_GPIO,
            button_isr,
            NULL
        )
    );
    printf("Emergency button ready on GPIO %d\n", BUTTON_GPIO);
    fflush(stdout);
}
