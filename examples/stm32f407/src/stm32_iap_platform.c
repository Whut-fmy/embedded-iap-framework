#include "stm32_iap_platform.h"

#include "stm32_iap_config.h"
#include "stm32_iap_test_protocol.h"
#include "stm32f407xx.h"
#include "stm32f4xx_hal.h"

#define STM32_IAP_IWDG_UNLOCK 0x5555u
#define STM32_IAP_IWDG_START 0xCCCCu
#define STM32_IAP_IWDG_RELOAD 0xAAAAu
#define STM32_IAP_IWDG_PRESCALER_256 6u
#define STM32_IAP_IWDG_RELOAD_VALUE 1000u

void stm32_iap_platform_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t HAL_GetTick(void)
{
    uint32_t cycles_per_millisecond = SystemCoreClock / 1000u;

    if (cycles_per_millisecond == 0u) {
        return 0u;
    }
    return DWT->CYCCNT / cycles_per_millisecond;
}

static int stack_pointer_is_valid(uint32_t stack_pointer)
{
    return (stack_pointer >= 0x20000000u && stack_pointer <= 0x20020000u) ||
           (stack_pointer >= 0x10000000u && stack_pointer <= 0x10010000u);
}

static int reset_handler_is_valid(iap_slot_t slot, uint32_t reset_handler)
{
    uint32_t address = reset_handler & ~1u;
    uint32_t slot_address = slot == IAP_SLOT_A
                                ? STM32_IAP_SLOT_A_ADDRESS
                                : STM32_IAP_SLOT_B_ADDRESS;

    return (reset_handler & 1u) != 0u &&
           address >= slot_address + STM32_IAP_IMAGE_HEADER_SIZE &&
           address < slot_address + STM32_IAP_SLOT_SIZE;
}

void stm32_iap_watchdog_start(void)
{
    uint32_t timeout = SystemCoreClock;

    IWDG->KR = STM32_IAP_IWDG_UNLOCK;
    IWDG->PR = STM32_IAP_IWDG_PRESCALER_256;
    IWDG->RLR = STM32_IAP_IWDG_RELOAD_VALUE;
    IWDG->KR = STM32_IAP_IWDG_START;
    while (IWDG->SR != 0u && timeout > 0u) {
        timeout--;
    }
    IWDG->KR = STM32_IAP_IWDG_RELOAD;
}

void stm32_iap_watchdog_feed(void)
{
    IWDG->KR = STM32_IAP_IWDG_RELOAD;
}

uint32_t stm32_iap_capture_reset_cause(void)
{
    uint32_t cause = RCC->CSR;
    RCC->CSR |= RCC_CSR_RMVF;
    return cause;
}

void stm32_iap_system_reset(void)
{
    __DSB();
    NVIC_SystemReset();
    for (;;) {
    }
}

void stm32_iap_power_cut_pause(uint32_t phase)
{
    stm32_iap_test_set_status(STM32_IAP_STATUS_POWER_CUT_ARMED, phase);
    for (;;) {
        stm32_iap_watchdog_feed();
    }
}

static void watchdog_start_callback(void *user)
{
    (void)user;
    stm32_iap_watchdog_start();
}

static void enter_recovery_callback(void *user, iap_status_t reason)
{
    (void)user;
    stm32_iap_test_set_status(STM32_IAP_STATUS_RECOVERY, (uint32_t)reason);
    for (;;) {
        stm32_iap_watchdog_feed();
    }
}

static void jump_to_slot_callback(void *user,
                                  iap_slot_t slot,
                                  uint32_t boot_address)
{
    const uint32_t *vectors = (const uint32_t *)(uintptr_t)boot_address;
    uint32_t stack_pointer = vectors[0];
    uint32_t reset_handler = vectors[1];
    uint32_t index;

    (void)user;
    if ((boot_address & (STM32_IAP_IMAGE_HEADER_SIZE - 1u)) != 0u ||
        !stack_pointer_is_valid(stack_pointer) ||
        !reset_handler_is_valid(slot, reset_handler)) {
        stm32_iap_test_set_status(STM32_IAP_STATUS_ERROR | (uint32_t)IAP_ERR_VERIFY,
                                  boot_address);
        enter_recovery_callback(NULL, IAP_ERR_VERIFY);
    }

    g_stm32_iap_test_control.last_slot = (uint32_t)slot;
    g_stm32_iap_test_control.last_boot_address = boot_address;
    stm32_iap_test_set_status(slot == IAP_SLOT_A
                                  ? STM32_IAP_STATUS_BOOTLOADER_JUMP_A
                                  : STM32_IAP_STATUS_BOOTLOADER_JUMP_B,
                              boot_address);

    __disable_irq();
    SysTick->CTRL = 0u;
    SysTick->LOAD = 0u;
    SysTick->VAL = 0u;
    for (index = 0u; index < 8u; index++) {
        NVIC->ICER[index] = UINT32_MAX;
        NVIC->ICPR[index] = UINT32_MAX;
    }

    SCB->VTOR = boot_address;
    __set_CONTROL(0u);
    __DSB();
    __ISB();
    __asm volatile(
        "msr msp, %0\n"
        "cpsie i\n"
        "isb\n"
        "bx %1\n"
        :
        : "r"(stack_pointer), "r"(reset_handler)
        : "memory");
    __builtin_unreachable();
}

iap_bootloader_ops_t stm32_iap_bootloader_ops(void)
{
    iap_bootloader_ops_t ops;

    ops.watchdog_start = watchdog_start_callback;
    ops.jump_to_slot = jump_to_slot_callback;
    ops.enter_recovery = enter_recovery_callback;
    ops.user = NULL;
    return ops;
}
