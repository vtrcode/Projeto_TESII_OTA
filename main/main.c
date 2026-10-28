#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "ota_web.h"

// Altere conforme o LED da sua placa.
// Exemplo: algumas placas ESP32 usam GPIO2.
#define LED_GPIO GPIO_NUM_2

static const char *TAG = "MAIN";

void app_main(void)
{
    // Inicializar NVS (necessario para o Wi-Fi)
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    // Configurar GPIO do LED
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));

    // Inicializar SoftAP e WebServer OTA
    ESP_ERROR_CHECK(ota_web_start());

    ESP_LOGI(TAG, "Aplicacao iniciada");

    // Aplicacao principal: piscar LED
    while (1) {

        gpio_set_level(LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(50));

        gpio_set_level(LED_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}