// ---------------------------------------------------------------------------
//  stas_ha - pressure monitoring node
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

#include <Arduino.h>
#include <algorithm>
#include <Wire.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_ADS1X15.h>

#include "config.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "include/secrets.h is missing - copy include/secrets.h.example to include/secrets.h and fill in WiFi/MQTT credentials"
#endif

namespace {

Adafruit_ADS1115 ads;
WiFiClient net;
PubSubClient mqtt(net);

String g_deviceId;   // unique id of this node, e.g. stas_ha_pressure_a1b2c3
String g_clientId;   // MQTT client id

uint32_t g_lastPublish = 0;
uint32_t g_lastWifiTry = 0;
uint32_t g_lastMqttTry = 0;
uint32_t g_lastAdsTry = 0;
bool g_adsReady = false;
bool g_discoverySent = false;

struct Reading {
  bool connected = false;
  float vAdc = NAN;     // volts on the ADS1115 input
  float vSensor = NAN;  // volts on the sensor output (before the divider)
  float bar = NAN;      // pressure, bar
};

String macSuffix() {
  const String mac = WiFi.macAddress();  // "AA:BB:CC:DD:EE:FF"
  String s;
  for (size_t i = 0; i < mac.length(); ++i) {
    const char c = mac[i];
    if (c != ':') {
      s += static_cast<char>(tolower(c));
    }
  }
  return s;
}

// --------------------------------------------------------------- ADC setup
bool initAds() {
  if (!ads.begin(ADS1115_ADDR, &Wire)) {
    Serial.println(F("[ADS] not found on I2C bus (check SDA/SCL/ADDR/VDD)"));
    return false;
  }
  ads.setGain(GAIN_ONE);                       // +-4.096 V, 0.125 mV/bit
  ads.setDataRate(RATE_ADS1115_128SPS);
  ads.setMode(0);                              // continuous conversion
  ads.readADC_SingleEnded(ADS1115_CHANNEL);    // kick off the first conversion
  Serial.println(F("[ADS] ADS1115 initialised at 0x48, gain +-4.096 V"));
  return true;
}

float medianOf(float* values, size_t n) {
  std::sort(values, values + n);
  return values[n / 2];
}

// ------------------------------------------------------------------ sensor
Reading readPressure() {
  Reading r;
  if (!g_adsReady) {
    return r;  // connected == false
  }

  float samples[SAMPLES];
  for (uint8_t i = 0; i < SAMPLES; ++i) {
    const int16_t raw = ads.readADC_SingleEnded(ADS1115_CHANNEL);
    samples[i] = ads.computeVolts(raw);
  }

  r.vAdc = medianOf(samples, SAMPLES);
  r.vSensor = r.vAdc * DIVIDER_RATIO;

  // 0.5 V at 0 MPa .. 4.5 V at 1.2 MPa. Anything far below the zero point
  // means the sensor is unplugged or shorted to ground.
  if (r.vSensor < SENSOR_V_AT_ZERO * 0.5f || r.vSensor > (SENSOR_V_AT_FULL + 0.5f)) {
    r.connected = false;
    return r;
  }

  const float mpa = (r.vSensor - SENSOR_V_AT_ZERO) /
                    (SENSOR_V_AT_FULL - SENSOR_V_AT_ZERO) * SENSOR_FULL_SCALE_MPA;
  r.bar = constrain(mpa * MPA_TO_BAR, 0.0f, PRESSURE_MAX_BAR);
  r.connected = true;
  return r;
}

// -------------------------------------------------------------------- MQTT
String deviceJson() {
  String dev = F("\"dev\":{\"ids\":[\"");
  dev += g_deviceId;
  dev += F("\"],\"name\":\"");
  dev += F(DEVICE_NAME);
  dev += F("\",\"mf\":\"stas_ha\",\"mdl\":\"ESP32-C3 + ADS1115\",\"sw\":\"1.0.0\"}");
  return dev;
}

void publishDiscoveryOne(const char* component, const char* objectId,
                         const String& body) {
  String topic = String(DISCOVERY_PREFIX) + "/" + component + "/" + g_deviceId +
                 "/" + objectId + "/config";
  mqtt.publish(topic.c_str(), body.c_str(), true);
}

void publishDiscovery() {
  const String dev = deviceJson();
  const String avail = String(F("\"avty_t\":\"")) + TOPIC_AVAIL + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\"";

  // Pressure sensor -------------------------------------------------------
  {
    String b = F("{\"name\":\"Давление воды\",\"uniq_id\":\"");
    b += g_deviceId + F("_pressure\",\"stat_t\":\"");
    b += TOPIC_STATE;
    b += F("\",");
    b += avail;
    b += F(",\"val_tpl\":\"{{ value_json.pressure_bar }}\",\"unit_of_meas\":\"bar\",\"dev_cla\":\"pressure\",\"stat_cla\":\"measurement\",\"ic\":\"mdi:gauge\",");
    b += dev;
    b += "}";
    publishDiscoveryOne("sensor", "pressure", b);
  }

  // Sensor output voltage (diagnostic) ------------------------------------
  {
    String b = F("{\"name\":\"Напряжение датчика\",\"uniq_id\":\"");
    b += g_deviceId + F("_sensor_voltage\",\"stat_t\":\"");
    b += TOPIC_STATE;
    b += F("\",");
    b += avail;
    b += F(",\"val_tpl\":\"{{ value_json.sensor_voltage }}\",\"unit_of_meas\":\"V\",\"dev_cla\":\"voltage\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\",");
    b += dev;
    b += "}";
    publishDiscoveryOne("sensor", "sensor_voltage", b);
  }

  // ADC input voltage (diagnostic) ----------------------------------------
  {
    String b = F("{\"name\":\"ADC напряжение\",\"uniq_id\":\"");
    b += g_deviceId + F("_adc_voltage\",\"stat_t\":\"");
    b += TOPIC_STATE;
    b += F("\",");
    b += avail;
    b += F(",\"val_tpl\":\"{{ value_json.adc_voltage }}\",\"unit_of_meas\":\"V\",\"dev_cla\":\"voltage\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\",");
    b += dev;
    b += "}";
    publishDiscoveryOne("sensor", "adc_voltage", b);
  }

  // Sensor health as a binary sensor --------------------------------------
  {
    String b = F("{\"name\":\"Датчик давления\",\"uniq_id\":\"");
    b += g_deviceId + F("_connected\",\"stat_t\":\"");
    b += TOPIC_STATE;
    b += F("\",");
    b += avail;
    b += F(",\"val_tpl\":\"{{ 'ON' if value_json.connected else 'OFF' }}\",\"dev_cla\":\"connectivity\",\"ent_cat\":\"diagnostic\",");
    b += dev;
    b += "}";
    publishDiscoveryOne("binary_sensor", "connected", b);
  }

  g_discoverySent = true;
  Serial.println(F("[MQTT] HA discovery published"));
}

void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  String msg;
  msg.reserve(len);
  for (unsigned int i = 0; i < len; ++i) {
    msg += static_cast<char>(payload[i]);
  }
  msg.trim();
  Serial.printf("[MQTT] <- %s: %s\n", topic, msg.c_str());

