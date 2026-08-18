#ifndef STM32_IAP_STORAGE_H
#define STM32_IAP_STORAGE_H

#include "iap/iap.h"

typedef struct {
    iap_slot_t protected_slot;
    uint32_t read_failures_remaining;
    uint32_t write_failures_remaining;
    uint32_t pause_after_first_erase;
    uint32_t program_pause_after_bytes;
    uint32_t pause_before_control_commit;
    uint32_t pause_phase;
} stm32_iap_storage_t;

void stm32_iap_storage_init(stm32_iap_storage_t *storage, iap_slot_t protected_slot);
iap_storage_ops_t stm32_iap_storage_ops(stm32_iap_storage_t *storage);
iap_status_t stm32_iap_context_init(iap_context_t *ctx,
                                    stm32_iap_storage_t *storage,
                                    iap_slot_t protected_slot);
void stm32_iap_storage_fail_next_read(stm32_iap_storage_t *storage);
void stm32_iap_storage_fail_next_write(stm32_iap_storage_t *storage);
void stm32_iap_storage_pause_after_first_erase(stm32_iap_storage_t *storage,
                                               uint32_t phase);
void stm32_iap_storage_pause_during_program(stm32_iap_storage_t *storage,
                                            uint32_t bytes_written,
                                            uint32_t phase);
void stm32_iap_storage_pause_before_control_commit(stm32_iap_storage_t *storage,
                                                   uint32_t phase);

#endif
