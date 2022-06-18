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

static bool s_fan_manual = false;
static double s_fan_manual_duty = 0;
static double s_fan_automatic_duty = 0.7;
static double s_fan_threshold_f = 215.0;

static double s_probe1_temp_f = 0;
static double s_probe2_temp_f = 0;
static double s_current_duty = 0;

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

// static double fan_duty() {
//     if (s_fan_manual) {
//         return s_fan_manual_duty;
//     }

//     double ret = s_fan_ki * s_error_integral + s_fan_kp * (s_set_point_f - s_probe1_temp_f);
//     if (ret < 0.1) {
//         return 0;
//     }
//         return ret;
// }

static double fan_duty()
{
    if (s_fan_manual)
    {
        return s_fan_manual_duty;
    }

    if (s_probe1_temp_f < s_fan_threshold_f)
    {
        return s_fan_automatic_duty;
    }

    return 0;
}

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

struct StringBuffer
{
    char *buffer;
    size_t buflen;
    size_t written;
};
// void myfun(const char *fmt, va_list argp) {
//     vfprintf(stderr, fmt, argp);
// }

static bool sb_format(struct StringBuffer *buffer, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    if (buffer->written < buffer->buflen)
    {
        buffer->written += vsnprintf(buffer->buffer + buffer->written, buffer->buflen - buffer->written, fmt, args);
    }
    va_end(args);
    return buffer->written < buffer->buflen;
}

static void get_stats(char *buf, size_t buflen)
{
    struct StringBuffer sb = {.buffer = buf,
                              .buflen = buflen,
                              .written = 0};

#define APPEND_JSON(field, fmt, value) \
    sb_format(&sb, "\"" field "\": " fmt ",", value)

#define APPEND_GLOBAL_JSON(field, fmt) \
    sb_format(&sb, "\"" #field "\": " fmt ",", s_##field)

    int64_t uptime_usec = esp_timer_get_time();

    sb_format(&sb, "{");
    APPEND_GLOBAL_JSON(probe1_temp_f, "%f");
    APPEND_GLOBAL_JSON(probe2_temp_f, "%f");
    APPEND_GLOBAL_JSON(current_duty, "%f");
    APPEND_JSON("uptime_usec", "%lld", uptime_usec);
    sb_format(&sb, "}");
}

/* An HTTP GET handler */
static esp_err_t index_get_handler(httpd_req_t *req)
{
    /* Send response with custom headers and body set as the
     * string passed in user context*/
    char resp[2048];

    get_stats(&resp, sizeof(resp));

    httpd_resp_set_hdr(req, "content-type", "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static size_t settings_json(char *buf, size_t buf_len)
{
    return snprintf(buf, buf_len, "{"
                                  "\"manual\": %d,"
                                  "\"manual_duty\": %f,"
                                  "\"threshold_f\": %f,"
                                  "\"automatic_duty\": %f"
                                  "}\n",
                    s_fan_manual,
                    s_fan_manual_duty,
                    s_fan_threshold_f,
                    s_fan_automatic_duty);
}

static const httpd_uri_t index_get = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_get_handler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx = "Hello World!"};

/* An HTTP GET handler */
static esp_err_t settings_get_handler(httpd_req_t *req)
{
    char resp[2048];
    settings_json(resp, sizeof(resp));
    httpd_resp_set_hdr(req, "content-type", "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t settings_get = {
    .uri = "/settings",
    .method = HTTP_GET,
    .handler = settings_get_handler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx = "Hello World!"};

/* An HTTP POST handler */
static esp_err_t settings_post_handler(httpd_req_t *req)
{
    /* Read URL query string length and allocate memory for length + 1,
     * extra byte for null termination */
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1)
    {
        char *buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK)
        {
            char param[32];
            if (httpd_query_key_value(buf, "manual", param, sizeof(param)) == ESP_OK)
            {
                s_fan_manual = atoi(param);
            }
            if (httpd_query_key_value(buf, "manual_duty", param, sizeof(param)) == ESP_OK)
            {
                s_fan_manual_duty = atof(param);
            }
            if (httpd_query_key_value(buf, "threshold_f", param, sizeof(param)) == ESP_OK)
            {
                s_fan_threshold_f = atof(param);
            }
            if (httpd_query_key_value(buf, "automatic_duty", param, sizeof(param)) == ESP_OK)
            {
                s_fan_automatic_duty = atof(param);
            }
        }
        free(buf);
    }

    char resp[2048];
    settings_json(resp, sizeof(resp));
    httpd_resp_set_hdr(req, "content-type", "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t settings_post = {
    .uri = "/settings",
    .method = HTTP_POST,
    .handler = settings_post_handler,
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
        httpd_register_uri_handler(server, &index_get);
        httpd_register_uri_handler(server, &settings_get);
        httpd_register_uri_handler(server, &settings_post);
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
        if (tick % CONFIG_DUTY_PERIOD == 0)
        {
            s_current_duty = fan_duty();
            ESP_LOGI(TAG, "setting duty = %f", s_current_duty);
            // ESP_LOGI(TAG, "v_p1: %d, v_p2: %d, v_ref: %d", probe1_voltage, probe2_voltage, ref_voltage);
            ESP_LOGI(TAG, "temp1_f: %f, temp2_f: %f", s_probe1_temp_f, s_probe2_temp_f);
        }
        /* Set the GPIO level according to the state (LOW or HIGH)*/
        bool set_fan = tick % CONFIG_DUTY_PERIOD < s_current_duty * CONFIG_DUTY_PERIOD;
        gpio_set_level(FAN_CONTROL_GPIO, set_fan);

        int probe1_raw = adc1_get_raw(ADC1_PROBE1_CHAN);
        int probe2_raw = adc1_get_raw(ADC1_PROBE2_CHAN);
        int vref_raw = adc1_get_raw(ADC1_VREF_CHAN);

        uint32_t probe1_voltage = esp_adc_cal_raw_to_voltage(probe1_raw, &s_adc1_chars);
        uint32_t probe2_voltage = esp_adc_cal_raw_to_voltage(probe2_raw, &s_adc1_chars);
        uint32_t ref_voltage = esp_adc_cal_raw_to_voltage(vref_raw, &s_adc1_chars);

        double probe1_temp_f = probe_temp_f(probe1_voltage, ref_voltage);
        double probe2_temp_f = probe_temp_f(probe2_voltage, ref_voltage);

        // Exponential moving average.
        s_probe1_temp_f = (s_probe1_temp_f + probe1_temp_f) / 2;
        s_probe2_temp_f = (s_probe2_temp_f + probe2_temp_f) / 2;

        vTaskDelay(pdMS_TO_TICKS(CONFIG_TICK_PERIOD));
        tick += 1;
    }
}