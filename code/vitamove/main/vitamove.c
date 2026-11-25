#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "mqtt_client.h"        // espressif/mqtt (componente gestionado)
#include "cJSON.h"              // espressif/cjson (componente gestionado)

#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/i2s_pdm.h"

// ============================================================================
// CONFIGURACIÓN DEL PROYECTO
// ============================================================================

// ThingsBoard (MQTT)
#define THINGSBOARD_URI         "mqtt://eu.thingsboard.cloud"
#define THINGSBOARD_PORT        1883
#define ACCESS_TOKEN            "rYqpdzYL5OiOXWl3NZoO"
#define TELEMETRY_TOPIC         "v1/devices/me/telemetry"
#define TELEMETRY_PERIOD_MS     30000

// WiFi: prioridad 1 = hogar, prioridad 2 = hotspot del móvil
#define WIFI_SSID_HOME          "SBC"
#define WIFI_PASS_HOME          "SBCwifi$"
#define WIFI_SSID_HOTSPOT       "PLACEHOLDER"
#define WIFI_PASS_HOTSPOT       "PLACEHOLDER"
#define WIFI_RETRIES_PER_NET    3

// OTA
#define CURRENT_FW_VERSION      "1.0.0"
#define VERSION_URL             "https://raw.githubusercontent.com/MPimienta/VitaMove/master/firmware/version.txt"
#define FIRMWARE_URL            "https://raw.githubusercontent.com/MPimienta/VitaMove/master/firmware/firmware.bin"

#define OTA_CHECK_PERIOD_MS     (6U * 60U * 60U * 1000U)   // cada 6 horas

static const char *TAG = "VitaMove_Main";

// ============================================================================
// DEFINICIÓN DE PINES (LILYGO T-Watch 2020 V3)
// ============================================================================

// I2C 0 (PMU AXP202, Acel. BMA423, RTC PCF8563)
#define I2C_MASTER_SCL_IO           22
#define I2C_MASTER_SDA_IO           21
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          400000

// I2C 1 (Touch FT6336U exclusivo)
#define I2C_TOUCH_SCL_IO            32
#define I2C_TOUCH_SDA_IO            23
#define I2C_TOUCH_NUM               I2C_NUM_1
#define I2C_TOUCH_FREQ_HZ           400000

// Direcciones I2C
#define ADDR_AXP202                 0x35
#define ADDR_BMA423                 0x19
#define ADDR_PCF8563                0x51
#define ADDR_FT6336                 0x38

// AXP202: registro de control de salidas de alimentación
#define AXP202_REG_POWER_OUT_CTRL   0x12
#define AXP202_LDO2_BIT             (1 << 2)   // LDO2: pantalla/backlight (VERIFICAR en V3)

// I2S (Micrófono PDM SPM1423HM4H-B)
#define I2S_MIC_PORT                I2S_NUM_0
#define I2S_MIC_CLK                 0
#define I2S_MIC_DATA                2

// SPI (Pantalla ST7789V)
#define TFT_SPI_HOST                SPI3_HOST
#define TFT_MISO                    -1 // No se usa lectura de la pantalla
#define TFT_MOSI                    19
#define TFT_SCLK                    18
#define TFT_CS                      5
#define TFT_DC                      27
#define TFT_MAX_TRANSFER_SZ         (240 * 240 * 2 + 8)

// GPIOs (Motor e Interrupciones)
#define MOTOR_PIN                   4
#define PMU_IRQ_PIN                 35
#define RTC_IRQ_PIN                 37
#define TOUCH_IRQ_PIN               38
#define BMA_IRQ_PIN                 39

// ============================================================================
// HANDLES Y ESTADO GLOBAL
// ============================================================================
i2c_master_bus_handle_t i2c_bus_0_handle;
i2c_master_bus_handle_t i2c_touch_bus_handle;
i2c_master_dev_handle_t pmu_dev_handle;     // AXP202 (lo usará Task 8)
i2s_chan_handle_t rx_chan;                  // Handle del micrófono PDM

// Event group de red
#define WIFI_CONNECTED_BIT  BIT0
#define MQTT_CONNECTED_BIT  BIT1
static EventGroupHandle_t g_events;

// Estado compartido entre tareas (protegido por mutex)
typedef struct {
    int16_t accel_x, accel_y, accel_z;  // Task 2
    uint8_t battery_pct;                // Task 8
    bool    charging;                   // Task 8
    double  lat, lon;                   // Task 7
    bool    gps_valid;                  // Task 7
} vm_state_t;

