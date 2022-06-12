/*
 * SPDX-FileCopyrightText: 2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* ADC1 Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_eth.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_tls_crypto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>
#include <soc/adc_channel.h>

static const char *TAG = "BBQ";

static double s_probe1_temp_f = 0;
static double s_probe2_temp_f = 0;
static int s_duty = 30;
static esp_adc_cal_characteristics_t s_adc1_chars;

#define ADC1_FROM_GPIO_IMPL(gpio) ADC1_GPIO##gpio##_CHANNEL
#define ADC1_FROM_GPIO(gpio) ADC1_FROM_GPIO_IMPL(gpio)

#define ADC1_PROBE1_CHAN ADC1_FROM_GPIO(CONFIG_PROBE1_GPIO)
#define ADC1_PROBE2_CHAN ADC1_FROM_GPIO(CONFIG_PROBE2_GPIO)
#define ADC1_VREF_CHAN ADC1_FROM_GPIO(CONFIG_VREF_GPIO)

#define FAN_CONTROL_GPIO CONFIG_FAN_CONTROL_GPIO

#define PROBE_RESISTOR_OHMS CONFIG_PROBE_RESISTOR_OHMS


// ADC Attenuation
#define ADC_EXAMPLE_ATTEN ADC_ATTEN_DB_11

// ADC Calibration
#if CONFIG_IDF_TARGET_ESP32
#define ADC_EXAMPLE_CALI_SCHEME ESP_ADC_CAL_VAL_EFUSE_VREF
#elif CONFIG_IDF_TARGET_ESP32S2
#define ADC_EXAMPLE_CALI_SCHEME ESP_ADC_CAL_VAL_EFUSE_TP
#elif CONFIG_IDF_TARGET_ESP32C3
#define ADC_EXAMPLE_CALI_SCHEME ESP_ADC_CAL_VAL_EFUSE_TP
#elif CONFIG_IDF_TARGET_ESP32S3
#define ADC_EXAMPLE_CALI_SCHEME ESP_ADC_CAL_VAL_EFUSE_TP_FIT
#endif

static double STEINHART_COEFFS[] = {7.3431401e-4, 2.1574370e-4, 9.5156860e-8};


void start_mdns_service()
{
    // initialize mDNS service
    esp_err_t err = mdns_init();
    if (err)
    {
        printf("MDNS Init failed: %d\n", err);
        return;
    }

    // set hostname
    mdns_hostname_set("bbq");
    // set default instance
    mdns_instance_name_set("BBQ Thing");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

/* Simple HTTP Server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/* A simple example that demonstrates how to create GET and POST
 * handlers for the web server.
 */

