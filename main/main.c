// ESP32 BBQ thermometer.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <esp_adc_cal.h>
#include <esp_eth.h>
#include <esp_event.h>
#include "lwip/apps/sntp.h"
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_tls_crypto.h>
#include <esp_websocket_client.h>
#include <esp_wifi.h>
#include <soc/adc_channel.h>
#include <sys/param.h>
#include <esp_random.h>
#include <iotc.h>
#include <iotc_jwt.h>

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

static Settings settings;
// static  State s_state;
static RawState s_raw_state;
static uint32_t s_session;

static esp_adc_cal_characteristics_t s_adc1_chars;

extern const uint8_t ec_pv_key_start[] asm("_binary_private_key_pem_start");
extern const uint8_t ec_pv_key_end[] asm("_binary_private_key_pem_end");

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

#define DEVICE_PATH "projects/%s/locations/%s/registries/%s/devices/%s"
#define SUBSCRIBE_TOPIC_COMMAND "/devices/%s/commands/#"
#define SUBSCRIBE_TOPIC_CONFIG "/devices/%s/config"
#define PUBLISH_TOPIC_EVENT "/devices/%s/events"
#define PUBLISH_TOPIC_STATE "/devices/%s/state"

char *subscribe_topic_command, *subscribe_topic_config;

iotc_mqtt_qos_t iotc_example_qos = IOTC_MQTT_QOS_AT_LEAST_ONCE;
static iotc_timed_task_handle_t delayed_publish_task =
    IOTC_INVALID_TIMED_TASK_HANDLE;
iotc_context_handle_t iotc_context = IOTC_INVALID_CONTEXT_HANDLE;

#define APPEND_JSON(sb, field, fmt, ...) \
    sb_format(&sb, "\"" field "\": " fmt, __VA_ARGS__)

