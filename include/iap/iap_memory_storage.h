#ifndef IAP_MEMORY_STORAGE_H
#define IAP_MEMORY_STORAGE_H

#include "iap/iap.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *memory;
    uint32_t size;
    uint8_t erased_value;
} iap_memory_storage_t;

void iap_memory_storage_init(iap_memory_storage_t *storage,
                             uint8_t *memory,
                             uint32_t size,
                             uint8_t erased_value);

iap_storage_ops_t iap_memory_storage_ops(iap_memory_storage_t *storage);

#ifdef __cplusplus
}
#endif

#endif
