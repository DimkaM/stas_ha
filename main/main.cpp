// ---------------------------------------------------------------------------
//  stas_ha - pressure monitoring node (ESP-IDF 6.0 build, same toolchain as
//  D:\src\appsDM\testspi)
//  ESP32-C3 + ADS1115 + 0.5..4.5 V pressure sensor (0..1.2 MPa)
//
//  Data path:
//     sensor -> divider (3k0 / 8k2) -> ADS1115 A0 -> median filter ->
//     pressure in bar -> MQTT (state topic) -> Home Assistant auto-discovery
//
//  Wiring (see README.md):
//     ADS1115  VDD  -> 3V3         ADS1115 SDA -> GPIO5 (PIN_I2C_SDA)
//     ADS1115  GND  -> GND         ADS1115 SCL -> GPIO6 (PIN_I2C_SCL)
//     ADS1115  ADDR -> GND (0x48)  ADS1115 ALRT -> not connected
//     Sensor   VCC  -> 5V          Sensor GND -> GND
//     Sensor   OUT  -> R1 3k0 -> ADS A0 -> R2 8k2 -> GND
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "ads1115.h"
#include "config.h"
#include "secrets.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

namespace {

constexpr const char* TAG = "SYS";
constexpr const char* TAG_ADS = "ADS";
constexpr const char* TAG_WIFI = "WiFi";
constexpr const char* TAG_MQTT = "MQTT";
constexpr const char* TAG_SENS = "SENS";
constexpr const char* TAG_PUB = "PUB";

constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;

EventGroupHandle_t s_wifiEvents = nullptr;
esp_netif_t* s_staNetif = nullptr;
esp_mqtt_client_handle_t s_mqtt = nullptr;
stas_ha::Ads1115 s_ads;

bool s_adsReady = false;
bool s_mqttConnected = false;
bool s_discoveryPublished = false;
uint32_t s_lastAdsTry = 0;

char s_deviceId[48] = {0};  // e.g. stas_ha_pressure_a1b2c3d4e5f6
char s_clientId[56] = {0};
char s_brokerUri[96] = {0};

struct Reading {
  bool connected = false;
  float vAdc = NAN;     // volts on the ADS1115 input
  float vSensor = NAN;  // volts on the sensor output (before the divider)
  float bar = NAN;      // pressure, bar
};

// Unique id of this node, derived from the WiFi MAC address.
void buildDeviceId() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(s_deviceId, sizeof(s_deviceId),
           "%s_%02x%02x%02x%02x%02x%02x", NODE_ID, mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
  snprintf(s_clientId, sizeof(s_clientId), "%s", s_deviceId);
}

float medianOf(float* values, size_t n) {
  std::sort(values, values + n);
  return values[n / 2];
}

bool initAds() {
  return s_ads.begin(I2C_NUM_0, static_cast<gpio_num_t>(PIN_I2C_SDA),
                     static_cast<gpio_num_t>(PIN_I2C_SCL), ADS1115_ADDR,
                     I2C_CLOCK_HZ);
}

void mqttPublish(const char* topic, const char* payload, int qos, bool retain) {
  if (s_mqtt == nullptr || !s_mqttConnected) {
    return;
  }
  const int msgId =
      esp_mqtt_client_publish(s_mqtt, topic, payload, 0, qos, retain ? 1 : 0);
  if (msgId < 0) {
    ESP_LOGW(TAG_MQTT, "publish to %s failed", topic);
  }
}

// ------------------------------------------------------------------- sensor
Reading readPressure() {
  Reading r;
  if (!s_adsReady) {
    return r;  // connected == false
  }

  float samples[SAMPLES];
  for (uint8_t i = 0; i < SAMPLES; ++i) {
    int16_t raw = 0;
    if (!s_ads.readSingleEnded(ADS1115_CHANNEL, &raw)) {
      // Lost the ADC: mark it as not ready, the main loop re-probes it.
      s_adsReady = false;
      return r;
    }
    samples[i] = stas_ha::Ads1115::codeToVolts(raw);
  }

  r.vAdc = medianOf(samples, SAMPLES);
  r.vSensor = r.vAdc * DIVIDER_RATIO;

  // 0.5 V at 0 MPa .. 4.5 V at 1.2 MPa. Anything far below the zero point
  // means the sensor is unplugged or shorted to ground.
  if (r.vSensor < SENSOR_V_AT_ZERO * 0.5f ||
      r.vSensor > (SENSOR_V_AT_FULL + 0.5f)) {
    r.connected = false;
    return r;
  }

  const float mpa = (r.vSensor - SENSOR_V_AT_ZERO) /
                    (SENSOR_V_AT_FULL - SENSOR_V_AT_ZERO) *
                    SENSOR_FULL_SCALE_MPA;
  r.bar = std::min(std::max(mpa * MPA_TO_BAR, 0.0f), PRESSURE_MAX_BAR);
  r.connected = true;
  return r;
}

// --------------------------------------------------------------------- MQTT
void publishDiscoveryOne(const char* component, const char* objectId,
                         const char* payload) {
  char topic[160];
  snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config", DISCOVERY_PREFIX,
           component, s_deviceId, objectId);
  mqttPublish(topic, payload, 1, true);
}

