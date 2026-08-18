#ifndef IAP_BOOTLOADER_H
#define IAP_BOOTLOADER_H

#include "iap/iap.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*iap_bootloader_watchdog_start_fn)(void *user);
typedef void (*iap_bootloader_jump_fn)(void *user, iap_slot_t slot, uint32_t address);
typedef void (*iap_bootloader_recovery_fn)(void *user, iap_status_t reason);

typedef struct {
    iap_bootloader_watchdog_start_fn watchdog_start;
    iap_bootloader_jump_fn jump_to_slot;
    iap_bootloader_recovery_fn enter_recovery;
    void *user;
} iap_bootloader_ops_t;

iap_status_t iap_bootloader_run(const iap_context_t *ctx,
                                const iap_bootloader_ops_t *ops,
                                iap_slot_t default_active_slot);

#ifdef __cplusplus
}
#endif

#endif
