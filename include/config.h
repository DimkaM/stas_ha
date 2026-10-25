#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
//  stas_ha pressure node - hardware configuration
//
//  ESP32-C3 (USB-UART bridge -> /dev/ttyACM0)
//     |
//     +-- I2C --> ADS1115 (addr 0x48, ADDR -> GND), VDD = 3V3
//     |            A0 <- voltage divider <- pressure sensor OUT
//     |            A1..A3 free
//     |
//     +-- 5V supply for the pressure sensor (GND common with ESP32 and ADS1115)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------- I2C / ADC
// ESP32-C3 free GPIOs: not strapping, not flash, not USB.
#define PIN_I2C_SDA 5
#define PIN_I2C_SCL 6
#define I2C_CLOCK_HZ 400000

#define ADS1115_ADDR 0x48  // ADDR pin -> GND
#define ADS1115_CHANNEL 0  // A0

// --------------------------------------------------------- Voltage divider
// Sensor OUT ---[ R1 ]--- ADS A0 ---[ R2 ]--- GND
//   R1 = 3.0 kOhm (series), R2 = 8.2 kOhm (to GND)
//   V_ads = V_sensor * R2 / (R1 + R2)   ->  4.5 V becomes ~3.30 V
//   +-4.096 V range of the ADS1115, 0.125 mV per LSB
constexpr float DIVIDER_R1_OHM = 3000.0f;
constexpr float DIVIDER_R2_OHM = 8200.0f;
constexpr float DIVIDER_RATIO = (DIVIDER_R1_OHM + DIVIDER_R2_OHM) / DIVIDER_R2_OHM;  // ~1.3659

// ------------------------------------------------------- Pressure sensor
// 3-wire sensor: 5 V supply, output 0.5 V .. 4.5 V for 0 .. 1.2 MPa
constexpr float SENSOR_V_AT_ZERO = 0.5f;    // output at 0 MPa
constexpr float SENSOR_V_AT_FULL = 4.5f;    // output at 1.2 MPa
constexpr float SENSOR_FULL_SCALE_MPA = 1.2f;
constexpr float MPA_TO_BAR = 10.0f;
constexpr float PRESSURE_MAX_BAR = 20.0f;   // sanity clamp for the UI

// --------------------------------------------------------------- Timing
constexpr uint32_t PUBLISH_INTERVAL_MS = 5000;  // MQTT publish period
constexpr uint8_t SAMPLES = 9;                  // median filter depth
constexpr uint32_t I2C_RETRY_MS = 10000;

// --------------------------------------------------------------- MQTT / device
#define NODE_ID "stas_ha_pressure"
#define DEVICE_NAME "stas_ha pressure"
#define TOPIC_STATE "stas_ha/pressure/state"
#define TOPIC_AVAIL "stas_ha/pressure/availability"
#define TOPIC_CMD "stas_ha/pressure/cmd"
#define DISCOVERY_PREFIX "homeassistant"