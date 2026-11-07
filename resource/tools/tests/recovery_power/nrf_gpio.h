#pragma once
#include <stdint.h>
#define NRF_GPIO_PIN_SENSE_HIGH 2u
uint32_t nrf_gpio_pin_read(uint32_t pin);
void nrf_gpio_cfg_output(uint32_t pin);
void nrf_gpio_pin_clear(uint32_t pin);
void nrf_gpio_pin_set(uint32_t pin);
void nrf_gpio_cfg_sense_input(uint32_t pin, uint32_t pull, uint32_t sense);