#define APPEND_JSON_FIELD(sb, struct, field, fmt) \
    sb_format(&sb, "\"" #field "\": " fmt, struct->field)

static double STEINHART_COEFFS[] = {7.3431401e-4, 2.1574370e-4, 9.5156860e-8};

static uint8_t fan_duty_pct(double ambient_temp_f, struct Settings *settings)
{
    if (settings->is_manual)
    {
        return settings->manual_duty_pct;
    }

    if (ambient_temp_f < settings->threshold_f)
    {
        return settings->automatic_duty_pct;
    }

    return 0;
}

void publish_telemetry_event(iotc_context_handle_t context_handle, State state)
{
    // IOTC_UNUSED(timed_task);
    // IOTC_UNUSED(user_data);

    char *publish_topic = NULL;
    asprintf(&publish_topic, PUBLISH_TOPIC_EVENT, CONFIG_GIOT_DEVICE_ID);

    /* Send response with custom headers and body set as the
     * string passed in user context*/
    char *publish_message = NULL;

    state_json(s_session, &state, &publish_message);

    // char *publish_message = NULL;
    // asprintf(&publish_message, "message");
    ESP_LOGI(TAG, "publishing msg \"%s\" to topic: \"%s\"", publish_message, publish_topic);

    iotc_publish(context_handle, publish_topic, publish_message,
                 iotc_example_qos,
                 /*callback=*/NULL, /*user_data=*/NULL);
    free(publish_topic);
    free(publish_message);
}

void iotc_mqttlogic_subscribe_callback(
    iotc_context_handle_t in_context_handle, iotc_sub_call_type_t call_type,
    const iotc_sub_call_params_t *const params, iotc_state_t state,
    void *user_data)
{
    // IOTC_UNUSED(in_context_handle);
    // IOTC_UNUSED(call_type);
    // IOTC_UNUSED(state);
    // IOTC_UNUSED(user_data);
    if (params != NULL && params->message.topic != NULL)
    {
        ESP_LOGI(TAG, "Subscription Topic: %s", params->message.topic);
        char *sub_message = (char *)malloc(params->message.temporary_payload_data_length + 1);
        if (sub_message == NULL)
        {
            ESP_LOGE(TAG, "Failed to allocate memory");
            return;
        }
        memcpy(sub_message, params->message.temporary_payload_data, params->message.temporary_payload_data_length);
        sub_message[params->message.temporary_payload_data_length] = '\0';
        ESP_LOGI(TAG, "Message Payload: %s ", sub_message);
        if (strcmp(subscribe_topic_command, params->message.topic) == 0)
        {
            int value;
            sscanf(sub_message, "{\"outlet\": %d}", &value);
            ESP_LOGI(TAG, "value: %d", value);
            // if (value == 1) {
            //     gpio_set_level(OUTPUT_GPIO, true);
            // } else if (value == 0) {
            //     gpio_set_level(OUTPUT_GPIO, false);
            // }
        }
        free(sub_message);
    }
}

void on_connection_state_changed(iotc_context_handle_t in_context_handle,
                                 void *data, iotc_state_t state)
{
    iotc_connection_data_t *conn_data = (iotc_connection_data_t *)data;

    switch (conn_data->connection_state)
    {
    /* IOTC_CONNECTION_STATE_OPENED means that the connection has been
       established and the IoTC Client is ready to send/recv messages */
    case IOTC_CONNECTION_STATE_OPENED:
        ESP_LOGI(TAG, "connected!");

        /* Publish immediately upon connect. 'publish_function' is defined
           in this example file and invokes the IoTC API to publish a
           message. */
        asprintf(&subscribe_topic_command, SUBSCRIBE_TOPIC_COMMAND, CONFIG_GIOT_DEVICE_ID);
        ESP_LOGI(TAG, "subscribing to topic: \"%s\"", subscribe_topic_command);
        iotc_subscribe(in_context_handle, subscribe_topic_command, IOTC_MQTT_QOS_AT_LEAST_ONCE,
                       &iotc_mqttlogic_subscribe_callback, /*user_data=*/NULL);

        asprintf(&subscribe_topic_config, SUBSCRIBE_TOPIC_CONFIG, CONFIG_GIOT_DEVICE_ID);
        ESP_LOGI(TAG, "subscribing to topic: \"%s\"", subscribe_topic_config);
        iotc_subscribe(in_context_handle, subscribe_topic_config, IOTC_MQTT_QOS_AT_LEAST_ONCE,
                       &iotc_mqttlogic_subscribe_callback, /*user_data=*/NULL);

        // /* Create a timed task to publish every 10 seconds. */
        // delayed_publish_task = iotc_schedule_timed_task(in_context_handle,
        //                        publish_telemetry_event, 10,
        //                        15, /*user_data=*/NULL);
        break;

    /* IOTC_CONNECTION_STATE_OPEN_FAILED is set when there was a problem
       when establishing a connection to the server. The reason for the error
       is contained in the 'state' variable. Here we log the error state and
       exit out of the application. */

    /* Publish immediately upon connect. 'publish_function' is defined
       in this example file and invokes the IoTC API to publish a
       message. */
    case IOTC_CONNECTION_STATE_OPEN_FAILED:
        ESP_LOGI(TAG, "ERROR! Connection has failed reason %d", state);

        /* exit it out of the application by stopping the event loop. */
        iotc_events_stop();
        break;

    /* IOTC_CONNECTION_STATE_CLOSED is set when the IoTC Client has been
       disconnected. The disconnection may have been caused by some external
       issue, or user may have requested a disconnection. In order to
       distinguish between those two situation it is advised to check the state
       variable value. If the state == IOTC_STATE_OK then the application has
       requested a disconnection via 'iotc_shutdown_connection'. If the state !=
       IOTC_STATE_OK then the connection has been closed from one side. */
    case IOTC_CONNECTION_STATE_CLOSED:
        free(subscribe_topic_command);
        free(subscribe_topic_config);
        /* When the connection is closed it's better to cancel some of previously
           registered activities. Using cancel function on handler will remove the
           handler from the timed queue which prevents the registered handle to be
           called when there is no connection. */
        if (IOTC_INVALID_TIMED_TASK_HANDLE != delayed_publish_task)
        {
            iotc_cancel_timed_task(delayed_publish_task);
            delayed_publish_task = IOTC_INVALID_TIMED_TASK_HANDLE;
        }

        if (state == IOTC_STATE_OK)
        {
            /* The connection has been closed intentionally. Therefore, stop
               the event processing loop as there's nothing left to do
               in this example. */
            iotc_events_stop();
        }
        else
        {
            ESP_LOGE(TAG, "connection closed - reason %d!", state);
            /* The disconnection was unforeseen.  Try reconnect to the server
            with previously set configuration, which has been provided
            to this callback in the conn_data structure. */
            iotc_connect(
                in_context_handle, conn_data->username, conn_data->password, conn_data->client_id,
                conn_data->connection_timeout, conn_data->keepalive_timeout,
                &on_connection_state_changed);
        }
        break;

    default:
        ESP_LOGE(TAG, "incorrect connection state value.");
        break;
    }
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

static double probe_temp_f(double probe_mv, double ref_mv)
{
    // V = Vref * 2

    // V = Vprobe + Vresistor
    // Vresistor = V - VProbe
    // Vprobe / Rprobe = Vresistor / Rresistor
    // Rprobe = Vprobe * Rresistor / Vresistor
    // Rprobe = Vprobe * PROBE_RESISTOR_OHMS / (Vref*2 - Vprobe)
    double r_probe_ohm = probe_mv * PROBE_RESISTOR_OHMS / (ref_mv * 2 - probe_mv);

    // Fix up some wacky edge cases that can cause NaNs and Infs.
    if (r_probe_ohm < 1) {
        r_probe_ohm = 10000000;
    }
    r_probe_ohm = MIN(r_probe_ohm, 10000000);
    r_probe_ohm = MAX(r_probe_ohm, 10);
    
    double log_r = log(r_probe_ohm);
    double temp_k = 1.0 / (STEINHART_COEFFS[0] + STEINHART_COEFFS[1] * log_r + STEINHART_COEFFS[2] * log_r * log_r * log_r);
    return (temp_k - 273.15) * 9 / 5 + 32;
}

static State derive_state(RawState *raw, Settings *settings)
{
    double avg_ref_voltage_mv = 0;
    double avg_probe_voltage_mv[2] = {0, 0};

    for (int i = 0; i < CONFIG_TEMP_BUFFER_LEN; i++)
    {
        avg_ref_voltage_mv += raw->reference_voltage_mv[i];
        avg_probe_voltage_mv[0] += raw->probe_voltage_mv[0][i];
        avg_probe_voltage_mv[1] += raw->probe_voltage_mv[1][i];
    }

    avg_ref_voltage_mv /= CONFIG_TEMP_BUFFER_LEN;
    avg_probe_voltage_mv[0] /= CONFIG_TEMP_BUFFER_LEN;
    avg_probe_voltage_mv[1] /= CONFIG_TEMP_BUFFER_LEN;

    // ESP_LOGI(TAG, "avg_ref_mv: %f", avg_ref_voltage_mv);
    // ESP_LOGI(TAG, "p0_mv: %f", avg_probe_voltage_mv[0]);
    // ESP_LOGI(TAG, "p1_mv: %f", avg_probe_voltage_mv[1]);

    State res = {
        .probe_temps_f = {
            probe_temp_f(avg_probe_voltage_mv[0], avg_ref_voltage_mv),
            probe_temp_f(avg_probe_voltage_mv[1], avg_ref_voltage_mv),
        }};
    res.duty_pct = fan_duty_pct(res.probe_temps_f[0], settings);

    return res;
}

static void raw_json(char *buf, size_t buflen, RawState *raw)
{
    StringBuffer sb = sb_create(buf, buflen);

    sb_format(&sb, "{");
    sb_format(&sb, "\"reference_voltage_mv\": [");
    for (int i = 0; i < CONFIG_TEMP_BUFFER_LEN; i++)
    {
        sb_format(&sb, "%d, ", raw->reference_voltage_mv[i]);
    }
    sb_format(&sb, "], \"probe_voltage_mv\": [[");
    for (int i = 0; i < CONFIG_TEMP_BUFFER_LEN; i++)
    {
        sb_format(&sb, "%d, ", raw->probe_voltage_mv[0][i]);
    }
    sb_format(&sb, "], [");
    for (int i = 0; i < CONFIG_TEMP_BUFFER_LEN; i++)
    {
        sb_format(&sb, "%d, ", raw->probe_voltage_mv[1][i]);
    }
    sb_format(&sb, "]}");
    // APPEND_JSON(sb, "probe_temps_f", "[%f, %f],", state->probe_temps_f[0],
    // state->probe_temps_f[1]    );
    // APPEND_JSON(sb, "uptime_usec", "%lld", );
    // sb_format(&sb, "}");
    
}

static void settings_json(char *buf, size_t buflen, Settings *settings)
{
    StringBuffer sb = sb_create(buf, buflen);

    sb_format(&sb, "{");
    APPEND_JSON_FIELD(sb, settings, is_manual, "%d,");
    APPEND_JSON_FIELD(sb, settings, manual_duty_pct, "%d,");
    APPEND_JSON_FIELD(sb, settings, threshold_f, "%d,");
    APPEND_JSON_FIELD(sb, settings, automatic_duty_pct, "%d");
    sb_format(&sb, "}");
}

/* An HTTP GET handler */
static esp_err_t index_get_handler(httpd_req_t *req)
{
    /* Send response with custom headers and body set as the
     * string passed in user context*/
    char* resp =  NULL;

    State s = derive_state(&s_raw_state, &settings);

    state_json(s_session, &s, &resp);

    httpd_resp_set_hdr(req, "content-type", "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    free(resp);
    return ESP_OK;
}

static const httpd_uri_t index_get = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_get_handler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx = "Hello World!"};

/* An HTTP GET handler */
static esp_err_t raw_get_handler(httpd_req_t *req)
{
    /* Send response with custom headers and body set as the
     * string passed in user context*/
    char resp[RESPONSE_BUFFER_SIZE];

    raw_json(resp, sizeof(resp), &s_raw_state);

    httpd_resp_set_hdr(req, "content-type", "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t raw_get = {
    .uri = "/raw",
    .method = HTTP_GET,
    .handler = raw_get_handler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx = "Hello World!"};

/* An HTTP GET handler */
static esp_err_t settings_get_handler(httpd_req_t *req)
{
    char resp[RESPONSE_BUFFER_SIZE];
    settings_json(resp, sizeof(resp), &settings);
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
                settings.is_manual = atoi(param);
            }
            if (httpd_query_key_value(buf, "manual_duty_pct", param, sizeof(param)) == ESP_OK)
            {
                settings.manual_duty_pct = atoi(param);
            }
            if (httpd_query_key_value(buf, "threshold_f", param, sizeof(param)) == ESP_OK)
            {
                settings.threshold_f = atoi(param);
            }
            if (httpd_query_key_value(buf, "automatic_duty", param, sizeof(param)) == ESP_OK)
            {
                settings.automatic_duty_pct = atoi(param);
            }
        }
        free(buf);
    }

    char resp[RESPONSE_BUFFER_SIZE];
    settings_json(resp, sizeof(resp), &settings);
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
    config.stack_size = 1 << 13; // 8k

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK)
    {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &index_get);
        httpd_register_uri_handler(server, &raw_get);
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

