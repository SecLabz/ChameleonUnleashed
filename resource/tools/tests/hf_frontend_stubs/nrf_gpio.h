#pragma once
#include <stdint.h>
#define NRF_GPIO_PIN_NOPULL 0u
void nrf_gpio_pin_clear(uint32_t pin);
void nrf_gpio_pin_set(uint32_t pin);
void nrf_gpio_cfg_output(uint32_t pin);
void nrf_gpio_cfg_input(uint32_t pin, uint32_t pull);