  if (strcmp(topic, TOPIC_CMD) == 0) {
    if (msg.equalsIgnoreCase("restart")) {
      Serial.println(F("[MQTT] restart requested"));
      delay(200);
      ESP.restart();
    } else if (msg.equalsIgnoreCase("discovery")) {
      g_discoverySent = false;  // re-announce on the next publish
    }
  }
}

bool mqttConnect() {
  const bool hasUser = strlen(MQTT_USER) > 0;
  const char* user = hasUser ? MQTT_USER : nullptr;
  const char* pass = hasUser ? MQTT_PASSWORD : nullptr;

  Serial.printf("[MQTT] connecting to %s:%d ...\n", MQTT_HOST, (int)MQTT_PORT);

  const bool ok = mqtt.connect(g_clientId.c_str(), user, pass, TOPIC_AVAIL, 1,
                               true, "offline");
  if (!ok) {
    Serial.printf("[MQTT] failed, rc=%d (%s)\n", mqtt.state(),
                  mqtt.state() == -2 ? "network/timeout"
                                     : "broker rejected credentials or unavailable");
    return false;
  }

  Serial.println(F("[MQTT] connected"));
  mqtt.publish(TOPIC_AVAIL, "online", true);
  mqtt.subscribe(TOPIC_CMD);
  g_discoverySent = false;
  return true;
}