static void mqtt_task(void *pvParameters)
{
    /* Format the key type descriptors so the client understands
     which type of key is being represented. In this case, a PEM encoded
     byte array of a ES256 key. */
    iotc_crypto_key_data_t iotc_connect_private_key_data;
    iotc_connect_private_key_data.crypto_key_signature_algorithm = IOTC_CRYPTO_KEY_SIGNATURE_ALGORITHM_ES256;
    iotc_connect_private_key_data.crypto_key_union_type = IOTC_CRYPTO_KEY_UNION_TYPE_PEM;
    iotc_connect_private_key_data.crypto_key_union.key_pem.key = (char *)ec_pv_key_start;

    /* initialize iotc library and create a context to use to connect to the
     * GCP IoT Core Service. */
    const iotc_state_t error_init = iotc_initialize();

    if (IOTC_STATE_OK != error_init)
    {
        ESP_LOGE(TAG, " iotc failed to initialize, error: %d", error_init);
        vTaskDelete(NULL);
    }

    /*  Create a connection context. A context represents a Connection
        on a single socket, and can be used to publish and subscribe
        to numerous topics. */
    iotc_context = iotc_create_context();
    if (IOTC_INVALID_CONTEXT_HANDLE >= iotc_context)
    {
        ESP_LOGE(TAG, " iotc failed to create context, error: %d", -iotc_context);
        vTaskDelete(NULL);
    }

    /*  Queue a connection request to be completed asynchronously.
        The 'on_connection_state_changed' parameter is the name of the
        callback function after the connection request completes, and its
        implementation should handle both successful connections and
        unsuccessful connections as well as disconnections. */
    const uint16_t connection_timeout = 0;
    const uint16_t keepalive_timeout = 20;

    /* Generate the client authentication JWT, which will serve as the MQTT
     * password. */
    char jwt[IOTC_JWT_SIZE] = {0};
    size_t bytes_written = 0;
    iotc_state_t state = iotc_create_iotcore_jwt(
        CONFIG_GIOT_PROJECT_ID,
        /*jwt_expiration_period_sec=*/3600, &iotc_connect_private_key_data, jwt,
        IOTC_JWT_SIZE, &bytes_written);

    if (IOTC_STATE_OK != state)
    {
        ESP_LOGE(TAG, "iotc_create_iotcore_jwt returned with error: %ul", state);
        vTaskDelete(NULL);
    }

    char *device_path = NULL;
    asprintf(&device_path, DEVICE_PATH, CONFIG_GIOT_PROJECT_ID, CONFIG_GIOT_LOCATION, CONFIG_GIOT_REGISTRY_ID, CONFIG_GIOT_DEVICE_ID);
    iotc_connect(iotc_context, NULL, jwt, device_path, connection_timeout,
                 keepalive_timeout, &on_connection_state_changed);
    free(device_path);
    /* The IoTC Client was designed to be able to run on single threaded devices.
        As such it does not have its own event loop thread. Instead you must
        regularly call the function iotc_events_process_blocking() to process
        connection requests, and for the client to regularly check the sockets for
        incoming data. This implementation has the loop operate endlessly. The loop
        will stop after closing the connection, using iotc_shutdown_connection() as
        defined in on_connection_state_change logic, and exit the event handler
        handler by calling iotc_events_stop(); */
    iotc_events_process_blocking();

    iotc_delete_context(iotc_context);

    iotc_shutdown();

    vTaskDelete(NULL);
}

