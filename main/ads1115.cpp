#include "ads1115.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace stas_ha {
namespace {

constexpr const char* TAG = "ADS";
constexpr uint8_t REG_CONVERSION = 0x00;
constexpr uint8_t REG_CONFIG = 0x01;

constexpr int I2C_TIMEOUT_MS = 50;
constexpr uint32_t CONVERSION_WAIT_MS = 10;  // 128 SPS -> 7.8 ms per sample

}  // namespace

bool Ads1115::begin(i2c_port_num_t port, gpio_num_t sda_gpio,
                    gpio_num_t scl_gpio, uint8_t address, uint32_t scl_hz) {
  end();

  i2c_master_bus_config_t bus_cfg = {};
  bus_cfg.i2c_port = port;
  bus_cfg.sda_io_num = sda_gpio;
  bus_cfg.scl_io_num = scl_gpio;
  bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_cfg.glitch_ignore_cnt = 7;
  bus_cfg.flags.enable_internal_pullup = true;

  esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
    bus_ = nullptr;
    return false;
  }

  i2c_device_config_t dev_cfg = {};
  dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev_cfg.device_address = address;
  dev_cfg.scl_speed_hz = scl_hz;

  err = i2c_master_bus_add_device(bus_, &dev_cfg, &dev_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
    i2c_del_master_bus(bus_);
    bus_ = nullptr;
    dev_ = nullptr;
    return false;
  }

  // Probe the config register; the ADS1115 must answer with an ACK.
  uint16_t probe = 0;
  if (!readRegister(REG_CONFIG, &probe)) {
    ESP_LOGW(TAG, "no answer from ADS1115 at 0x%02x", address);
    end();
    return false;
  }

  ESP_LOGI(TAG, "ADS1115 initialised at 0x%02x, gain +-4.096 V, 128 SPS",
           address);
  return true;
}

void Ads1115::end() {
  if (dev_) {
    i2c_master_bus_rm_device(dev_);
    dev_ = nullptr;
  }
  if (bus_) {
    i2c_del_master_bus(bus_);
    bus_ = nullptr;
  }
}

bool Ads1115::readSingleEnded(uint8_t channel, int16_t* raw) {
  if (!dev_ || raw == nullptr || channel > 3) {
    return false;
  }

  // OS=1 (start single conversion), MUX=AINx-GND, PGA=+-4.096 V,
  // MODE=single-shot, DR=128 SPS, comparator disabled.
  const uint16_t config = static_cast<uint16_t>(
      0x8000 | ((0x04 + channel) << 12) | (0x01 << 9) | (0x01 << 8) |
      (0x04 << 5) | 0x0003);

  if (!writeRegister(REG_CONFIG, config)) {
    return false;
  }

  vTaskDelay(pdMS_TO_TICKS(CONVERSION_WAIT_MS));

  uint16_t value = 0;
  if (!readRegister(REG_CONVERSION, &value)) {
    return false;
  }

  *raw = static_cast<int16_t>(value);
  return true;
}

bool Ads1115::writeRegister(uint8_t reg, uint16_t value) {
  const uint8_t buf[3] = {reg, static_cast<uint8_t>(value >> 8),
                          static_cast<uint8_t>(value & 0xFF)};
  const esp_err_t err = i2c_master_transmit(dev_, buf, sizeof(buf),
                                            I2C_TIMEOUT_MS);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "write reg 0x%02x failed: %s", reg, esp_err_to_name(err));
    return false;
  }
  return true;
}

bool Ads1115::readRegister(uint8_t reg, uint16_t* value) {
  uint8_t buf[2] = {0, 0};
  const esp_err_t err = i2c_master_transmit_receive(dev_, &reg, 1, buf,
                                                    sizeof(buf), I2C_TIMEOUT_MS);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "read reg 0x%02x failed: %s", reg, esp_err_to_name(err));
    return false;
  }
  *value = static_cast<uint16_t>((buf[0] << 8) | buf[1]);
  return true;
}

}  // namespace stas_ha