static vm_state_t        g_state;
static SemaphoreHandle_t g_state_mutex;

// Cola de comandos de voz (Task 3 -> Task 4)
typedef struct {
    char cmd[32];
} vm_command_t;
static QueueHandle_t g_cmd_queue;

static vm_state_t vm_state_get(void)
{
    vm_state_t copy;
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    copy = g_state;
    xSemaphoreGive(g_state_mutex);
    return copy;
}

void vm_state_set_accel(int16_t x, int16_t y, int16_t z)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_state.accel_x = x; g_state.accel_y = y; g_state.accel_z = z;
    xSemaphoreGive(g_state_mutex);
}

void vm_state_set_battery(uint8_t pct, bool charging)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_state.battery_pct = pct; g_state.charging = charging;
    xSemaphoreGive(g_state_mutex);
}

void vm_state_set_location(double lat, double lon)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_state.lat = lat; g_state.lon = lon; g_state.gps_valid = true;
    xSemaphoreGive(g_state_mutex);
}

// ============================================================================
// FUNCIONES DE INICIALIZACION DE HARDWARE
// ============================================================================

esp_err_t init_i2c_bus(void) {
    i2c_master_bus_config_t i2c_mst_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_NUM,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&i2c_mst_config, &i2c_bus_0_handle);
}

esp_err_t init_i2c_touch_bus(void) {
    i2c_master_bus_config_t i2c_mst_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_TOUCH_NUM,
        .scl_io_num = I2C_TOUCH_SCL_IO,
        .sda_io_num = I2C_TOUCH_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&i2c_mst_config, &i2c_touch_bus_handle);
}

esp_err_t init_spi_bus(void) {
    spi_bus_config_t buscfg = {
        .miso_io_num = TFT_MISO,
        .mosi_io_num = TFT_MOSI,
        .sclk_io_num = TFT_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_MAX_TRANSFER_SZ,
    };
    return spi_bus_initialize(TFT_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
}

esp_err_t init_i2s_mic(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_MIC_PORT, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_chan);
    if (err != ESP_OK) return err;

    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = I2S_MIC_CLK,
            .din = I2S_MIC_DATA,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    // El canal queda inicializado pero DESHABILITADO (push-to-talk, Task 3):
    // la tarea del micrófono hará i2s_channel_enable() al empezar a grabar
    // y i2s_channel_disable() al terminar, para no gastar batería.
    return i2s_channel_init_pdm_rx_mode(rx_chan, &pdm_rx_cfg);
}

void init_gpios(void) {
    uint64_t irq_pin_sel = (1ULL<<PMU_IRQ_PIN) | (1ULL<<RTC_IRQ_PIN) | (1ULL<<TOUCH_IRQ_PIN) | (1ULL<<BMA_IRQ_PIN);
    gpio_config_t io_conf_irq = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = irq_pin_sel,
        .pull_up_en = 0,
        .pull_down_en = 0
    };
    gpio_config(&io_conf_irq);

    uint64_t output_pin_sel = (1ULL<<MOTOR_PIN) | (1ULL<<TFT_DC);
    gpio_config_t io_conf_out = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = output_pin_sel,
        .pull_down_en = 0,
        .pull_up_en = 0
    };
    gpio_config(&io_conf_out);

    gpio_set_level(MOTOR_PIN, 0); // Apagar motor
}

// Comprueba qué dispositivos I2C responden (diagnóstico de arranque)
static void scan_i2c_devices(void)
{
    struct { i2c_master_bus_handle_t bus; uint8_t addr; const char *name; } devs[] = {
        { i2c_bus_0_handle,     ADDR_AXP202,  "AXP202 (PMU)"   },
        { i2c_bus_0_handle,     ADDR_BMA423,  "BMA423 (acel.)" },
        { i2c_bus_0_handle,     ADDR_PCF8563, "PCF8563 (RTC)"  },
        { i2c_touch_bus_handle, ADDR_FT6336,  "FT6336 (touch)" },
    };
    for (size_t i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        if (devs[i].bus == NULL) continue;
        esp_err_t err = i2c_master_probe(devs[i].bus, devs[i].addr, 100);
        if (err == ESP_OK) ESP_LOGI(TAG, "  [OK]  %s en 0x%02X", devs[i].name, devs[i].addr);
        else               ESP_LOGW(TAG, "  [--]  %s en 0x%02X no responde", devs[i].name, devs[i].addr);
    }
}