static void init(void)
{
    settings = settings_create();

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

void post_state(State *state)
{

    // esp_http_client_config_t config = {
    //     .url = CONFIG_DATA_ENDPOINT,
    //     .method = HTTP_METHOD_POST,
    // };

    /* Send response with custom headers and body set as the
     * string passed in user context*/
    char *resp = NULL;

    state_json(s_session, state, &resp);

    // esp_http_client_handle_t client = esp_http_client_init(&config);
    // esp_http_client_set_post_field(client, resp, strlen(resp));
    // esp_http_client_set_header(client, "content-type", "application/json");
    // esp_err_t err = esp_http_client_perform(client);

    // if (err == ESP_OK)
    // {
    //     ESP_LOGI(TAG, "HTTPS Status = %d, content_length = %d",
    //              esp_http_client_get_status_code(client),
    //              esp_http_client_get_content_length(client));
    // }
    // else
    // {
    //     ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    // }
    // esp_http_client_cleanup(client);
}

Readings do_readings()
{
    int probe1_raw = adc1_get_raw(ADC1_PROBE1_CHAN);
    int probe2_raw = adc1_get_raw(ADC1_PROBE2_CHAN);
    int vref_raw = adc1_get_raw(ADC1_VREF_CHAN);

    Readings res = {
        .reference_voltage_mv = esp_adc_cal_raw_to_voltage(vref_raw, &s_adc1_chars),
        .probe_voltage_mv = {
            esp_adc_cal_raw_to_voltage(probe1_raw, &s_adc1_chars),
            esp_adc_cal_raw_to_voltage(probe2_raw, &s_adc1_chars)}};

    return res;
}

static void update_state(uint64_t tick, RawState *state, Readings readings)
{
    state->reference_voltage_mv[tick % CONFIG_TEMP_BUFFER_LEN] = readings.reference_voltage_mv;
    state->probe_voltage_mv[0][tick % CONFIG_TEMP_BUFFER_LEN] = readings.probe_voltage_mv[0];
    state->probe_voltage_mv[1][tick % CONFIG_TEMP_BUFFER_LEN] = readings.probe_voltage_mv[1];
}

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "time.google.com");
    sntp_init();
}