void publishDiscovery() {
  char device[224];
  snprintf(device, sizeof(device),
           "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"stas_ha\","
           "\"mdl\":\"ESP32-C3 + ADS1115\",\"sw\":\"%s\"}",
           s_deviceId, DEVICE_NAME, FW_VERSION);

  char avail[192];
  snprintf(avail, sizeof(avail),
           "\"avty_t\":\"%s\",\"pl_avail\":\"online\",\"pl_not_avail\":"
           "\"offline\"",
           TOPIC_AVAIL);

  char payload[768];

  // Pressure sensor ---------------------------------------------------------
  snprintf(payload, sizeof(payload),
           "{\"name\":\"Давление воды\",\"uniq_id\":\"%s_pressure\","
           "\"stat_t\":\"%s\",%s,\"val_tpl\":"
           "\"{{ value_json.pressure_bar }}\",\"unit_of_meas\":\"bar\","
           "\"dev_cla\":\"pressure\",\"stat_cla\":\"measurement\","
           "\"ic\":\"mdi:gauge\",%s}",
           s_deviceId, TOPIC_STATE, avail, device);
  publishDiscoveryOne("sensor", "pressure", payload);

  // Sensor output voltage (diagnostic) -------------------------------------
  snprintf(payload, sizeof(payload),
           "{\"name\":\"Напряжение датчика\",\"uniq_id\":\"%s_sensor_voltage\","
           "\"stat_t\":\"%s\",%s,\"val_tpl\":"
           "\"{{ value_json.sensor_voltage }}\",\"unit_of_meas\":\"V\","
           "\"dev_cla\":\"voltage\",\"stat_cla\":\"measurement\","
           "\"ent_cat\":\"diagnostic\",%s}",
           s_deviceId, TOPIC_STATE, avail, device);
  publishDiscoveryOne("sensor", "sensor_voltage", payload);

  // ADC input voltage (diagnostic) -----------------------------------------
  snprintf(payload, sizeof(payload),
           "{\"name\":\"ADC напряжение\",\"uniq_id\":\"%s_adc_voltage\","
           "\"stat_t\":\"%s\",%s,\"val_tpl\":"
           "\"{{ value_json.adc_voltage }}\",\"unit_of_meas\":\"V\","
           "\"dev_cla\":\"voltage\",\"stat_cla\":\"measurement\","
           "\"ent_cat\":\"diagnostic\",%s}",
           s_deviceId, TOPIC_STATE, avail, device);
  publishDiscoveryOne("sensor", "adc_voltage", payload);

  // Sensor health as a binary sensor ---------------------------------------
  snprintf(payload, sizeof(payload),
           "{\"name\":\"Датчик давления\",\"uniq_id\":\"%s_connected\","
           "\"stat_t\":\"%s\",%s,\"val_tpl\":"
           "\"{{ 'ON' if value_json.connected else 'OFF' }}\","
           "\"dev_cla\":\"connectivity\",\"ent_cat\":\"diagnostic\",%s}",
           s_deviceId, TOPIC_STATE, avail, device);
  publishDiscoveryOne("binary_sensor", "connected", payload);

  s_discoveryPublished = true;
  ESP_LOGI(TAG_MQTT, "HA discovery published for %s", s_deviceId);
}

