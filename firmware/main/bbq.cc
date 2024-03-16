/* OTA example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string>

#include "bbqctl.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mcp3002.h"
#include "mqtt_client.h"
#include "protocol_examples_common.h"
#include "string.h"

#ifdef CONFIG_EXAMPLE_USE_CERT_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "nvs.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#if CONFIG_EXAMPLE_CONNECT_WIFI
#include "esp_wifi.h"
#endif

#define HASH_LEN 32

#ifdef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF
/* The interface name value can refer to if_desc in esp_netif_defaults.h */
#if CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF_ETH
static const char *bind_interface_name = EXAMPLE_NETIF_DESC_ETH;
#elif CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF_STA
static const char *bind_interface_name = EXAMPLE_NETIF_DESC_STA;
#endif
#endif

static const char *TAG = "bbq";
extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

const bbqctl::Config config = {
    .steinhart_coeff0 = 0.0007343140544,
    .steinhart_coeff1 = 0.0002157437229,
    .steinhart_coeff2 = 0.0000000951568577,

    .probe_resistor_ohms = 10000,
};

static bbqctl::Controller controller(config);

static MCP_t mcp;

#define OTA_URL_SIZE 256

esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
  switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
      ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
      break;
    case HTTP_EVENT_ON_CONNECTED:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
      break;
    case HTTP_EVENT_HEADER_SENT:
      ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
      break;
    case HTTP_EVENT_ON_HEADER:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key,
               evt->header_value);
      break;
    case HTTP_EVENT_ON_DATA:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
      break;
    case HTTP_EVENT_ON_FINISH:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
      break;
    case HTTP_EVENT_DISCONNECTED:
      ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
      break;
    case HTTP_EVENT_REDIRECT:
      ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
      break;
  }
  return ESP_OK;
}

void handle_command(char *data, size_t data_len) {
  if (strncmp(data, "ota", data_len) == 0) {
    ESP_LOGI(TAG, "Triggering OTA");
    // trigger_ota();
  } else {
    ESP_LOGI(TAG, "unknown command: %s", data);
  }
}

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id,
                               esp_mqtt_event_handle_t event_data) {
  ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%ld", base,
           event_id);
  esp_mqtt_event_handle_t event = event_data;
  esp_mqtt_client_handle_t client = event->client;
  int msg_id;
  switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
      ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
      msg_id = esp_mqtt_client_subscribe(client, "/birbcam/command", 0);
      ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

      msg_id = esp_mqtt_client_subscribe(client, "/topic/qos1", 1);
      ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

      msg_id = esp_mqtt_client_unsubscribe(client, "/topic/qos1");
      ESP_LOGI(TAG, "sent unsubscribe successful, msg_id=%d", msg_id);
      break;
    case MQTT_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
      break;

    case MQTT_EVENT_SUBSCRIBED:
      ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
      // msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0,
      // 0); ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
      break;
    case MQTT_EVENT_UNSUBSCRIBED:
      ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
      break;
    case MQTT_EVENT_PUBLISHED:
      ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
      break;
    case MQTT_EVENT_DATA:
      ESP_LOGI(TAG, "MQTT_EVENT_DATA");
      printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
      printf("DATA=%.*s\r\n", event->data_len, event->data);
      // if (strncmp(event->data, "send binary please", event->data_len) == 0)
      // {
      //     ESP_LOGI(TAG, "Sending the binary");
      //     send_binary(client);
      // }
      if (strncmp(event->topic, "/bbq/command", event->topic_len) == 0) {
        handle_command(event->data, event->data_len);
      }
      break;
    case MQTT_EVENT_ERROR:
      ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
      if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
        ESP_LOGI(TAG, "Last error code reported from esp-tls: 0x%x",
                 event->error_handle->esp_tls_last_esp_err);
        ESP_LOGI(TAG, "Last tls stack error number: 0x%x",
                 event->error_handle->esp_tls_stack_err);
        ESP_LOGI(TAG, "Last captured errno : %d (%s)",
                 event->error_handle->esp_transport_sock_errno,
                 strerror(event->error_handle->esp_transport_sock_errno));
      } else if (event->error_handle->error_type ==
                 MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        ESP_LOGI(TAG, "Connection refused error: 0x%x",
                 event->error_handle->connect_return_code);
      } else {
        ESP_LOGW(TAG, "Unknown error type: 0x%x",
                 event->error_handle->error_type);
      }
      break;
    default:
      ESP_LOGI(TAG, "Other event id:%d", event->event_id);
      break;
  }
}