static void obtain_time(void)
{
    initialize_sntp();
    // wait for time to be set
    time_t now = 0;
    struct tm timeinfo = {0};
    while (timeinfo.tm_year < (2016 - 1900))
    {
        ESP_LOGI(TAG, "Waiting for system time to be set...");
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }
    ESP_LOGI(TAG, "Time is set...");
}

void app_main(void)
{
    init();
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    obtain_time();
    xTaskCreate(&mqtt_task, "mqtt_task", 8192, NULL, 5, NULL);

    s_session = esp_random();
    uint64_t tick = 0;
    while (1)
    {
        update_state(tick, &s_raw_state, do_readings());

        State state = derive_state(&s_raw_state, &settings);

        if (tick != 0 && tick % CONFIG_REPORT_PERIOD == 0)
        {
            publish_telemetry_event(iotc_context, state);
            //            post_state(&state);
        }

        /* Set the GPIO level according to the state (LOW or HIGH)*/
        bool set_fan = tick % CONFIG_DUTY_PERIOD < state.duty_pct * CONFIG_DUTY_PERIOD / 100;
        gpio_set_level(FAN_CONTROL_GPIO, set_fan);

        vTaskDelay(pdMS_TO_TICKS(CONFIG_TICK_PERIOD));
        tick += 1;
    }
}