/* An HTTP GET handler */
static esp_err_t hello_get_handler(httpd_req_t *req)
{
    char *buf;
    size_t buf_len;

    /* Get header value string length and allocate memory for length + 1,
     * extra byte for null termination */
    buf_len = httpd_req_get_hdr_value_len(req, "Host") + 1;
    if (buf_len > 1)
    {
        buf = malloc(buf_len);
        /* Copy null terminated value string into buffer */
        if (httpd_req_get_hdr_value_str(req, "Host", buf, buf_len) == ESP_OK)
        {
            ESP_LOGI(TAG, "Found header => Host: %s", buf);
        }
        free(buf);
    }

    buf_len = httpd_req_get_hdr_value_len(req, "Test-Header-2") + 1;
    if (buf_len > 1)
    {
        buf = malloc(buf_len);
        if (httpd_req_get_hdr_value_str(req, "Test-Header-2", buf, buf_len) == ESP_OK)
        {
            ESP_LOGI(TAG, "Found header => Test-Header-2: %s", buf);
        }
        free(buf);
    }

    buf_len = httpd_req_get_hdr_value_len(req, "Test-Header-1") + 1;
    if (buf_len > 1)
    {
        buf = malloc(buf_len);
        if (httpd_req_get_hdr_value_str(req, "Test-Header-1", buf, buf_len) == ESP_OK)
        {
            ESP_LOGI(TAG, "Found header => Test-Header-1: %s", buf);
        }
        free(buf);
    }

    /* Read URL query string length and allocate memory for length + 1,
     * extra byte for null termination */
    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1)
    {
        buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK)
        {
            ESP_LOGI(TAG, "Found URL query => %s", buf);
            char param[32];
            /* Get value of expected key from query string */
            if (httpd_query_key_value(buf, "duty", param, sizeof(param)) == ESP_OK)
            {
                ESP_LOGI(TAG, "Found URL query parameter => duty=%s", param);
                s_duty = atoi(param);
            }
            if (httpd_query_key_value(buf, "query3", param, sizeof(param)) == ESP_OK)
            {
                ESP_LOGI(TAG, "Found URL query parameter => query3=%s", param);
            }
            if (httpd_query_key_value(buf, "query2", param, sizeof(param)) == ESP_OK)
            {
                ESP_LOGI(TAG, "Found URL query parameter => query2=%s", param);
            }
        }
        free(buf);
    }

    /* Set some custom headers */
    httpd_resp_set_hdr(req, "Custom-Header-1", "Custom-Value-1");
    httpd_resp_set_hdr(req, "Custom-Header-2", "Custom-Value-2");

    /* Send response with custom headers and body set as the
     * string passed in user context*/
    const char *resp_str = (const char *)req->user_ctx;
    char str[80];

    sprintf(str, "Temp: %f\nDuty: %d", 666.0, s_duty);

    httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);

    /* After sending the HTTP response the old HTTP request
     * headers are lost. Check if HTTP request headers can be read now. */
    if (httpd_req_get_hdr_value_len(req, "Host") == 0)
    {
        ESP_LOGI(TAG, "Request headers lost");
    }
    return ESP_OK;
}

static const httpd_uri_t hello = {
    .uri = "/hello",
    .method = HTTP_GET,
    .handler = hello_get_handler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx = "Hello World!"};

/* An HTTP POST handler */
static esp_err_t echo_post_handler(httpd_req_t *req)
{
    char buf[100];
    int ret, remaining = req->content_len;

    while (remaining > 0)
    {
        /* Read the data for the request */
        if ((ret = httpd_req_recv(req, buf,
                                  MIN(remaining, sizeof(buf)))) <= 0)
        {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT)
            {
                /* Retry receiving if timeout occurred */
                continue;
            }
            return ESP_FAIL;
        }

        /* Send back the same data */
        httpd_resp_send_chunk(req, buf, ret);
        remaining -= ret;

        /* Log data received */
        ESP_LOGI(TAG, "=========== RECEIVED DATA ==========");
        ESP_LOGI(TAG, "%.*s", ret, buf);
        ESP_LOGI(TAG, "====================================");
    }

    // End response
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static const httpd_uri_t echo = {
    .uri = "/echo",
    .method = HTTP_POST,
    .handler = echo_post_handler,
    .user_ctx = NULL};

/* This handler allows the custom error handling functionality to be
 * tested from client side. For that, when a PUT request 0 is sent to
 * URI /ctrl, the /hello and /echo URIs are unregistered and following
 * custom error handler http_404_error_handler() is registered.
 * Afterwards, when /hello or /echo is requested, this custom error
 * handler is invoked which, after sending an error message to client,
 * either closes the underlying socket (when requested URI is /echo)
 * or keeps it open (when requested URI is /hello). This allows the
 * client to infer if the custom error handler is functioning as expected
 * by observing the socket state.
 */
esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    if (strcmp("/hello", req->uri) == 0)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "/hello URI is not available");
        /* Return ESP_OK to keep underlying socket open */
        return ESP_OK;
    }
    else if (strcmp("/echo", req->uri) == 0)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "/echo URI is not available");
        /* Return ESP_FAIL to close underlying socket */
        return ESP_FAIL;
    }
    /* For any other URI send 404 and close socket */
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Some 404 error message");
    return ESP_FAIL;
}

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK)
    {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &hello);
        httpd_register_uri_handler(server, &echo);
#if CONFIG_EXAMPLE_BASIC_AUTH
        httpd_register_basic_auth(server);
#endif
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

static void stop_webserver(httpd_handle_t server)
{
    // Stop the httpd server
    httpd_stop(server);
}

