#ifndef STM32_IAP_PLATFORM_H
#define STM32_IAP_PLATFORM_H

#include "iap/iap_bootloader.h"

iap_bootloader_ops_t stm32_iap_bootloader_ops(void);
void stm32_iap_platform_init(void);
void stm32_iap_watchdog_start(void);
void stm32_iap_watchdog_feed(void);
uint32_t stm32_iap_capture_reset_cause(void);
void stm32_iap_system_reset(void);
void stm32_iap_power_cut_pause(uint32_t phase);

#endif
