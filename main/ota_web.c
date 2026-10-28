#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

#include "ota_web.h"

#define WIFI_SSID       "GR02-OTA"
#define WIFI_PASSWORD   "GR02_OTA"
#define WIFI_CHANNEL    1
#define WIFI_MAX_CONN   2

#define OTA_BUFFER_SIZE 4096
#define OTA_RECV_RETRIES 5

static const char *TAG = "OTA_WEB";

/*
 * Pagina HTML incorporada ao firmware.
 *
 * O progresso representa o envio HTTP dos bytes.
 * A mensagem de sucesso so aparece quando o ESP32
 * confirma a gravacao e a validacao do firmware.
 */
static const char html_page[] =
"<!DOCTYPE html>"
"<html lang='pt-br'>"
"<head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"
"<title>ESP32 OTA</title>"
"<style>"
"body{font-family:Arial;background:#101827;color:#fff;"
"max-width:520px;margin:50px auto;padding:20px}"
".card{background:#202c40;padding:25px;border-radius:12px}"
"h2{margin-top:0}"
"input{max-width:100%;margin:15px 0}"
"button{background:#1685d4;color:white;border:0;"
"padding:12px 20px;border-radius:6px;cursor:pointer}"
"button:disabled{background:#64748b;cursor:default}"
"progress{width:100%;height:24px;margin-top:15px}"
"#status{margin-top:18px;white-space:pre-wrap;line-height:1.5}"
"</style>"
"</head>"
"<body>"
"<div class='card'>"
"<h2>ESP32 - Atualizacao OTA</h2>"
"<p>Selecione o novo firmware (.bin).</p>"
"<input type='file' id='firmware' accept='.bin'>"
"<br>"
"<button id='send' onclick='upload()'>Atualizar firmware</button>"
"<progress id='bar' value='0' max='100'></progress>"
"<p id='percent'>0%</p>"
"<div id='status'>Aguardando firmware...</div>"
"</div>"
"<script>"
"function upload(){"
"const file=document.getElementById('firmware').files[0];"
"const status=document.getElementById('status');"
"const bar=document.getElementById('bar');"
"const percent=document.getElementById('percent');"
"const button=document.getElementById('send');"

"if(!file){alert('Selecione um arquivo .bin');return;}"
"if(!file.name.toLowerCase().endsWith('.bin')){"
"alert('Arquivo invalido');return;}"

"button.disabled=true;"
"bar.value=0;"
"percent.innerText='0%';"
"status.innerText='Enviando firmware...';"

"const xhr=new XMLHttpRequest();"
"xhr.open('POST','/update',true);"
"xhr.setRequestHeader('Content-Type','application/octet-stream');"
"xhr.timeout=120000;"

"xhr.upload.onprogress=function(e){"
"if(e.lengthComputable){"
"const p=Math.round(e.loaded*100/e.total);"
"bar.value=p;"
"percent.innerText=p+'%';"
"if(p===100) status.innerText="
"'Upload concluido. Validando gravacao no ESP32...';"
"}};"

"xhr.onload=function(){"
"if(xhr.status===200){"
"bar.value=100;"
"percent.innerText='100%';"
"status.innerText=xhr.responseText;"
"}else{"
"status.innerText='FALHA: '+xhr.responseText;"
"button.disabled=false;"
"}};"

"xhr.onerror=function(){"
"status.innerText='Falha de conexao com ESP32.';"
"button.disabled=false;"
"};"

"xhr.ontimeout=function(){"
"status.innerText='Tempo limite excedido.';"
"button.disabled=false;"
"};"

"xhr.send(file);"
"}"
"</script>"
"</body>"
"</html>";


static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    vTaskDelete(NULL);
}


static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    return httpd_resp_send(
        req,
        html_page,
        HTTPD_RESP_USE_STRLEN
    );
}


