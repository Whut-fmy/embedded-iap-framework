#include "stm32_iap_test_protocol.h"

#include <string.h>

volatile stm32_iap_test_control_t g_stm32_iap_test_control
    __attribute__((section(".iap_test_control"), used, aligned(4)));

void stm32_iap_test_control_init(void)
{
    if (g_stm32_iap_test_control.magic == STM32_IAP_TEST_MAGIC &&
        g_stm32_iap_test_control.version == STM32_IAP_TEST_VERSION) {
        return;
    }

    memset((void *)&g_stm32_iap_test_control, 0, sizeof(g_stm32_iap_test_control));
    g_stm32_iap_test_control.magic = STM32_IAP_TEST_MAGIC;
    g_stm32_iap_test_control.version = STM32_IAP_TEST_VERSION;
}

void stm32_iap_test_set_status(uint32_t status, uint32_t detail)
{
    g_stm32_iap_test_control.status = status;
    g_stm32_iap_test_control.detail = detail;
    __asm volatile("dmb 0xF" ::: "memory");
}
