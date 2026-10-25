#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"

namespace stas_ha {

// Minimal register-level driver for the ADS1115 16-bit I2C ADC.
// Only what the pressure node needs: single-ended readings of AIN0..AIN3
// with GAIN_ONE (+-4.096 V, 0.125 mV per LSB) in single-shot mode.
class Ads1115 {
 public:
  bool begin(i2c_port_num_t port, gpio_num_t sda_gpio, gpio_num_t scl_gpio,
             uint8_t address, uint32_t scl_hz);
  void end();

  bool isReady() const { return dev_ != nullptr; }

  // Reads AINx vs GND. Returns false on I2C error.
  bool readSingleEnded(uint8_t channel, int16_t* raw);

  // GAIN_ONE: 1 LSB = 4.096 V / 32768
  static float codeToVolts(int16_t raw) { return raw * (4.096f / 32768.0f); }

 private:
  bool writeRegister(uint8_t reg, uint16_t value);
  bool readRegister(uint8_t reg, uint16_t* value);

  i2c_master_bus_handle_t bus_ = nullptr;
  i2c_master_dev_handle_t dev_ = nullptr;
};

}  // namespace stas_ha
