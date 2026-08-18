#include "iap/iap.h"
#include "stm32_iap_config.h"
#include "stm32_iap_platform.h"
#include "stm32_iap_storage.h"
#include "stm32_iap_test_protocol.h"
#include "stm32f407xx.h"

#ifndef IAP_APP_SLOT
#error "IAP_APP_SLOT must be IAP_SLOT_A or IAP_SLOT_B"
#endif

static const iap_slot_t current_slot = (iap_slot_t)IAP_APP_SLOT;

static iap_slot_t other_slot(void)
{
    return current_slot == IAP_SLOT_A ? IAP_SLOT_B : IAP_SLOT_A;
}

static void report_error(iap_status_t status, uint32_t stage)
{
    stm32_iap_test_set_status(STM32_IAP_STATUS_ERROR | (uint32_t)status, stage);
    g_stm32_iap_test_control.command = STM32_IAP_CMD_NONE;
}

static void report_unexpected_status(iap_status_t status, uint32_t stage)
{
    report_error(status == IAP_OK ? IAP_ERR_VERIFY : status, stage);
}

static void install_slot_b(iap_context_t *ctx)
{
    iap_boot_control_t control;
    iap_status_t status;
    uint32_t image_size = g_stm32_iap_test_control.argument0;

    if (current_slot != IAP_SLOT_A ||
        image_size == 0u ||
        image_size > STM32_IAP_STAGING_SIZE) {
        report_error(IAP_ERR_INVALID_ARG, 10u);
        return;
    }

    stm32_iap_test_set_status(STM32_IAP_STATUS_INSTALLING_SLOT_B, image_size);
    stm32_iap_watchdog_feed();
    status = iap_write_candidate_image(ctx,
                                       IAP_SLOT_B,
                                       (const void *)(uintptr_t)STM32_IAP_STAGING_ADDRESS,
                                       image_size);
    if (status != IAP_OK) {
        report_error(status, 11u);
        return;
    }

    status = iap_boot_control_load(ctx, &control);
    if (status != IAP_OK) {
        report_error(status, 12u);
        return;
    }

    status = iap_mark_pending(ctx, &control, IAP_SLOT_B);
    if (status != IAP_OK) {
        report_error(status, 13u);
        return;
    }

    stm32_iap_test_set_status(STM32_IAP_STATUS_INSTALL_OK, image_size);
    g_stm32_iap_test_control.command = STM32_IAP_CMD_HOLD_PENDING;
    __DMB();
    stm32_iap_system_reset();
}

static void confirm_current(iap_context_t *ctx)
{
    iap_boot_control_t control;
    iap_status_t status;

    status = iap_boot_control_load(ctx, &control);
    if (status != IAP_OK) {
        report_error(status, 20u);
        return;
    }

    status = iap_confirm_current_image(ctx, &control, current_slot);
    if (status != IAP_OK) {
        report_error(status, 21u);
        return;
    }

    g_stm32_iap_test_control.command = STM32_IAP_CMD_NONE;
    stm32_iap_test_set_status(STM32_IAP_STATUS_CONFIRMED, (uint32_t)current_slot);
}

static void handle_failure_command(iap_context_t *ctx, int *feed_watchdog)
{
    iap_boot_control_t control;
    iap_status_t status;

    if (current_slot == IAP_SLOT_A) {
        status = iap_boot_control_load(ctx, &control);
        if (status == IAP_OK && control.pending_slot == (uint32_t)IAP_SLOT_NONE) {
            g_stm32_iap_test_control.command = STM32_IAP_CMD_NONE;
            stm32_iap_test_set_status(STM32_IAP_STATUS_ROLLED_BACK,
                                      control.boot_attempts);
            *feed_watchdog = 1;
            return;
        }
    }

    stm32_iap_test_set_status(STM32_IAP_STATUS_FAILING, (uint32_t)current_slot);
    *feed_watchdog = 0;
}

