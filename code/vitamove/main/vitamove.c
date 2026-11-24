#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "driver/gpio.h"

// Buses and peripherals
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/i2s_pdm.h"

// WiFi and telemetry
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_spiffs.h"

// ---- Task 1: OTA y rollback ----
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"            // esp_ota_mark_app_valid_cancel_rollback()
#include "esp_crt_bundle.h"         // certificados raíz para HTTPS

// ---- Task 5: pantalla, táctil y LVGL ----
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"   // ST7789 (comprobar en v6, ver nota)
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_ft5x06.h"   // espressif/esp_lcd_touch_ft5x06
#include "lvgl.h"
#include "esp_lvgl_port.h"          // espressif/esp_lvgl_port

// ---- Task 7: BLE (elige UNA pila) ----
// NimBLE (más ligera, recomendada):
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// ---- Task 6 y 8: hora y energía ----
#include <time.h>
#include <sys/time.h>
#include "esp_sleep.h"              // deep sleep / light sleep
#include "esp_pm.h"

#define THINGSBOARD_URI "mqtt://eu.thingsboard.cloud"
#define ACCESS_TOKEN    "PLACEHOLDER"
#define WIFI_SSID       "SBC"
#define WIFI_PASS       "SBCwifi&"

#define CURRENT_FW_VERSION  "1.0.0"
#define VERSION_URL         "PLACEHOLDER"
#define FIRMWARE_URL        "PLACEHOLDER"


static const char *TAG = "VitaMove_Main";
EventGroupHandle_t network_event_group;

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
// HANDLES GLOBALES (Para usarlos luego en las tareas)
// ============================================================================
i2c_master_bus_handle_t i2c_bus_0_handle;
i2c_master_bus_handle_t i2c_touch_bus_handle;
i2s_chan_handle_t rx_chan; // Handle del micrófono PDM

// ============================================================================
// FUNCIONES DE INICIALIZACIÓN
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
    err = i2s_channel_init_pdm_rx_mode(rx_chan, &pdm_rx_cfg);
    if (err != ESP_OK) return err;

    return i2s_channel_enable(rx_chan); // Habilitamos el canal
}

void init_gpios(void) {
    uint64_t irq_pin_sel = (1ULL<<PMU_IRQ_PIN) | (1ULL<<RTC_IRQ_PIN) | (1ULL<<TOUCH_IRQ_PIN) | (1ULL<<BMA_IRQ_PIN);
    gpio_config_t io_conf_irq = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = irq_pin_sel,
        .pull_up_en = 0,    // <--- CAMBIAR ESTO A 0
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


void task_ota(void *pvParameters) 
{
    while(1) 
    {
        vTaskDelay(pdMS_TO_TICKS(10000)); 
    } 
}




void task_accelerometer(void *pvParameters) { while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); } }
void task_microphone(void *pvParameters) { while(1) { vTaskDelay(pdMS_TO_TICKS(100)); } }
void task_telemetry(void *pvParameters) { while(1) { vTaskDelay(pdMS_TO_TICKS(5000)); } }
void task_display(void *pvParameters) { while(1) { vTaskDelay(pdMS_TO_TICKS(500)); } }
void task_rtc(void *pvParameters) { while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); } }
void task_ble(void *pvParameters) { while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); } }
void task_power_management(void *pvParameters) { while(1) { vTaskDelay(pdMS_TO_TICKS(5000)); } }

// ============================================================================
// FUNCIÓN PRINCIPAL
// ============================================================================

void app_main(void)
{
    ESP_LOGI(TAG, "Iniciando firmware VitaMove para LILYGO T-Watch 2020 V3...");

    init_gpios();
    ESP_LOGI(TAG, "GPIOs inicializados.");

    if (init_i2c_bus() == ESP_OK) {
        ESP_LOGI(TAG, "Bus I2C 0 inicializado (PMU, Acel, RTC).");
    } else {
        ESP_LOGE(TAG, "Fallo al inicializar el bus I2C 0.");
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
        ESP_LOGI(TAG, "Bus I2S (Micrófono PDM) inicializado.");
    } else {
        ESP_LOGE(TAG, "Fallo al inicializar el micrófono.");
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
       ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    network_event_group = xEventGroupCreate();

    // Config mqtt client with credentials
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = THINGSBOARD_URI,
        .broker.address.port = 1883,
        .credentials.username = ACCESS_TOKEN
    };

    xTaskCreate(task_power_management, "Power_Task", 4096, NULL, 6, NULL);
    xTaskCreate(task_display, "Display_Task", 4096, NULL, 5, NULL);
    xTaskCreate(task_accelerometer, "Accel_Task", 4096, NULL, 5, NULL);
    xTaskCreate(task_microphone, "Mic_Task", 4096, NULL, 5, NULL);
    xTaskCreate(task_rtc, "RTC_Task", 4096, NULL, 5, NULL);
    xTaskCreate(task_telemetry, "Telemetry_Task", 8192, NULL, 4, NULL);
    xTaskCreate(task_ble, "BLE_Task", 4096, NULL, 4, NULL);
    xTaskCreate(task_ota, "OTA_Task", 4096, NULL, 3, NULL);
}

