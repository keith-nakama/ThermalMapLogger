#if !defined(ARDUINO) && __has_include("wokwi-api.h")
#include "wokwi-api.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint8_t reg;
  bool has_reg;
  uint32_t frame;
  uint8_t registers[256];
} chip_state_t;

static void set_raw_pixel(chip_state_t *chip, uint8_t index, int16_t raw) {
  uint8_t base = 0x80 + index * 2;
  chip->registers[base] = raw & 0xff;
  chip->registers[base + 1] = (raw >> 8) & 0x0f;
}

static void update_frame(chip_state_t *chip) {
  chip->frame++;

  for (uint8_t i = 0; i < 64; i++) {
    uint8_t row = i / 8;
    uint8_t col = i % 8;
    int16_t temp_c = 10 + row * 7 + col * 2 + ((chip->frame / 4) % 8);
    if (temp_c > 80) {
      temp_c = 80;
    }
    set_raw_pixel(chip, i, temp_c * 4);
  }

  int16_t thermistor_raw = 28 * 16;
  chip->registers[0x0e] = thermistor_raw & 0xff;
  chip->registers[0x0f] = (thermistor_raw >> 8) & 0x0f;
}

static bool on_i2c_connect(void *user_data, uint32_t address, bool read) {
  chip_state_t *chip = (chip_state_t *)user_data;
  (void)address;
  if (!read) {
    // Wireのwrite-then-readでは、各writeトランザクションの先頭バイトがレジスタアドレス。
    chip->has_reg = false;
  }
  return true;
}

static uint8_t on_i2c_read(void *user_data) {
  chip_state_t *chip = (chip_state_t *)user_data;
  if (chip->reg == 0x80) {
    update_frame(chip);
  }
  return chip->registers[chip->reg++];
}

static bool on_i2c_write(void *user_data, uint8_t data) {
  chip_state_t *chip = (chip_state_t *)user_data;
  if (!chip->has_reg) {
    chip->reg = data;
    chip->has_reg = true;
  } else {
    chip->registers[chip->reg++] = data;
  }
  return true;
}

static void on_i2c_disconnect(void *user_data) {
  chip_state_t *chip = (chip_state_t *)user_data;
  chip->has_reg = false;
}

void chip_init(void) {
  chip_state_t *chip = malloc(sizeof(chip_state_t));
  memset(chip, 0, sizeof(chip_state_t));
  update_frame(chip);

  i2c_config_t config = {
    .address = 0x68,
    .scl = pin_init("SCL", INPUT_PULLUP),
    .sda = pin_init("SDA", INPUT_PULLUP),
    .connect = on_i2c_connect,
    .read = on_i2c_read,
    .write = on_i2c_write,
    .disconnect = on_i2c_disconnect,
    .user_data = chip
  };
  i2c_init(&config);
}
#endif
