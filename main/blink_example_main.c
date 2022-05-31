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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"


//ADC Channels
#if CONFIG_IDF_TARGET_ESP32
#define ADC1_EXAMPLE_CHAN0          ADC1_CHANNEL_6
static const char *TAG_CH[2][10] = {{"ADC1_CH6"}, {"ADC2_CH0"}};
#else
#define ADC1_EXAMPLE_CHAN0          ADC1_CHANNEL_2
static const char *TAG_CH[2][10] = {{"ADC1_CH2"}, {"ADC2_CH0"}};
#endif

//ADC Attenuation
#define ADC_EXAMPLE_ATTEN           ADC_ATTEN_DB_11

//ADC Calibration
#if CONFIG_IDF_TARGET_ESP32
#define ADC_EXAMPLE_CALI_SCHEME     ESP_ADC_CAL_VAL_EFUSE_VREF
#elif CONFIG_IDF_TARGET_ESP32S2
#define ADC_EXAMPLE_CALI_SCHEME     ESP_ADC_CAL_VAL_EFUSE_TP
#elif CONFIG_IDF_TARGET_ESP32C3
#define ADC_EXAMPLE_CALI_SCHEME     ESP_ADC_CAL_VAL_EFUSE_TP
#elif CONFIG_IDF_TARGET_ESP32S3
#define ADC_EXAMPLE_CALI_SCHEME     ESP_ADC_CAL_VAL_EFUSE_TP_FIT
#endif


//static double CONSTANTS[] = {0.0007343140544,0.0002157437229,0.0000000951568577};
                            static double CONSTANTS[] = {7.3431401e-4,2.1574370e-4,9.5156860e-8};
static int adc_raw[2][10];
static const char *TAG = "ADC SINGLE";

static esp_adc_cal_characteristics_t adc1_chars;

static bool adc_calibration_init(void)
{
    esp_err_t ret;
    bool cali_enable = false;

    ret = esp_adc_cal_check_efuse(ADC_EXAMPLE_CALI_SCHEME);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Calibration scheme not supported, skip software calibration");
    } else if (ret == ESP_ERR_INVALID_VERSION) {
        ESP_LOGW(TAG, "eFuse not burnt, skip software calibration");
    } else if (ret == ESP_OK) {
        cali_enable = true;
        esp_adc_cal_characterize(ADC_UNIT_1, ADC_EXAMPLE_ATTEN, ADC_WIDTH_BIT_DEFAULT, 0, &adc1_chars);
    } else {
        ESP_LOGE(TAG, "Invalid arg");
    }

    return cali_enable;
}

void app_main(void)
{
    esp_err_t ret = ESP_OK;
    uint32_t voltage = 0;
    bool cali_enable = adc_calibration_init();

    //ADC1 config
    ESP_ERROR_CHECK(adc1_config_width(ADC_WIDTH_BIT_DEFAULT));
    ESP_ERROR_CHECK(adc1_config_channel_atten(ADC1_EXAMPLE_CHAN0, ADC_EXAMPLE_ATTEN));

    while (1) {
        adc_raw[0][0] = adc1_get_raw(ADC1_EXAMPLE_CHAN0);
        ESP_LOGI(TAG_CH[0][0], "raw  data: %d", adc_raw[0][0]);
        if (cali_enable) {
            voltage = esp_adc_cal_raw_to_voltage(adc_raw[0][0], &adc1_chars);
            ESP_LOGI(TAG_CH[0][0], "cali data: %d mV", voltage);


            // V = Vprobe + Vres
            // Vres = V - VProbe
            // Vres = 3300 - Vprobe
            // Vprobe / Rprobe = Vres / Rres
            // Rprobe = Vprobe * Rres / Vres 
            // Rprobe = Vprobe * 10000 / (3000 - Vprobe)
            double r_probe_ohm = ((double)voltage) * 10000.0 / (3300.0 - ((double)voltage));
            ESP_LOGI(TAG_CH[0][0], "resistance: %f ohm", r_probe_ohm);

            double log_r = log(r_probe_ohm);
            double temp_k = 1.0/(CONSTANTS[0] + CONSTANTS[1] * log_r + CONSTANTS[2]*log_r*log_r*log_r);
            ESP_LOGI(TAG_CH[0][0], "temp_k: %f", temp_k);
            double temp_f = (temp_k - 273.15)* 9/5 + 32;
            ESP_LOGI(TAG_CH[0][0], "temp_f: %f", temp_f);
            
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
