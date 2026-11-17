#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "VitaMove_Main";

void task_ota(void *pvParameters) {
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void task_accelerometer(void *pvParameters) {
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void task_microphone(void *pvParameters) {
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void task_telemetry(void *pvParameters) {
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void task_display(void *pvParameters) {
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void task_rtc(void *pvParameters) {
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void task_ble(void *pvParameters) {
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void task_power_management(void *pvParameters) {
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Iniciando firmware VitaMove para LILYGO T-Watch 2020 V3...");

    // Task 0 - Main: Configuración inicial
    ESP_LOGI(TAG, "Ejecutando configuración inicial de puertos, sensores y módulos...");
    // TODO: Inicializar buses (I2C, SPI), UART, pines y periféricos base aquí

    ESP_LOGI(TAG, "Comprobación de actualizaciones disponibles al arranque...");
    // TODO: Leer estado de particiones OTA y verificar si hay actualizaciones pendientes

    // Inicialización de las tareas de FreeRTOS basadas en los requerimientos
    
    // Task 1 - OTA (Over The Air)
    xTaskCreate(task_ota, "OTA_Task", 4096, NULL, 5, NULL);
    
    // Task 2 - Acelerómetro (BMA423)
    xTaskCreate(task_accelerometer, "Accel_Task", 4096, NULL, 5, NULL);
    
    // Task 3 - Micrófono (SPM1423HM4H-B)
    xTaskCreate(task_microphone, "Mic_Task", 4096, NULL, 5, NULL);
    
    // Task 4 - WiFi / Telemetría (Store & Forward)
    xTaskCreate(task_telemetry, "Telemetry_Task", 8192, NULL, 5, NULL);
    
    // Task 5 - Pantalla e Interfaz (ST7789V & FT6336)
    xTaskCreate(task_display, "Display_Task", 4096, NULL, 5, NULL);
    
    // Task 6 - Reloj / Alarmas (RTC PCF8563)
    xTaskCreate(task_rtc, "RTC_Task", 4096, NULL, 5, NULL);
    
    // Task 7 - BLE y Ubicación
    xTaskCreate(task_ble, "BLE_Task", 4096, NULL, 5, NULL);
    
    // Task 8 - Gestión de Energía (AXP202)
    xTaskCreate(task_power_management, "Power_Task", 4096, NULL, 5, NULL);
}


