// ESP32 BBQ thermometer.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <esp_adc_cal.h>
#include <esp_eth.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_tls_crypto.h>
#include <esp_websocket_client.h>
#include <esp_wifi.h>
#include <soc/adc_channel.h>
#include <sys/param.h>

#include "driver/adc.h"
#include "freertos/event_groups.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"

#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"

#include "state.h"
#include "stringbuffer.h"

#define RESPONSE_BUFFER_SIZE 1024

static const char *TAG = "BBQ";

static struct Settings s_settings;
static struct State s_state;

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

#define APPEND_JSON(sb, field, fmt, value) \
    sb_format(&sb, "\"" field "\": " fmt, value)

#define APPEND_JSON_FIELD(sb, struct, field, fmt) \
    sb_format(&sb, "\"" #field "\": " fmt, struct->field)

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

static uint8_t fan_duty()
{
    if (s_settings.is_manual)
    {
        return s_settings.manual_duty_pct;
    }

    if (s_state.probe1_f < s_settings.threshold_f)
    {
        return s_settings.automatic_duty_pct;
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

static void state_json(char *buf, size_t buflen, struct State *state)
{
    struct StringBuffer sb = sb_create(buf, buflen);

    sb_format(&sb, "{");
    APPEND_JSON_FIELD(sb, state, probe1_f, "%d,");
    APPEND_JSON_FIELD(sb, state, probe2_f, "%d,");
    APPEND_JSON_FIELD(sb, state, duty_pct, "%d,");
    APPEND_JSON(sb, "uptime_usec", "%lld", esp_timer_get_time());
    sb_format(&sb, "}");
}

/* An HTTP GET handler */
static esp_err_t index_get_handler(httpd_req_t *req)
{
    /* Send response with custom headers and body set as the
     * string passed in user context*/
    char resp[RESPONSE_BUFFER_SIZE];

    state_json(resp, sizeof(resp), &s_state);

    httpd_resp_set_hdr(req, "content-type", "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void settings_json(char *buf, size_t buflen, struct Settings *settings)
{
    struct StringBuffer sb = sb_create(buf, buflen);

    sb_format(&sb, "{");
    APPEND_JSON_FIELD(sb, settings, is_manual, "%d,");
    APPEND_JSON_FIELD(sb, settings, manual_duty_pct, "%d,");
    APPEND_JSON_FIELD(sb, settings, threshold_f, "%d,");
    APPEND_JSON_FIELD(sb, settings, automatic_duty_pct, "%d");
    sb_format(&sb, "}");
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
    char resp[RESPONSE_BUFFER_SIZE];
    settings_json(resp, sizeof(resp), &s_settings);
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
            if (httpd_query_key_value(buf, "is_manual", param, sizeof(param)) == ESP_OK)
            {
                s_settings.is_manual = atoi(param);
            }
            if (httpd_query_key_value(buf, "manual_duty_pct", param, sizeof(param)) == ESP_OK)
            {
                s_settings.manual_duty_pct = atoi(param);
            }
            if (httpd_query_key_value(buf, "threshold_f", param, sizeof(param)) == ESP_OK)
            {
                s_settings.threshold_f = atoi(param);
            }
            if (httpd_query_key_value(buf, "automatic_duty", param, sizeof(param)) == ESP_OK)
            {
                s_settings.automatic_duty_pct = atoi(param);
            }
        }
        free(buf);
    }

    char resp[RESPONSE_BUFFER_SIZE];
    settings_json(resp, sizeof(resp), &s_settings);
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
    // config.stack_size = 1<<13; // 8k

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
    s_settings = settings_create();

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

void post_state() {

    esp_http_client_config_t config = {
        .url = "https://bbqbe.hjfreyer.repl.co/device",
        .method = HTTP_METHOD_POST,
    };

    /* Send response with custom headers and body set as the
     * string passed in user context*/
    char resp[RESPONSE_BUFFER_SIZE];

    state_json(resp, sizeof(resp), &s_state);

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_post_field(client, resp, strlen(resp));
    esp_http_client_set_header(client, "content-type", "application/json");
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTPS Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

void app_main(void)
{
    init();
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    int tick = 0;
    while (1)
    {
        if (tick % CONFIG_DUTY_PERIOD == 0)
        {
            s_state.duty_pct = fan_duty();
            ESP_LOGI(TAG, "setting duty = %d%%", s_state.duty_pct);
            // ESP_LOGI(TAG, "v_p1: %d, v_p2: %d, v_ref: %d", probe1_voltage, probe2_voltage, ref_voltage);
            ESP_LOGI(TAG, "temp1_f: %d, temp2_f: %d", s_state.probe1_f, s_state.probe2_f);
        }

        if (tick % CONFIG_REPORT_PERIOD == 0) {
            post_state();
        }

        /* Set the GPIO level according to the state (LOW or HIGH)*/
        bool set_fan = tick % CONFIG_DUTY_PERIOD < s_state.duty_pct * CONFIG_DUTY_PERIOD / 100;
        gpio_set_level(FAN_CONTROL_GPIO, set_fan);

        int probe1_raw = adc1_get_raw(ADC1_PROBE1_CHAN);
        int probe2_raw = adc1_get_raw(ADC1_PROBE2_CHAN);
        int vref_raw = adc1_get_raw(ADC1_VREF_CHAN);

        uint32_t probe1_voltage = esp_adc_cal_raw_to_voltage(probe1_raw, &s_adc1_chars);
        uint32_t probe2_voltage = esp_adc_cal_raw_to_voltage(probe2_raw, &s_adc1_chars);
        uint32_t ref_voltage = esp_adc_cal_raw_to_voltage(vref_raw, &s_adc1_chars);

        double probe1_f = probe_temp_f(probe1_voltage, ref_voltage);
        double probe2_f = probe_temp_f(probe2_voltage, ref_voltage);

        // Exponential moving average.
        s_state.probe1_f = (s_state.probe1_f + probe1_f) / 2;
        s_state.probe2_f = (s_state.probe2_f + probe2_f) / 2;

        vTaskDelay(pdMS_TO_TICKS(CONFIG_TICK_PERIOD));
        tick += 1;
    }
}