// Enciende los rails mínimos del AXP202 para pantalla. La gestión completa
// de energía (batería, sleep, apagar periféricos) se hace en Task 8.
static esp_err_t pmu_init(void)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ADDR_AXP202,
        .scl_speed_hz    = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(i2c_bus_0_handle, &dev_cfg, &pmu_dev_handle);
    if (err != ESP_OK) return err;

    uint8_t reg = AXP202_REG_POWER_OUT_CTRL;
    uint8_t val = 0;
    err = i2c_master_transmit_receive(pmu_dev_handle, &reg, 1, &val, 1, 100);
    if (err != ESP_OK) return err;

    val |= AXP202_LDO2_BIT;
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(pmu_dev_handle, buf, sizeof(buf), 100);
}

// ============================================================================
// WIFI con prioridades (1º hogar, 2º hotspot del móvil)
// ============================================================================
typedef struct { const char *ssid; const char *pass; } wifi_net_t;

static const wifi_net_t WIFI_NETS[] = {
    { WIFI_SSID_HOME,    WIFI_PASS_HOME    },
    { WIFI_SSID_HOTSPOT, WIFI_PASS_HOTSPOT },
};
#define WIFI_NET_COUNT (sizeof(WIFI_NETS) / sizeof(WIFI_NETS[0]))

static int s_wifi_net_idx = 0;
static int s_wifi_retries = 0;
static esp_timer_handle_t s_wifi_retry_timer;

static void wifi_apply_network(int idx)
{
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid,     WIFI_NETS[idx].ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, WIFI_NETS[idx].pass, sizeof(cfg.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    ESP_LOGI(TAG, "WiFi: red objetivo = '%s' (prioridad %d)", WIFI_NETS[idx].ssid, idx + 1);
}

static void wifi_retry_timer_cb(void *arg)
{
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_events, WIFI_CONNECTED_BIT);

        uint64_t delay_us = 2ULL * 1000000ULL;
        if (++s_wifi_retries >= WIFI_RETRIES_PER_NET) {
            s_wifi_retries = 0;
            s_wifi_net_idx = (s_wifi_net_idx + 1) % WIFI_NET_COUNT;
            wifi_apply_network(s_wifi_net_idx);
            // Si hemos recorrido todas las redes, esperamos más para ahorrar batería
            if (s_wifi_net_idx == 0) delay_us = 30ULL * 1000000ULL;
        }
        esp_timer_stop(s_wifi_retry_timer);
        esp_timer_start_once(s_wifi_retry_timer, delay_us);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi conectado, IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_wifi_retries = 0;
        xEventGroupSetBits(g_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init(void)
{
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) return err;

    const esp_timer_create_args_t timer_args = {
        .callback = wifi_retry_timer_cb,
        .name = "wifi_retry",
    };
    err = esp_timer_create(&timer_args, &s_wifi_retry_timer);
    if (err != ESP_OK) return err;

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_apply_network(0);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);   // ahorro de energía del modem
    return esp_wifi_start();
}

// ============================================================================
// OTA (Task 1): validación/rollback, comprobación de versión y actualización
// ============================================================================

// Si el firmware recién actualizado arranca bien, se confirma; si no, vuelve al anterior.
// Requiere CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y.
static void ota_confirm_or_rollback(bool healthy)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return;

    if (healthy) {
        ESP_LOGI(TAG, "OTA: firmware nuevo validado.");
        esp_ota_mark_app_valid_cancel_rollback();
    } else {
        ESP_LOGE(TAG, "OTA: fallo crítico en el arranque, volviendo al firmware anterior.");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

static bool version_is_newer(const char *remote, const char *local)
{
    int r[3] = {0}, l[3] = {0};
    if (sscanf(remote, "%d.%d.%d", &r[0], &r[1], &r[2]) != 3) return false;
    if (sscanf(local,  "%d.%d.%d", &l[0], &l[1], &l[2]) != 3) return false;
    for (int i = 0; i < 3; i++) {
        if (r[i] != l[i]) return r[i] > l[i];
    }
    return false;
}

static esp_err_t ota_fetch_remote_version(char *out, size_t len)
{
    esp_http_client_config_t cfg = {
        .url = VERSION_URL,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    int n = (status == 200) ? esp_http_client_read(client, out, len - 1) : -1;

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (n <= 0) return ESP_FAIL;

    out[n] = '\0';
    while (n > 0 && isspace((unsigned char)out[n - 1])) out[--n] = '\0';
    return ESP_OK;
}

static void ota_check_and_update(void)
{
    char remote[32];
    if (ota_fetch_remote_version(remote, sizeof(remote)) != ESP_OK) {
        ESP_LOGW(TAG, "OTA: no se pudo consultar la versión remota.");
        return;
    }
    ESP_LOGI(TAG, "OTA: versión actual %s, remota %s", CURRENT_FW_VERSION, remote);

    if (!version_is_newer(remote, CURRENT_FW_VERSION)) return;

    ESP_LOGI(TAG, "OTA: descargando nueva versión...");
    esp_http_client_config_t http_cfg = {
        .url = FIRMWARE_URL,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA: actualización correcta, reiniciando.");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA: fallo en la actualización (%s).", esp_err_to_name(err));
    }
}

void task_ota(void *pvParameters)
{
    while (1) {
        // Espera a tener WiFi; al conectar por primera vez comprueba al arranque
        xEventGroupWaitBits(g_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        ota_check_and_update();
        vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_PERIOD_MS));
    }
}

