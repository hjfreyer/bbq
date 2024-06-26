/* OTA example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <driver/gpio.h>
#include <esp_event.h>
#include <esp_http_client.h>
#include <esp_http_server.h>
#include <esp_https_ota.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mcp3002.h>
#include <mdns.h>
#include <protocol_examples_common.h>
#include <string.h>

#include <sstream>
#include <string>
#include <vector>

#include "bbqctl.h"

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

static const char *TAG = "bbq";
extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

static uint32_t SESSION_ID;

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
  ESP_LOGI(TAG, "unknown command: %s", data);
}

std::string serialize_output(const bbqctl::Output &output) {
  std::stringstream out;
  out << "{";
  out << R"("session": )" << SESSION_ID << ", ";
  out << R"("food_temp_f": )" << output.food_temp_f << ", ";
  out << R"("ambient_temp_f": )" << output.ambient_temp_f << ", ";
  uint32_t duty_pct = output.duty_pct;
  out << R"("duty_pct": )" << duty_pct;
  out << "}";
  return out.str();
}

void post_output(const bbqctl::Output& output)
{
    esp_http_client_config_t config = {
        .url = "https://bbq.hjfreyer.com/api/readings",
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    std::string post_data = serialize_output(output);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_post_field(client, post_data.c_str(), post_data.length());

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTPS Status = %d, content_length = %"PRId64,
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}


void bbq_task(void *pvParameter) {
  int32_t tock = 0;
  while (true) {
    if (tock % CONFIG_MEASUREMENT_PERIOD == 0) {
      bbqctl::voltage_t ambient = mcpReadData(&mcp, 5);
      bbqctl::voltage_t food = mcpReadData(&mcp, 6);
      bbqctl::voltage_t ref = mcpReadData(&mcp, 7);

      controller.ProvideReadings(ref, ambient, food);
    }

    bbqctl::Output output = controller.GetOutput();

    if (tock % CONFIG_REPORTING_PERIOD == 0) {
      ESP_LOGI(TAG, "ambient: %f, food: %f, duty_pct: %d",
               output.ambient_temp_f, output.food_temp_f, output.duty_pct);
      post_output(output);
    }

    /* Set the GPIO level according to the state (LOW or HIGH)*/
    bool set_fan =
        tock % CONFIG_DUTY_PERIOD < output.duty_pct * CONFIG_DUTY_PERIOD / 100;
    gpio_set_level((gpio_num_t)CONFIG_FAN_CONTROL_GPIO, set_fan);

    vTaskDelay(CONFIG_TICKS_PER_TOCK);
    tock++;
  }
}

static std::string settings_json(const bbqctl::Settings &settings) {
  std::stringstream out;

  out << "{";
  out << R"("is_manual": )";
  if (settings.is_manual) {
    out << "true,";
  } else {
    out << "false,";
  }
  out << R"("threshold_f": )" << settings.threshold_f << ",";
  out << "}";

  return out.str();
}

/* An HTTP GET handler */
static esp_err_t root_get_handler(httpd_req_t *req) {
  ESP_LOGI(TAG, "GOT GET");
  const std::string body =
      R"(<script src="https://bbq.hjfreyer.com/remote.js"></script>)";
  httpd_resp_send(req, body.c_str(), body.length());
  return ESP_OK;
}

static const httpd_uri_t root_get = {.uri = "/",
                                         .method = HTTP_GET,
                                         .handler = root_get_handler,
                                         .user_ctx = nullptr};

/* An HTTP GET handler */
static esp_err_t settings_get_handler(httpd_req_t *req) {
  ESP_LOGI(TAG, "GOT GET");
  std::string settings = settings_json(controller.GetSettings());
  httpd_resp_set_hdr(req, "content-type", "application/json");
  httpd_resp_send(req, settings.c_str(), settings.length());
  return ESP_OK;
}

static const httpd_uri_t settings_get = {.uri = "/settings",
                                         .method = HTTP_GET,
                                         .handler = settings_get_handler,
                                         .user_ctx = nullptr};