static void report_pending_state(iap_context_t *ctx)
{
    iap_boot_control_t control;
    iap_status_t status = iap_boot_control_load(ctx, &control);

    if (status != IAP_OK) {
        report_error(status, 40u);
        return;
    }

    if (control.active_slot == (uint32_t)current_slot &&
        control.pending_slot == (uint32_t)IAP_SLOT_NONE) {
        g_stm32_iap_test_control.command = STM32_IAP_CMD_NONE;
        stm32_iap_test_set_status(STM32_IAP_STATUS_ROLLED_BACK,
                                  (uint32_t)current_slot);
        return;
    }

    stm32_iap_test_set_status(STM32_IAP_STATUS_PENDING_HELD,
                              (uint32_t)current_slot);
}

static void test_storage_read_failure(iap_context_t *ctx,
                                      stm32_iap_storage_t *storage)
{
    iap_boot_control_t control;
    iap_slot_t selected_slot = IAP_SLOT_NONE;
    iap_slot_t target_slot = other_slot();
    iap_status_t status;

    status = iap_boot_control_load(ctx, &control);
    if (status != IAP_OK) {
        report_error(status, 50u);
        return;
    }
    if (control.active_slot != (uint32_t)current_slot ||
        control.pending_slot != (uint32_t)IAP_SLOT_NONE) {
        report_error(IAP_ERR_BOOT_CONTROL, 51u);
        return;
    }

    status = iap_mark_pending(ctx, &control, target_slot);
    if (status != IAP_OK) {
        report_error(status, 52u);
        return;
    }

    stm32_iap_storage_fail_next_read(storage);
    status = iap_select_boot_slot(ctx, &control, &selected_slot);
    if (status != IAP_ERR_STORAGE || selected_slot != IAP_SLOT_NONE ||
        control.pending_slot != (uint32_t)target_slot ||
        control.boot_attempts != 0u) {
        report_unexpected_status(status, 53u);
        return;
    }

    status = iap_rollback(ctx, &control);
    if (status != IAP_OK) {
        report_error(status, 54u);
        return;
    }

    g_stm32_iap_test_control.command = STM32_IAP_CMD_NONE;
    stm32_iap_test_set_status(STM32_IAP_STATUS_READ_FAILURE_PASSED,
                              (uint32_t)target_slot);
}

static void test_control_write_failure(iap_context_t *ctx,
                                       stm32_iap_storage_t *storage)
{
    iap_boot_control_t control;
    iap_boot_control_t persisted;
    iap_slot_t target_slot = other_slot();
    iap_status_t status;

    status = iap_boot_control_load(ctx, &control);
    if (status != IAP_OK) {
        report_error(status, 60u);
        return;
    }
    if (control.active_slot != (uint32_t)current_slot ||
        control.pending_slot != (uint32_t)IAP_SLOT_NONE) {
        report_error(IAP_ERR_BOOT_CONTROL, 61u);
        return;
    }

    stm32_iap_storage_fail_next_write(storage);
    status = iap_mark_pending(ctx, &control, target_slot);
    if (status != IAP_ERR_STORAGE ||
        control.pending_slot != (uint32_t)IAP_SLOT_NONE) {
        report_unexpected_status(status, 62u);
        return;
    }

    status = iap_boot_control_load(ctx, &persisted);
    if (status != IAP_OK) {
        report_error(status, 63u);
        return;
    }
    if (persisted.active_slot != (uint32_t)current_slot ||
        persisted.pending_slot != (uint32_t)IAP_SLOT_NONE ||
        persisted.boot_attempts != 0u) {
        report_error(IAP_ERR_BOOT_CONTROL, 64u);
        return;
    }

    g_stm32_iap_test_control.command = STM32_IAP_CMD_NONE;
    stm32_iap_test_set_status(STM32_IAP_STATUS_WRITE_FAILURE_PASSED,
                              (uint32_t)target_slot);
}