static esp_err_t ota_error(
    httpd_req_t *req,
    const char *http_status,
    const char *message,
    esp_err_t err)
{
    ESP_LOGE(TAG, "%s: %s", message, esp_err_to_name(err));

    httpd_resp_set_status(req, http_status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");

    char response[160];

    snprintf(
        response,
        sizeof(response),
        "%s (0x%x)",
        message,
        (unsigned int)err
    );

    return httpd_resp_sendstr(req, response);
}

static esp_err_t update_handler(httpd_req_t *req)
{
    esp_err_t err;

    const esp_partition_t *partition =
        esp_ota_get_next_update_partition(NULL);

    if (partition == NULL) {
        return ota_error(req, "500 Internal Server Error",
                         "Particao OTA nao encontrada",
                         ESP_ERR_NOT_FOUND);
    }

    const size_t firmware_size = req->content_len;

    if (firmware_size == 0 ||
        firmware_size > partition->size) {

        return ota_error(req, "400 Bad Request",
                         "Firmware vazio ou maior que a particao",
                         ESP_ERR_INVALID_SIZE);
    }

    ESP_LOGI(TAG, "Firmware: %u bytes",
             (unsigned int)firmware_size);

    ESP_LOGI(TAG, "Particao destino: %s",
             partition->label);

    char buffer[OTA_BUFFER_SIZE];

    size_t received = 0;
    int retries = 0;

    esp_ota_handle_t ota_handle = 0;

    // Prepara a particao de destino
    err = esp_ota_begin(
        partition,
        firmware_size,
        &ota_handle
    );

    if (err != ESP_OK) {
        return ota_error(req, "500 Internal Server Error",
                         "Falha ao iniciar gravacao OTA", err);
    }

    bool failed = false;
    const char *failure_message = "Erro desconhecido";
    esp_err_t failure_code = ESP_FAIL;

    while (received < firmware_size) {

        size_t remaining = firmware_size - received;

        int to_read = remaining > sizeof(buffer)
                      ? sizeof(buffer)
                      : (int)remaining;

        int len = httpd_req_recv(
            req,
            buffer,
            to_read
        );

        if (len == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++retries <= OTA_RECV_RETRIES) {
                continue;
            }

            failed = true;
            failure_message = "Timeout durante upload";
            failure_code = ESP_ERR_TIMEOUT;
            break;
        }

        if (len <= 0) {
            failed = true;
            failure_message = "Conexao interrompida no upload";
            failure_code = ESP_FAIL;
            break;
        }

        retries = 0;

        // Verificacao inicial do formato ESP32
        if (received == 0 && (uint8_t)buffer[0] != 0xE9) {
            failed = true;
            failure_message = "Arquivo nao e um firmware ESP valido";
            failure_code = ESP_ERR_INVALID_ARG;
            break;
        }

        // Gravacao direta na flash
        err = esp_ota_write(
            ota_handle,
            buffer,
            len
        );

        if (err != ESP_OK) {
            failed = true;
            failure_message = "Falha na escrita da flash";
            failure_code = err;
            break;
        }

        received += len;

        ESP_LOGI(
            TAG,
            "OTA: %u / %u bytes (%u%%)",
            (unsigned int)received,
            (unsigned int)firmware_size,
            (unsigned int)((received * 100ULL) / firmware_size)
        );
    }

    if (failed) {
        esp_ota_abort(ota_handle);

        return ota_error(
            req,
            "500 Internal Server Error",
            failure_message,
            failure_code
        );
    }

    // Finaliza e valida a imagem
    err = esp_ota_end(ota_handle);

    if (err != ESP_OK) {
        return ota_error(
            req,
            "500 Internal Server Error",
            "Firmware invalido ou erro de validacao",
            err
        );
    }

    // Define a particao para o proximo boot
    err = esp_ota_set_boot_partition(partition);

    if (err != ESP_OK) {
        return ota_error(
            req,
            "500 Internal Server Error",
            "Falha ao selecionar firmware de boot",
            err
        );
    }

    ESP_LOGI(TAG, "OTA concluida com sucesso!");

    httpd_resp_set_type(
        req,
        "text/plain; charset=utf-8"
    );

    esp_err_t response_err = httpd_resp_sendstr(
        req,
        "Firmware gravado e validado com sucesso!\n"
        "O ESP32 sera reiniciado automaticamente."
    );

    // Reinicia fora do handler para permitir
    // o envio da resposta HTTP.
    xTaskCreate(
        restart_task,
        "restart_task",
        2048,
        NULL,
        5,
        NULL
    );

    return response_err;
}

static esp_err_t start_http_server(void)
{
    httpd_handle_t server = NULL;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.server_port = 80;
    config.stack_size = 8192;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;

    esp_err_t err = httpd_start(&server, &config);

    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL
    };

    httpd_uri_t update = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = update_handler,
        .user_ctx = NULL
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &update));

    ESP_LOGI(TAG, "HTTP Server iniciado");

    return ESP_OK;
}

esp_err_t ota_web_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *ap_netif =
        esp_netif_create_default_wifi_ap();

    if (ap_netif == NULL) {
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = sizeof(WIFI_SSID) - 1,
            .password = WIFI_PASSWORD,
            .channel = WIFI_CHANNEL,
            .max_connection = WIFI_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false
            }
        }
    };

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_AP)
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_config(WIFI_IF_AP, &wifi_config)
    );

    ESP_ERROR_CHECK(
        esp_wifi_start()
    );

    ESP_LOGI(TAG, "SoftAP iniciado");
    ESP_LOGI(TAG, "SSID: %s", WIFI_SSID);
    ESP_LOGI(TAG, "IP: http://192.168.4.1");

    return start_http_server();
}