#include "iap/iap_bootloader.h"

iap_status_t iap_bootloader_run(const iap_context_t *ctx,
                                const iap_bootloader_ops_t *ops,
                                iap_slot_t default_active_slot)
{
    iap_boot_control_t control;
    iap_status_t status;
    iap_slot_t selected_slot;
    uint32_t boot_address;

    if (ctx == NULL || ops == NULL || ops->jump_to_slot == NULL || ops->enter_recovery == NULL) {
        return IAP_ERR_INVALID_ARG;
    }

    status = iap_boot_control_load(ctx, &control);
    if (status == IAP_ERR_BOOT_CONTROL) {
        control = iap_default_boot_control(default_active_slot, ctx->max_boot_attempts);
        status = iap_boot_control_save(ctx, &control);
    }

    if (status != IAP_OK) {
        ops->enter_recovery(ops->user, status);
        return status;
    }

    status = iap_select_boot_slot(ctx, &control, &selected_slot);
    if (status != IAP_OK) {
        ops->enter_recovery(ops->user, status);
        return status;
    }

    status = iap_get_image_boot_address(ctx, selected_slot, &boot_address);
    if (status != IAP_OK) {
        ops->enter_recovery(ops->user, status);
        return status;
    }

    if (ops->watchdog_start != 0) {
        ops->watchdog_start(ops->user);
    }

    ops->jump_to_slot(ops->user,
                      selected_slot,
                      boot_address);
    return IAP_OK;
}
