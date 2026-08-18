#include "iap/iap_bootloader.h"
#include "stm32_iap_platform.h"
#include "stm32_iap_storage.h"
#include "stm32_iap_test_protocol.h"

int main(void)
{
    iap_context_t ctx;
    stm32_iap_storage_t storage;
    iap_bootloader_ops_t ops;
    iap_status_t status;

    stm32_iap_platform_init();
    stm32_iap_test_control_init();
    g_stm32_iap_test_control.reset_cause = stm32_iap_capture_reset_cause();
    g_stm32_iap_test_control.boot_count++;
    stm32_iap_test_set_status(STM32_IAP_STATUS_BOOTLOADER_START, 0u);

    stm32_iap_watchdog_feed();
    status = stm32_iap_context_init(&ctx, &storage, IAP_SLOT_NONE);
    if (status != IAP_OK) {
        stm32_iap_test_set_status(STM32_IAP_STATUS_ERROR | (uint32_t)status, 1u);
        for (;;) {
            stm32_iap_watchdog_feed();
        }
    }

    ops = stm32_iap_bootloader_ops();
    status = iap_bootloader_run(&ctx, &ops, IAP_SLOT_A);
    stm32_iap_test_set_status(STM32_IAP_STATUS_ERROR | (uint32_t)status, 2u);

    for (;;) {
        stm32_iap_watchdog_feed();
    }
}