static void disconnect_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    httpd_handle_t *server = (httpd_handle_t *)arg;
    if (*server)
    {
        ESP_LOGI(TAG, "Stopping webserver");
        stop_webserver(*server);
        *server = NULL;
    }
}

static void connect_handler(void *arg, esp_event_base_t event_base,
                            int32_t event_id, void *event_data)
{
    httpd_handle_t *server = (httpd_handle_t *)arg;
    if (*server == NULL)
    {
        ESP_LOGI(TAG, "Starting webserver");
        *server = start_webserver();
    }
}

static double probe_temp_f(double probe_mv, double ref_mv)
{
    // V = Vref * 2

    // V = Vprobe + Vresistor
    // Vresistor = V - VProbe
    // Vprobe / Rprobe = Vresistor / Rresistor
    // Rprobe = Vprobe * Rresistor / Vresistor
    // Rprobe = Vprobe * PROBE_RESISTOR_OHMS / (Vref*2 - Vprobe)
    double r_probe_ohm = probe_mv * PROBE_RESISTOR_OHMS / (ref_mv * 2 - probe_mv);

    double log_r = log(r_probe_ohm);
    double temp_k = 1.0 / (STEINHART_COEFFS[0] + STEINHART_COEFFS[1] * log_r + STEINHART_COEFFS[2] * log_r * log_r * log_r);
    return (temp_k - 273.15) * 9 / 5 + 32;
}

static void init(void)
{
    // Network.

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    start_mdns_service();

    static httpd_handle_t server = NULL;

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));

    /* Start the server for the first time */
    server = start_webserver();

    // Fan control.
    gpio_reset_pin(FAN_CONTROL_GPIO);
    gpio_set_direction(FAN_CONTROL_GPIO, GPIO_MODE_OUTPUT);

    // ADC
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_EXAMPLE_ATTEN, ADC_WIDTH_BIT_DEFAULT, 0, &s_adc1_chars);
    ESP_ERROR_CHECK(adc1_config_width(ADC_WIDTH_BIT_DEFAULT));
    ESP_ERROR_CHECK(adc1_config_channel_atten(ADC1_PROBE1_CHAN, ADC_EXAMPLE_ATTEN));
    ESP_ERROR_CHECK(adc1_config_channel_atten(ADC1_PROBE2_CHAN, ADC_EXAMPLE_ATTEN));
    ESP_ERROR_CHECK(adc1_config_channel_atten(ADC1_VREF_CHAN, ADC_EXAMPLE_ATTEN));
}

void app_main(void)
{
    init();

    int tick = 0;
    while (1)
    {
        /* Set the GPIO level according to the state (LOW or HIGH)*/
        bool set_fan = tick % CONFIG_DUTY_PERIOD < s_duty;
        gpio_set_level(FAN_CONTROL_GPIO, set_fan);
        ESP_LOGI(TAG, "setting fan: %d, duty = %d", (int)set_fan, s_duty);

        int probe1_raw = adc1_get_raw(ADC1_PROBE1_CHAN);
        int probe2_raw = adc1_get_raw(ADC1_PROBE2_CHAN);
        int vref_raw = adc1_get_raw(ADC1_VREF_CHAN);

        uint32_t probe1_voltage = esp_adc_cal_raw_to_voltage(probe1_raw, &s_adc1_chars);
        uint32_t probe2_voltage = esp_adc_cal_raw_to_voltage(probe2_raw, &s_adc1_chars);
        uint32_t ref_voltage = esp_adc_cal_raw_to_voltage(vref_raw, &s_adc1_chars);

        double probe1_temp_f = probe_temp_f(probe1_voltage, ref_voltage);
        double probe2_temp_f = probe_temp_f(probe2_voltage, ref_voltage);

        ESP_LOGI(TAG, "v_p1: %d, v_p2: %d, v_ref: %d", probe1_voltage, probe2_voltage, ref_voltage);
        ESP_LOGI(TAG, "temp1_f: %f, temp2_f: %f", probe1_temp_f, probe2_temp_f);

        vTaskDelay(pdMS_TO_TICKS(100));
        tick += 1;
    }
}