void publishState(const Reading& r) {
  char pressure[24];
  char sensorVoltage[24];
  if (r.connected) {
    snprintf(pressure, sizeof(pressure), "%.3f", r.bar);
    snprintf(sensorVoltage, sizeof(sensorVoltage), "%.3f", r.vSensor);
  } else {
    snprintf(pressure, sizeof(pressure), "null");
    snprintf(sensorVoltage, sizeof(sensorVoltage), "null");
  }

  char payload[224];
  snprintf(payload, sizeof(payload),
           "{\"pressure_bar\":%s,\"sensor_voltage\":%s,"
           "\"adc_voltage\":%.4f,\"connected\":%s}",
           pressure, sensorVoltage, r.vAdc, r.connected ? "true" : "false");

  // State is a fast-changing measurement: QoS 0 keeps the socket free of
  // PUBACK round-trips. Discovery/last-will stay QoS 1 + retained.
  mqttPublish(TOPIC_STATE, payload, 0, false);
  ESP_LOGI(TAG_PUB, "%s", payload);
}

void onMqttData(esp_mqtt_event_handle_t event) {
  char topic[96];
  char message[32];

  const int topicLen =
      std::min(event->topic_len, static_cast<int>(sizeof(topic) - 1));
  const int dataLen =
      std::min(event->data_len, static_cast<int>(sizeof(message) - 1));
  memcpy(topic, event->topic, topicLen);
  topic[topicLen] = '\0';
  memcpy(message, event->data, dataLen);
  message[dataLen] = '\0';

  ESP_LOGI(TAG_MQTT, "<- %s: %s", topic, message);

  if (strcmp(topic, TOPIC_CMD) != 0) {
    return;
  }
  if (strcasecmp(message, "restart") == 0) {
    ESP_LOGW(TAG_MQTT, "restart requested");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
  } else if (strcasecmp(message, "discovery") == 0) {
    s_discoveryPublished = false;  // re-announce on the next iteration
  }
}

void mqttEventHandler(void* /*args*/, esp_event_base_t /*base*/,
                      int32_t eventId, void* eventData) {
  auto* event = static_cast<esp_mqtt_event_handle_t>(eventData);
  switch (static_cast<esp_mqtt_event_id_t>(eventId)) {
    case MQTT_EVENT_CONNECTED:
      s_mqttConnected = true;
      ESP_LOGI(TAG_MQTT, "connected to %s:%d", MQTT_HOST, MQTT_PORT);
      mqttPublish(TOPIC_AVAIL, "online", 1, true);
      esp_mqtt_client_subscribe(s_mqtt, TOPIC_CMD, 1);
      s_discoveryPublished = false;
      break;
    case MQTT_EVENT_DISCONNECTED:
      s_mqttConnected = false;
      ESP_LOGW(TAG_MQTT, "disconnected from %s:%d", MQTT_HOST, MQTT_PORT);
      break;
    case MQTT_EVENT_DATA:
      onMqttData(event);
      break;
    case MQTT_EVENT_ERROR:
      ESP_LOGW(TAG_MQTT, "transport error, will retry");
      break;
    default:
      break;
  }
}

