// Copyright (C) 2017 Rob Caelers <rob.caelers@gmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#define _GLIBCXX_USE_C99

#include <iostream>
#include <string>

#include "os/Wifi.hpp"
#include "os/BLEScanner.hpp"
#include "os/Slot.hpp"
#include "os/Task.hpp"
#include "os/MainLoop.hpp"
#include "os/MqttClient.hpp"
#include "os/hexdump.hpp"
#include "os/json.hpp"

#include "string.h"

extern "C"
{
#include "esp_heap_trace.h"
}
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "nvs_flash.h"

#include "user_config.h"

using json = nlohmann::json;

static const char tag[] = "BEACON-SCANNER";

extern const uint8_t ca_start[] asm("_binary_CA_crt_start");
extern const uint8_t ca_end[] asm("_binary_CA_crt_end");
extern const uint8_t certificate_start[] asm("_binary_esp32_crt_start");
extern const uint8_t certificate_end[] asm("_binary_esp32_crt_end");
extern const uint8_t private_key_start[] asm("_binary_esp32_key_start");
extern const uint8_t private_key_end[] asm("_binary_esp32_key_end");

// #define NUM_RECORDS 100
// static heap_trace_record_t trace_record[NUM_RECORDS]; // This buffer must be in internal RAM

class Main
{
public:
  Main():
    beacon_scanner(os::BLEScanner::instance()),
    wifi(os::Wifi::instance()),
    loop(std::make_shared<os::MainLoop>()),
    mqtt(std::make_shared<os::MqttClient>(loop, "BLEScanner", MQTT_HOST, MQTT_PORT)),
    task("main_task", std::bind(&Main::main_task, this))
  {
    gpio_pad_select_gpio(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

    std::string mac = wifi.get_mac();
    topic_config = "/beaconscanner/config/" + mac;
    topic_scan = "/beaconscanner/scan/" + mac;
  }

private:
  // https://stackoverflow.com/questions/180947/base64-decode-snippet-in-c
  static std::string base64_encode(const std::string &in)
  {
    std::string out;

    int val=0, valb=-6;
    for (char c : in)
      {
        val = (val<<8) + c;
        valb += 8;
        while (valb>=0)
          {
            out.push_back("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[(val>>valb)&0x3F]);
            valb-=6;
          }
      }
    if (valb>-6) out.push_back("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[((val<<8)>>(valb+8))&0x3F]);
    while (out.size()%4) out.push_back('=');
    return out;
  }

  void on_wifi_system_event(system_event_t event)
  {
    ESP_LOGI(tag, "-> System event %d", event.event_id);
  }

  void on_wifi_timeout()
  {
    ESP_LOGI(tag, "-> Wifi timer");
    wifi_timer = 0;
    if (!wifi.connected().get())
      {
        ESP_LOGI(tag, "-> Wifi failed to connect in time. Reset");
        wifi.reconnect();
        wifi_timer = loop->add_timer(std::chrono::milliseconds(5000), std::bind(&Main::on_wifi_timeout, this));
      }
  }

  void on_wifi_connected(bool connected)
  {
    if (connected)
      {
        ESP_LOGI(tag, "-> Wifi connected");
        loop->cancel_timer(wifi_timer);
#ifdef MQTT_USER
        mqtt->set_username(MQTT_USER);
#endif
#ifdef MQTT_PASS
        mqtt->set_password(MQTT_PASS);
#endif
#ifdef MQTT_USE_TLS
        mqtt->set_client_certificate(reinterpret_cast<const char *>(certificate_start), reinterpret_cast<const char *>(private_key_start));
        mqtt->set_ca_certificate(reinterpret_cast<const char *>(ca_start));
#endif
        mqtt->set_callback(os::make_slot(loop, [this] (std::string topic, std::string payload) { on_mqtt_data(topic, payload);} ));
        mqtt->connected().connect(loop, std::bind(&Main::on_mqtt_connected, this, std::placeholders::_1));
        mqtt->connect();
      }
    else
      {
        ESP_LOGI(tag, "-> Wifi disconnected");
      }
  }

  void on_mqtt_connected(bool connected)
  {
    if (connected)
      {
        ESP_LOGI(tag, "-> MQTT connected");
        ESP_LOGI(tag, "-> Requesting configuration at %s", topic_config.c_str());
        mqtt->subscribe(topic_config);
        start_beacon_scan();
      }
    else
      {
        ESP_LOGI(tag, "-> MQTT disconnected");
        stop_beacon_scan();
      }
  }

  void on_mqtt_data(std::string topic, std::string payload)
  {
    ESP_LOGI(tag, "-> MQTT %s -> %s (free %d)", topic.c_str(), payload.c_str(), heap_caps_get_free_size(MALLOC_CAP_DEFAULT));

    if (topic == topic_config)
      {
        // TODO:
        // beacon_scanner.start();
      }
  }

  void on_beacon_scanner_scan_result(os::BLEScanner::ScanResult result)
  {
    static int led_state = 0;

    led_state ^= 1;
    gpio_set_level(LED_GPIO, led_state);
    scan_results[result.bda_as_string()] = result;
  }

  void on_scan_timer()
  {
    json j;

    if (mqtt->connected().get())
      {
        for (auto kv : scan_results)
          {
            json jb;
            jb["mac"] = kv.second.bda_as_string();
            jb["bda"] = base64_encode(std::string(reinterpret_cast<char *>(kv.second.bda), sizeof(kv.second.bda)));
            jb["rssi"] = kv.second.rssi;
            jb["adv_data"] = base64_encode(kv.second.adv_data);
            j.push_back(jb);
          }

        mqtt->publish(topic_scan, j.dump());
      }
    scan_results.clear();
  }

  void start_beacon_scan()
  {
    scan_timer = loop->add_periodic_timer(std::chrono::milliseconds(1000), std::bind(&Main::on_scan_timer, this));
    beacon_scanner.start();
  }

  void stop_beacon_scan()
  {
    loop->cancel_timer(scan_timer);
    scan_timer = 0;

    beacon_scanner.stop();
  }

  void main_task()
  {
    wifi.set_ssid(WIFI_SSID);
    wifi.set_passphase(WIFI_PASS);
    wifi.set_host_name("scan");
    wifi.set_auto_connect(true);
    wifi.system_event_signal().connect(loop, std::bind(&Main::on_wifi_system_event, this, std::placeholders::_1));
    wifi.connected().connect(loop, std::bind(&Main::on_wifi_connected, this, std::placeholders::_1));

    beacon_scanner.scan_result_signal().connect(loop, std::bind(&Main::on_beacon_scanner_scan_result, this, std::placeholders::_1));

    wifi_timer = loop->add_timer(std::chrono::milliseconds(5000), std::bind(&Main::on_wifi_timeout, this));
    wifi.connect();

    heap_caps_print_heap_info(MALLOC_CAP_DEFAULT);
    // heap_trace_init_standalone(trace_record, NUM_RECORDS);
    // ESP_ERROR_CHECK( heap_trace_start(HEAP_TRACE_ALL) );

    loop->run();
  }

  os::BLEScanner &beacon_scanner;
  os::Wifi &wifi;
  std::shared_ptr<os::MainLoop> loop;
  std::shared_ptr<os::MqttClient> mqtt;
  os::Task task;
  os::MainLoop::timer_id wifi_timer = 0;
  os::MainLoop::timer_id scan_timer = 0;
  std::map<std::string, os::BLEScanner::ScanResult> scan_results;
  std::string topic_config;
  std::string topic_scan;

  const static gpio_num_t LED_GPIO = GPIO_NUM_5;
};



extern "C" void
app_main()
{
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES)
    {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
  ESP_ERROR_CHECK(ret);

  ESP_LOGI(tag, "HEAP: startup");
  heap_caps_print_heap_info(MALLOC_CAP_DEFAULT);

  new Main();

  while(1)
    {
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
