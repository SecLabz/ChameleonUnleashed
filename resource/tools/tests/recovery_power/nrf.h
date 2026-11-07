#pragma once
#include <stdint.h>

typedef struct { volatile uint32_t POWERCLR; } test_ram_t;
typedef struct {
    volatile uint32_t USBREGSTATUS, GPREGRET, GPREGRET2;
    test_ram_t RAM[9];
} test_power_t;
typedef struct { volatile uint32_t ENABLE; } test_usbd_t;
typedef struct { volatile uint32_t READY; } test_nvmc_t;
typedef struct { volatile uint32_t TASKS_STOP, INTENCLR, EVTENCLR; } test_rtc_t;
extern test_power_t test_power;
extern test_usbd_t test_usbd;
extern test_nvmc_t test_nvmc;
extern test_rtc_t test_rtc;
#define NRF_POWER (&test_power)
#define NRF_USBD (&test_usbd)
#define NRF_NVMC (&test_nvmc)
#define NRF_RTC1 (&test_rtc)
#define POWER_USBREGSTATUS_VBUSDETECT_Msk 1u
#define NVMC_READY_READY_Ready 1u
#define POWER_RAM_POWER_S0RETENTION_Msk (1u << 16)
#define POWER_RAM_POWER_S1RETENTION_Msk (1u << 17)
#define POWER_RAM_POWER_S2RETENTION_Msk (1u << 18)
#define POWER_RAM_POWER_S3RETENTION_Msk (1u << 19)
#define POWER_RAM_POWER_S4RETENTION_Msk (1u << 20)
#define POWER_RAM_POWER_S5RETENTION_Msk (1u << 21)
uint32_t __get_PRIMASK(void);
void __disable_irq(void);
void __set_PRIMASK(uint32_t value);