void initMqtt() {
  snprintf(s_brokerUri, sizeof(s_brokerUri), "mqtt://%s:%d", MQTT_HOST,
           MQTT_PORT);

  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = s_brokerUri;
  cfg.credentials.client_id = s_clientId;
  if (strlen(MQTT_USER) > 0) {
    cfg.credentials.username = MQTT_USER;
    cfg.credentials.authentication.password = MQTT_PASSWORD;
  }
  cfg.session.keepalive = 30;
  cfg.session.last_will.topic = TOPIC_AVAIL;
  cfg.session.last_will.msg = "offline";
  cfg.session.last_will.msg_len = 7;
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  cfg.network.reconnect_timeout_ms = 10000;
  cfg.network.timeout_ms = 30000;  // tolerate short Wi-Fi/network stalls
  cfg.buffer.size = 2048;          // HA discovery payloads are ~450 bytes

  s_mqtt = esp_mqtt_client_init(&cfg);
  if (s_mqtt == nullptr) {
    ESP_LOGE(TAG_MQTT, "esp_mqtt_client_init failed");
    return;
  }
  ESP_ERROR_CHECK(esp_mqtt_client_register_event(
      s_mqtt, MQTT_EVENT_ANY, mqttEventHandler, nullptr));
  ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt));
  ESP_LOGI(TAG_MQTT, "client started, broker %s", s_brokerUri);
}

// --------------------------------------------------------------------- WiFi
void wifiEventHandler(void* /*args*/, esp_event_base_t base, int32_t id,
                      void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(s_wifiEvents, WIFI_CONNECTED_BIT);
    ESP_LOGW(TAG_WIFI, "disconnected, reconnecting ...");
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    auto* event = static_cast<ip_event_got_ip_t*>(data);
    ESP_LOGI(TAG_WIFI, "connected to \"%s\", ip " IPSTR, WIFI_SSID,
             IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(s_wifiEvents, WIFI_CONNECTED_BIT);
  }
}

void initWifi() {
  s_wifiEvents = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  s_staNetif = esp_netif_create_default_wifi_sta();
  esp_netif_set_hostname(s_staNetif, OTA_HOSTNAME);

  wifi_init_config_t initCfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&initCfg));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, nullptr, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, nullptr, nullptr));

  wifi_config_t wifiCfg = {};
  strlcpy(reinterpret_cast<char*>(wifiCfg.sta.ssid), WIFI_SSID,
          sizeof(wifiCfg.sta.ssid));
  strlcpy(reinterpret_cast<char*>(wifiCfg.sta.password), WIFI_PASSWORD,
          sizeof(wifiCfg.sta.password));
  wifiCfg.sta.threshold.authmode =
      (strlen(WIFI_PASSWORD) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifiCfg));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG_WIFI, "connecting to \"%s\" ...", WIFI_SSID);
}

}  // namespace

// ---------------------------------------------------------------------------
extern "C" void app_main(void) {
  ESP_LOGI(TAG, "=== stas_ha pressure node (ESP-IDF %s) ===",
           esp_get_idf_version());

  buildDeviceId();
  ESP_LOGI(TAG, "device id: %s", s_deviceId);

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  s_adsReady = initAds();
  if (!s_adsReady) {
    ESP_LOGE(TAG_ADS,
             "not found on I2C bus (check SDA/SCL/ADDR/VDD), retry every %u ms",
             static_cast<unsigned>(I2C_RETRY_MS));
  }

  initWifi();
  initMqtt();

  ESP_LOGI(TAG, "free heap: %u bytes",
           static_cast<unsigned>(esp_get_free_heap_size()));

  for (;;) {
    const uint32_t now = esp_log_timestamp();

    // Re-probe the ADC if it was not detected at boot or has been lost.
    if (!s_adsReady && now - s_lastAdsTry >= I2C_RETRY_MS) {
      s_lastAdsTry = now;
      s_adsReady = initAds();
    }

    // The sensor is read even when MQTT is down: without a broker the log is
    // the only way to check the wiring, the ADC and the divider.
    const Reading r = readPressure();
    ESP_LOGI(TAG_SENS, "adc=%.4f V sensor=%.3f V pressure=%.3f bar (%s)",
             r.vAdc, r.vSensor, r.bar, r.connected ? "ok" : "no signal");

    if (s_mqttConnected) {
      if (!s_discoveryPublished) {
        publishDiscovery();
      }
      publishState(r);
    }

    vTaskDelay(pdMS_TO_TICKS(PUBLISH_INTERVAL_MS));
  }
}