static void activate_other_slot(iap_context_t *ctx)
{
    iap_boot_control_t control;
    iap_slot_t target_slot = other_slot();
    iap_status_t status;

    status = iap_boot_control_load(ctx, &control);
    if (status != IAP_OK) {
        report_error(status, 70u);
        return;
    }
    if (control.active_slot != (uint32_t)current_slot ||
        control.pending_slot != (uint32_t)IAP_SLOT_NONE) {
        report_error(IAP_ERR_BOOT_CONTROL, 71u);
        return;
    }

    status = iap_mark_pending(ctx, &control, target_slot);
    if (status != IAP_OK) {
        report_error(status, 72u);
        return;
    }

    g_stm32_iap_test_control.command = STM32_IAP_CMD_CONFIRM_CURRENT;
    stm32_iap_test_set_status(STM32_IAP_STATUS_SWITCHING_SLOT,
                              (uint32_t)target_slot);
    __DMB();
    stm32_iap_system_reset();
}

static void test_non_pending_confirm(iap_context_t *ctx)
{
    iap_boot_control_t control;
    iap_boot_control_t persisted;
    iap_slot_t target_slot = other_slot();
    iap_status_t status;

    status = iap_boot_control_load(ctx, &control);
    if (status != IAP_OK) {
        report_error(status, 80u);
        return;
    }
    if (control.active_slot != (uint32_t)current_slot ||
        control.pending_slot != (uint32_t)IAP_SLOT_NONE) {
        report_error(IAP_ERR_BOOT_CONTROL, 81u);
        return;
    }

    status = iap_confirm_current_image(ctx, &control, target_slot);
    if (status != IAP_ERR_INVALID_ARG) {
        report_unexpected_status(status, 82u);
        return;
    }

    status = iap_boot_control_load(ctx, &persisted);
    if (status != IAP_OK) {
        report_error(status, 83u);
        return;
    }
    if (persisted.active_slot != (uint32_t)current_slot ||
        persisted.pending_slot != (uint32_t)IAP_SLOT_NONE) {
        report_error(IAP_ERR_BOOT_CONTROL, 84u);
        return;
    }

    g_stm32_iap_test_control.command = STM32_IAP_CMD_NONE;
    stm32_iap_test_set_status(STM32_IAP_STATUS_NON_PENDING_CONFIRM_PASSED,
                              (uint32_t)target_slot);
}

static void test_boundary_guards(iap_context_t *ctx)
{
    uint32_t word = 0xA5A5A5A5u;
    iap_slot_t target_slot = other_slot();
    const iap_slot_desc_t *current = &ctx->slots[(uint32_t)current_slot];
    const iap_slot_desc_t *target = &ctx->slots[(uint32_t)target_slot];
    iap_status_t status;

    status = ctx->storage.erase(ctx->storage.user, current->address, current->size);
    if (status != IAP_ERR_INVALID_ARG) {
        report_unexpected_status(status, 90u);
        return;
    }
    status = ctx->storage.write(ctx->storage.user,
                                current->address,
                                &word,
                                (uint32_t)sizeof(word));
    if (status != IAP_ERR_INVALID_ARG) {
        report_unexpected_status(status, 91u);
        return;
    }
    status = ctx->storage.write(ctx->storage.user,
                                STM32_IAP_FLASH_BASE,
                                &word,
                                (uint32_t)sizeof(word));
    if (status != IAP_ERR_INVALID_ARG) {
        report_unexpected_status(status, 92u);
        return;
    }
    status = ctx->storage.write(ctx->storage.user,
                                target->address + 1u,
                                &word,
                                (uint32_t)sizeof(word));
    if (status != IAP_ERR_INVALID_ARG) {
        report_unexpected_status(status, 93u);
        return;
    }
    status = ctx->storage.erase(ctx->storage.user,
                                target->address + (uint32_t)sizeof(word),
                                target->size);
    if (status != IAP_ERR_INVALID_ARG) {
        report_unexpected_status(status, 94u);
        return;
    }
    status = ctx->storage.read(ctx->storage.user,
                               STM32_IAP_FLASH_END - 2u,
                               &word,
                               (uint32_t)sizeof(word));
    if (status != IAP_ERR_INVALID_ARG) {
        report_unexpected_status(status, 95u);
        return;
    }

    g_stm32_iap_test_control.command = STM32_IAP_CMD_NONE;
    stm32_iap_test_set_status(STM32_IAP_STATUS_BOUNDARY_GUARDS_PASSED,
                              (uint32_t)current_slot);
}