/* An HTTP POST handler */
static esp_err_t settings_post_handler(httpd_req_t *req) {
  /* Read URL query string length and allocate memory for length + 1,
   * extra byte for null termination */

  size_t buf_len = httpd_req_get_url_query_len(req) + 1;
  std::vector<char> buf;
  buf.resize(buf_len);
  if (buf_len > 1) {
    if (httpd_req_get_url_query_str(req, buf.data(), buf_len) == ESP_OK) {
      char param[32];
      if (httpd_query_key_value(buf.data(), "is_manual", param,
                                sizeof(param)) == ESP_OK) {
        controller.GetSettings().is_manual = atoi(param);
      }
      if (httpd_query_key_value(buf.data(), "manual_duty_pct", param,
                                sizeof(param)) == ESP_OK) {
        controller.GetSettings().manual_duty_pct = atoi(param);
      }
      if (httpd_query_key_value(buf.data(), "threshold_f", param,
                                sizeof(param)) == ESP_OK) {
        controller.GetSettings().threshold_f = atoi(param);
      }
      if (httpd_query_key_value(buf.data(), "automatic_duty", param,
                                sizeof(param)) == ESP_OK) {
        controller.GetSettings().automatic_duty_pct = atoi(param);
      }
    }
  }

  std::string settings = settings_json(controller.GetSettings());
  httpd_resp_set_hdr(req, "content-type", "application/json");
  httpd_resp_send(req, settings.c_str(), settings.length());
  ESP_LOGI(TAG, "got request!");
  return ESP_OK;
}

static const httpd_uri_t settings_post = {.uri = "/settings",
                                          .method = HTTP_POST,
                                          .handler = settings_post_handler,
                                          .user_ctx = NULL};

static void start_mdns_service() {
  // initialize mDNS service
  esp_err_t err = mdns_init();
  if (err) {
    ESP_LOGE(TAG, "MDNS Init failed: %d\n", err);
    return;
  }

  // set hostname
  mdns_hostname_set("bbq");

  // set default instance
  mdns_instance_name_set("BBQ Thing");
  mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
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

static httpd_handle_t start_webserver(void) {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.lru_purge_enable = true;
  config.stack_size = 1 << 13;  // 8k

  // Start the httpd server
  ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
  if (httpd_start(&server, &config) == ESP_OK) {
    // Set URI handlers
    ESP_LOGI(TAG, "Registering URI handlers");
    httpd_register_uri_handler(server, &root_get);
    httpd_register_uri_handler(server, &settings_get);
    httpd_register_uri_handler(server, &settings_post);
    return server;
  }

  ESP_LOGI(TAG, "Error starting server!");
  return NULL;
}

static void stop_webserver(httpd_handle_t server) {
  // Stop the httpd server
  httpd_stop(server);
}

static void disconnect_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  httpd_handle_t *server = (httpd_handle_t *)arg;
  if (*server) {
    ESP_LOGI(TAG, "Stopping webserver");
    stop_webserver(*server);
    *server = NULL;
  }
}

static void connect_handler(void *arg, esp_event_base_t event_base,
                            int32_t event_id, void *event_data) {
  httpd_handle_t *server = (httpd_handle_t *)arg;
  if (*server == NULL) {
    ESP_LOGI(TAG, "Starting webserver");
    *server = start_webserver();
  }
}

extern "C" void app_main(void) {
  ESP_LOGI(TAG, "Hello bbq!");

  SESSION_ID = esp_random();

  controller.GetSettings().automatic_duty_pct = 70;
  controller.GetSettings().threshold_f = 225;

  mcpInit(&mcp, MCP_MODEL::MCP3008,
          19,  // GPIO_MISO
          23,  // GPIO_MOSI,
          18,  // GPIO_SCLK,
          5,   // GPIO_CS,
          MCP_INPUT::MCP_SINGLE);

  // Fan control.
  gpio_reset_pin((gpio_num_t)CONFIG_FAN_CONTROL_GPIO);
  gpio_set_direction((gpio_num_t)CONFIG_FAN_CONTROL_GPIO, GPIO_MODE_OUTPUT);

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

  start_mdns_service();

  static httpd_handle_t server = NULL;

  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &connect_handler, &server));
  ESP_ERROR_CHECK(esp_event_handler_register(
      WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));
  /* Start the server for the first time */
  server = start_webserver();

#if CONFIG_EXAMPLE_CONNECT_WIFI
  /* Ensure to disable any WiFi power save mode, this allows best throughput
   * and hence timings for overall OTA operation.
   */
  esp_wifi_set_ps(WIFI_PS_NONE);
#endif  // CONFIG_EXAMPLE_CONNECT_WIFI

  ESP_LOGI(TAG, "[APP] Free memory: %ld bytes", esp_get_free_heap_size());
  xTaskCreate(&bbq_task, "bbq_task", 8192, nullptr, 5, NULL);
}