void serviceMqtt() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (mqtt.connected()) {
    mqtt.loop();
    return;
  }
  const uint32_t now = millis();
  if (now - g_lastMqttTry >= 5000) {
    g_lastMqttTry = now;
    mqttConnect();
  }
}

// -------------------------------------------------------------------- WiFi
void serviceWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }
  const uint32_t now = millis();
  if (now - g_lastWifiTry < 10000) {
    return;
  }
  g_lastWifiTry = now;
  Serial.printf("[WiFi] connecting to \"%s\" ...\n", WIFI_SSID);
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// ---------------------------------------------------------------- publisher
void publishState(const Reading& r) {
  JsonDocument doc;
  if (r.connected) {
    doc["pressure_bar"] = serialized(String(r.bar, 3));
    doc["sensor_voltage"] = serialized(String(r.vSensor, 3));
  } else {
    doc["pressure_bar"] = nullptr;
    doc["sensor_voltage"] = nullptr;
  }
  doc["adc_voltage"] = serialized(String(r.vAdc, 4));
  doc["connected"] = r.connected;

  char buf[192];
  const size_t n = serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(TOPIC_STATE, reinterpret_cast<const uint8_t*>(buf), n, false);
  Serial.printf("[PUB] %s\n", buf);
}

}  // namespace

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println(F("=== stas_ha pressure node ==="));

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(OTA_HOSTNAME);

  g_deviceId = String(NODE_ID) + "_" + macSuffix();
  g_clientId = g_deviceId;
  Serial.printf("[SYS] device id: %s\n", g_deviceId.c_str());

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_CLOCK_HZ);
  g_adsReady = initAds();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(1024);        // HA discovery payloads exceed the 256 default
  mqtt.setKeepAlive(30);

  Serial.printf("[WiFi] connecting to \"%s\" ...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  g_lastWifiTry = millis();

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { Serial.println(F("[OTA] update started")); });
  ArduinoOTA.onEnd([]() { Serial.println(F("\n[OTA] update finished")); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] error %u\n", e); });
  ArduinoOTA.begin();

  Serial.printf("[SYS] free heap: %u bytes\n", (unsigned)ESP.getFreeHeap());
}

void loop() {
  ArduinoOTA.handle();
  serviceWifi();
  serviceMqtt();

  // Re-probe the ADC if it was not detected at boot.
  if (!g_adsReady) {
    const uint32_t now = millis();
    if (now - g_lastAdsTry >= I2C_RETRY_MS) {
      g_lastAdsTry = now;
      g_adsReady = initAds();
    }
  }

  if (WiFi.status() != WL_CONNECTED || !mqtt.connected()) {
    return;
  }

  if (!g_discoverySent) {
    publishDiscovery();
  }

  const uint32_t now = millis();
  if (now - g_lastPublish >= PUBLISH_INTERVAL_MS || g_lastPublish == 0) {
    g_lastPublish = now;
    const Reading r = readPressure();
    publishState(r);
    Serial.printf("[SENS] adc=%.4f V sensor=%.3f V pressure=%.3f bar (%s)\n",
                  r.vAdc, r.vSensor, r.bar, r.connected ? "ok" : "no signal");
  }
}