// ============================================================================
// TELEMETRÍA (Task 4): MQTT hacia ThingsBoard
// ============================================================================
static esp_mqtt_client_handle_t s_mqtt = NULL;

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT conectado a ThingsBoard.");
        xEventGroupSetBits(g_events, MQTT_CONNECTED_BIT);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT desconectado.");
        xEventGroupClearBits(g_events, MQTT_CONNECTED_BIT);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error.");
        break;
    default:
        break;
    }
}

static esp_err_t mqtt_start(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = THINGSBOARD_URI,
        .broker.address.port = THINGSBOARD_PORT,
        .credentials.username = ACCESS_TOKEN,
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    if (!s_mqtt) return ESP_FAIL;

    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    return esp_mqtt_client_start(s_mqtt);
}

// El llamante debe liberar el resultado con free()
static char *build_telemetry_json(void)
{
    vm_state_t s = vm_state_get();
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "accel_x", s.accel_x);
    cJSON_AddNumberToObject(root, "accel_y", s.accel_y);
    cJSON_AddNumberToObject(root, "accel_z", s.accel_z);
    cJSON_AddNumberToObject(root, "battery", s.battery_pct);
    cJSON_AddBoolToObject(root, "charging", s.charging);
    if (s.gps_valid) {
        cJSON_AddNumberToObject(root, "latitude", s.lat);
        cJSON_AddNumberToObject(root, "longitude", s.lon);
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

static void publish_json(const char *json)
{
    if (esp_mqtt_client_publish(s_mqtt, TELEMETRY_TOPIC, json, 0, 1, 0) < 0) {
        ESP_LOGW(TAG, "MQTT: fallo al publicar.");
    }
}

void task_telemetry(void *pvParameters)
{
    // Espera la primera conexión WiFi y arranca MQTT (reconecta solo)
    xEventGroupWaitBits(g_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    if (mqtt_start() != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo arrancar el cliente MQTT.");
        vTaskDelete(NULL);
    }

    TickType_t last_wake = xTaskGetTickCount();
    while (1) {
        bool online = (xEventGroupGetBits(g_events) & MQTT_CONNECTED_BIT) != 0;

        // Comandos de voz pendientes
        vm_command_t cmd;
        while (xQueueReceive(g_cmd_queue, &cmd, 0) == pdTRUE) {
            if (online) {
                cJSON *c = cJSON_CreateObject();
                cJSON_AddStringToObject(c, "command", cmd.cmd);
                char *json = cJSON_PrintUnformatted(c);
                cJSON_Delete(c);
                if (json) { publish_json(json); free(json); }
            } else {
                ESP_LOGW(TAG, "Sin conexión: comando '%s' pendiente de guardar (TODO store&forward)", cmd.cmd);
            }
        }

        // Telemetría periódica
        char *json = build_telemetry_json();
        if (json) {
            if (online) {
                publish_json(json);
            } else {
                // TODO (Task 4): guardar en SPIFFS/LittleFS y reenviar al reconectar
                ESP_LOGW(TAG, "Sin conexión: telemetría descartada (TODO store&forward)");
            }
            free(json);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));
    }
}

// ============================================================================
// TAREAS PENDIENTES (esqueletos)
// ============================================================================
void task_accelerometer(void *pvParameters) { while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); } }
void task_microphone(void *pvParameters) { while(1) { vTaskDelay(pdMS_TO_TICKS(100)); } }
void task_display(void *pvParameters) { while(1) { vTaskDelay(pdMS_TO_TICKS(500)); } }
void task_rtc(void *pvParameters) { while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); } }
void task_ble(void *pvParameters) { while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); } }
void task_power_management(void *pvParameters) { while(1) { vTaskDelay(pdMS_TO_TICKS(5000)); } }