static iap_status_t configure_power_install_pause(stm32_iap_storage_t *storage,
                                                  uint32_t phase)
{
    const iap_image_header_t *header =
        (const iap_image_header_t *)(uintptr_t)STM32_IAP_STAGING_ADDRESS;
    uint32_t bytes_written;

    switch (phase) {
    case STM32_IAP_POWER_PHASE_ERASE:
        stm32_iap_storage_pause_after_first_erase(storage, phase);
        return IAP_OK;
    case STM32_IAP_POWER_PHASE_HEADER:
        stm32_iap_storage_pause_during_program(storage, 8u, phase);
        return IAP_OK;
    case STM32_IAP_POWER_PHASE_PAYLOAD_25:
        bytes_written = header->header_size +
                        (uint32_t)(((uint64_t)header->image_size * 25u) / 100u);
        stm32_iap_storage_pause_during_program(storage, bytes_written, phase);
        return IAP_OK;
    case STM32_IAP_POWER_PHASE_PAYLOAD_50:
        bytes_written = header->header_size +
                        (uint32_t)(((uint64_t)header->image_size * 50u) / 100u);
        stm32_iap_storage_pause_during_program(storage, bytes_written, phase);
        return IAP_OK;
    case STM32_IAP_POWER_PHASE_PAYLOAD_90:
        bytes_written = header->header_size +
                        (uint32_t)(((uint64_t)header->image_size * 90u) / 100u);
        stm32_iap_storage_pause_during_program(storage, bytes_written, phase);
        return IAP_OK;
    case STM32_IAP_POWER_PHASE_BEFORE_PENDING:
    case STM32_IAP_POWER_PHASE_PENDING_CONTROL:
    case STM32_IAP_POWER_PHASE_FIRST_BOOT:
        return IAP_OK;
    default:
        return IAP_ERR_INVALID_ARG;
    }
}

static void run_power_install(iap_context_t *ctx,
                              stm32_iap_storage_t *storage)
{
    iap_boot_control_t control;
    uint32_t image_size = g_stm32_iap_test_control.argument0;
    uint32_t phase = g_stm32_iap_test_control.argument1;
    iap_status_t status;

    if (current_slot != IAP_SLOT_A || image_size == 0u ||
        image_size > STM32_IAP_STAGING_SIZE) {
        report_error(IAP_ERR_INVALID_ARG, 100u);
        return;
    }

    status = configure_power_install_pause(storage, phase);
    if (status != IAP_OK) {
        report_error(status, 101u);
        return;
    }

    stm32_iap_test_set_status(STM32_IAP_STATUS_INSTALLING_SLOT_B, phase);
    status = iap_write_candidate_image(ctx,
                                       IAP_SLOT_B,
                                       (const void *)(uintptr_t)STM32_IAP_STAGING_ADDRESS,
                                       image_size);
    if (status != IAP_OK) {
        report_error(status, 102u);
        return;
    }

    if (phase == STM32_IAP_POWER_PHASE_BEFORE_PENDING) {
        stm32_iap_power_cut_pause(phase);
    }

    status = iap_boot_control_load(ctx, &control);
    if (status != IAP_OK) {
        report_error(status, 103u);
        return;
    }

    if (phase == STM32_IAP_POWER_PHASE_PENDING_CONTROL) {
        stm32_iap_storage_pause_before_control_commit(storage, phase);
    }

    status = iap_mark_pending(ctx, &control, IAP_SLOT_B);
    if (status != IAP_OK) {
        report_error(status, 104u);
        return;
    }

    g_stm32_iap_test_control.command =
        phase == STM32_IAP_POWER_PHASE_FIRST_BOOT
            ? STM32_IAP_CMD_POWER_PAUSE_ON_BOOT
            : STM32_IAP_CMD_HOLD_PENDING;
    __DMB();
    stm32_iap_system_reset();
}