void bbq_task(void *pvParameter) {
  esp_mqtt_client_handle_t client = (esp_mqtt_client_handle_t)pvParameter;

  int32_t iter = 0;
  while (true) {
    bbqctl::voltage_t ambient = mcpReadData(&mcp, 5);
    bbqctl::voltage_t food = mcpReadData(&mcp, 6);
    bbqctl::voltage_t ref = mcpReadData(&mcp, 7);

    controller.ProvideReadings(ref, ambient, food);

    bbqctl::Output output = controller.GetOutput();

    if (iter % 100 == 0) {
      ESP_LOGI(TAG, "ambient: %f, food: %f", output.ambient_temp_f,
               output.food_temp_f);
    }

    if (iter % 500 == 0) {
      ESP_LOGI(TAG, "Publishing");
      std::string food = std::to_string(output.food_temp_f);
      esp_mqtt_client_publish(client, "/bbq/food_temp_f", food.c_str(),
                              food.length(), 0, 0);

      std::string ambient = std::to_string(output.ambient_temp_f);
      esp_mqtt_client_publish(client, "/bbq/ambient_temp_f", ambient.c_str(),
                              ambient.length(), 0, 0);

      std::string duty = std::to_string(output.duty_pct);
      esp_mqtt_client_publish(client, "/bbq/duty_pct", duty.c_str(),
                              duty.length(), 0, 0);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
    iter++;
  }
}

static void mqtt_app_start(void) {
  const esp_mqtt_client_config_t mqtt_cfg = {
      .broker =
          {
              .address = {.uri = CONFIG_BROKER_URI},
              .verification = {.crt_bundle_attach = esp_crt_bundle_attach},
          },
      .credentials = {
          .username = CONFIG_BROKER_USERNAME,
          .authentication = {.password = CONFIG_BROKER_PASSWORD},
      }};

  ESP_LOGI(TAG, "[APP] Free memory: %ld bytes", esp_get_free_heap_size());
  esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
  /* The last argument may be used to pass data to the event handler, in this
   * example mqtt_event_handler */
  esp_mqtt_client_register_event(client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
                                 (esp_event_handler_t)mqtt_event_handler, NULL);

  esp_mqtt_client_start(client);

  xTaskCreate(&bbq_task, "bbq_task", 8192, client, 5, NULL);
}

void simple_ota_example_task(void *pvParameter) {
  ESP_LOGI(TAG, "Starting OTA example task");
#ifdef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF
  esp_netif_t *netif = get_example_netif_from_desc(bind_interface_name);
  if (netif == NULL) {
    ESP_LOGE(TAG, "Can't find netif from interface description");
    abort();
  }
  struct ifreq ifr;
  esp_netif_get_netif_impl_name(netif, ifr.ifr_name);
  ESP_LOGI(TAG, "Bind interface name is %s", ifr.ifr_name);
#endif
  esp_http_client_config_t config = {
      .url = CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL,
      .event_handler = _http_event_handler,
      // #ifdef CONFIG_EXAMPLE_USE_CERT_BUNDLE
      //         .crt_bundle_attach = esp_crt_bundle_attach,
      // #else
      //         .cert_pem = (char *)server_cert_pem_start,
      // #endif /* CONFIG_EXAMPLE_USE_CERT_BUNDLE */

      .keep_alive_enable = true
      // #ifdef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF
      //         .if_name = &ifr,
      // #endif
  };

#ifdef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL_FROM_STDIN
  char url_buf[OTA_URL_SIZE];
  if (strcmp(config.url, "FROM_STDIN") == 0) {
    example_configure_stdin_stdout();
    fgets(url_buf, OTA_URL_SIZE, stdin);
    int len = strlen(url_buf);
    url_buf[len - 1] = '\0';
    config.url = url_buf;
  } else {
    ESP_LOGE(TAG, "Configuration mismatch: wrong firmware upgrade image url");
    abort();
  }
#endif

#ifdef CONFIG_EXAMPLE_SKIP_COMMON_NAME_CHECK
  config.skip_cert_common_name_check = true;
#endif

  esp_https_ota_config_t ota_config = {
      .http_config = &config,
  };
  ESP_LOGI(TAG, "Attempting to download update from %s", config.url);
  esp_err_t ret = esp_https_ota(&ota_config);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "OTA Succeed, Rebooting...");
    esp_restart();
  } else {
    ESP_LOGE(TAG, "Firmware upgrade failed");
  }
  while (1) {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

static void print_sha256(const uint8_t *image_hash, const char *label) {
  char hash_print[HASH_LEN * 2 + 1];
  hash_print[HASH_LEN * 2] = 0;
  for (int i = 0; i < HASH_LEN; ++i) {
    sprintf(&hash_print[i * 2], "%02x", image_hash[i]);
  }
  ESP_LOGI(TAG, "%s %s", label, hash_print);
}

static void get_sha256_of_partitions(void) {
  uint8_t sha_256[HASH_LEN] = {0};
  esp_partition_t partition;

  // get sha256 digest for bootloader
  partition.address = ESP_BOOTLOADER_OFFSET;
  partition.size = ESP_PARTITION_TABLE_OFFSET;
  partition.type = ESP_PARTITION_TYPE_APP;
  esp_partition_get_sha256(&partition, sha_256);
  print_sha256(sha_256, "SHA-256 for bootloader: ");

  // get sha256 digest for running partition
  esp_partition_get_sha256(esp_ota_get_running_partition(), sha_256);
  print_sha256(sha_256, "SHA-256 for current firmware: ");
}

extern "C" void app_main(void) {
  ESP_LOGI(TAG, "Hello bbq!");

  mcpInit(&mcp, MCP_MODEL::MCP3008,
          19,  // GPIO_MISO
          23,  // GPIO_MOSI,
          18,  // GPIO_SCLK,
          5,   // GPIO_CS,
          MCP_INPUT::MCP_SINGLE);

  // Initialize NVS.
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // 1.OTA app partition table has a smaller NVS partition size than the
    // non-OTA partition table. This size mismatch may cause NVS initialization
    // to fail. 2.NVS partition contains data in new format and cannot be
    // recognized by this version of code. If this happens, we erase NVS
    // partition and initialize NVS again.
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
  get_sha256_of_partitions();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  /* This helper function configures Wi-Fi or Ethernet, as selected in
   * menuconfig. Read "Establishing Wi-Fi or Ethernet Connection" section in
   * examples/protocols/README.md for more information about this function.
   */
  ESP_ERROR_CHECK(example_connect());

#if CONFIG_EXAMPLE_CONNECT_WIFI
  /* Ensure to disable any WiFi power save mode, this allows best throughput
   * and hence timings for overall OTA operation.
   */
  esp_wifi_set_ps(WIFI_PS_NONE);
#endif  // CONFIG_EXAMPLE_CONNECT_WIFI

  mqtt_app_start();
  //   xTaskCreate(&simple_ota_example_task, "ota_example_task", 8192, NULL, 5,
  //               NULL);
}