// ============================================================================
// FUNCIÓN PRINCIPAL (Task 0)
// ============================================================================

void app_main(void)
{
    ESP_LOGI(TAG, "Iniciando firmware VitaMove v%s para LILYGO T-Watch 2020 V3...", CURRENT_FW_VERSION);

    bool critical_ok = true;   // si algo crítico falla y estamos en OTA pendiente -> rollback

    // --- NVS (necesario para WiFi) ---
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Fallo al inicializar NVS (%s).", esp_err_to_name(ret));
        critical_ok = false;
    }

    // --- Primitivas de sincronización ---
    g_state_mutex = xSemaphoreCreateMutex();
    g_events      = xEventGroupCreate();
    g_cmd_queue   = xQueueCreate(8, sizeof(vm_command_t));
    if (!g_state_mutex || !g_events || !g_cmd_queue) {
        ESP_LOGE(TAG, "No hay memoria para las primitivas de FreeRTOS.");
        critical_ok = false;
    }

    // --- Pila de red ---
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // --- Hardware ---
    init_gpios();
    ESP_LOGI(TAG, "GPIOs inicializados.");

    if (init_i2c_bus() == ESP_OK) {
        ESP_LOGI(TAG, "Bus I2C 0 inicializado (PMU, Acel, RTC).");
    } else {
        ESP_LOGE(TAG, "Fallo al inicializar el bus I2C 0.");
        critical_ok = false;
    }

    if (init_i2c_touch_bus() == ESP_OK) {
        ESP_LOGI(TAG, "Bus I2C 1 inicializado (Panel Táctil).");
    } else {
        ESP_LOGE(TAG, "Fallo al inicializar el bus I2C 1.");
    }

    if (init_spi_bus() == ESP_OK) {
        ESP_LOGI(TAG, "Bus SPI inicializado para la pantalla.");
    } else {
        ESP_LOGE(TAG, "Fallo al inicializar el bus SPI.");
    }

    if (init_i2s_mic() == ESP_OK) {
        ESP_LOGI(TAG, "Bus I2S (Micrófono PDM) inicializado (canal en reposo).");
    } else {
        ESP_LOGE(TAG, "Fallo al inicializar el micrófono.");
    }

    // --- Diagnóstico I2C y PMU ---
    if (i2c_bus_0_handle) {
        ESP_LOGI(TAG, "Dispositivos I2C:");
        scan_i2c_devices();

        if (pmu_init() == ESP_OK) {
            ESP_LOGI(TAG, "PMU AXP202: rail de pantalla (LDO2) activado.");
        } else {
            ESP_LOGE(TAG, "Fallo al configurar el PMU AXP202.");
        }
    }

    // --- WiFi (conecta en segundo plano con prioridades) ---
    if (critical_ok) {
        if (wifi_init() == ESP_OK) {
            ESP_LOGI(TAG, "WiFi iniciado.");
        } else {
            ESP_LOGE(TAG, "Fallo al iniciar WiFi.");
        }
    }

    // --- Confirmar (o revertir) el firmware si venimos de una OTA ---
    ota_confirm_or_rollback(critical_ok);
    if (!critical_ok) {
        ESP_LOGE(TAG, "Arranque con fallos críticos, no se lanzan las tareas.");
        return;
    }

    // --- Tareas ---
    xTaskCreate(task_power_management, "Power_Task", 4096, NULL, 6, NULL);
    xTaskCreate(task_display, "Display_Task", 4096, NULL, 5, NULL);
    xTaskCreate(task_accelerometer, "Accel_Task", 4096, NULL, 5, NULL);
    xTaskCreate(task_microphone, "Mic_Task", 4096, NULL, 5, NULL);
    xTaskCreate(task_rtc, "RTC_Task", 4096, NULL, 5, NULL);
    xTaskCreate(task_telemetry, "Telemetry_Task", 8192, NULL, 4, NULL);
    xTaskCreate(task_ble, "BLE_Task", 4096, NULL, 4, NULL);
    xTaskCreate(task_ota, "OTA_Task", 10240, NULL, 3, NULL);
}