static void run_power_confirm(iap_context_t *ctx,
                              stm32_iap_storage_t *storage)
{
    iap_boot_control_t control;
    iap_status_t status = iap_boot_control_load(ctx, &control);

    if (status != IAP_OK) {
        report_error(status, 110u);
        return;
    }
    if (control.pending_slot != (uint32_t)current_slot) {
        report_error(IAP_ERR_INVALID_ARG, 111u);
        return;
    }

    stm32_iap_storage_pause_before_control_commit(
        storage,
        STM32_IAP_POWER_PHASE_CONFIRM_CONTROL);
    status = iap_confirm_current_image(ctx, &control, current_slot);
    report_unexpected_status(status, 112u);
}

int main(void)
{
    iap_context_t ctx;
    stm32_iap_storage_t storage;
    iap_status_t status;
    int feed_watchdog = 1;
    uint32_t boot_address = current_slot == IAP_SLOT_A
                                ? STM32_IAP_SLOT_A_ADDRESS + STM32_IAP_IMAGE_HEADER_SIZE
                                : STM32_IAP_SLOT_B_ADDRESS + STM32_IAP_IMAGE_HEADER_SIZE;

    stm32_iap_platform_init();
    SCB->VTOR = boot_address;
    __DSB();
    __ISB();

    stm32_iap_test_control_init();
    status = stm32_iap_context_init(&ctx, &storage, current_slot);
    if (status != IAP_OK) {
        report_error(status, 30u);
        for (;;) {
            stm32_iap_watchdog_feed();
        }
    }

    stm32_iap_test_set_status(current_slot == IAP_SLOT_A
                                  ? STM32_IAP_STATUS_APP_A_RUNNING
                                  : STM32_IAP_STATUS_APP_B_RUNNING,
                              boot_address);

    for (;;) {
        uint32_t command = g_stm32_iap_test_control.command;

        if (feed_watchdog) {
            stm32_iap_watchdog_feed();
        }

        switch (command) {
        case STM32_IAP_CMD_NONE:
            break;
        case STM32_IAP_CMD_INSTALL_SLOT_B:
            install_slot_b(&ctx);
            break;
        case STM32_IAP_CMD_HOLD_PENDING:
            report_pending_state(&ctx);
            break;
        case STM32_IAP_CMD_CONFIRM_CURRENT:
            confirm_current(&ctx);
            break;
        case STM32_IAP_CMD_FAIL_CURRENT:
            handle_failure_command(&ctx, &feed_watchdog);
            break;
        case STM32_IAP_CMD_CLEAR_STATUS:
            g_stm32_iap_test_control.command = STM32_IAP_CMD_NONE;
            stm32_iap_test_set_status(current_slot == IAP_SLOT_A
                                          ? STM32_IAP_STATUS_APP_A_RUNNING
                                          : STM32_IAP_STATUS_APP_B_RUNNING,
                                      boot_address);
            break;
        case STM32_IAP_CMD_TEST_READ_FAILURE:
            test_storage_read_failure(&ctx, &storage);
            break;
        case STM32_IAP_CMD_TEST_WRITE_FAILURE:
            test_control_write_failure(&ctx, &storage);
            break;
        case STM32_IAP_CMD_ACTIVATE_OTHER:
            activate_other_slot(&ctx);
            break;
        case STM32_IAP_CMD_TEST_NON_PENDING_CONFIRM:
            test_non_pending_confirm(&ctx);
            break;
        case STM32_IAP_CMD_TEST_BOUNDARIES:
            test_boundary_guards(&ctx);
            break;
        case STM32_IAP_CMD_POWER_INSTALL:
            run_power_install(&ctx, &storage);
            break;
        case STM32_IAP_CMD_POWER_CONFIRM:
            run_power_confirm(&ctx, &storage);
            break;
        case STM32_IAP_CMD_POWER_PAUSE_ON_BOOT:
            stm32_iap_power_cut_pause(STM32_IAP_POWER_PHASE_FIRST_BOOT);
            break;
        default:
            report_error(IAP_ERR_INVALID_ARG, 31u);
            break;
        }
    }
}
