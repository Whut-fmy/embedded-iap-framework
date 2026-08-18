#include "iap/iap_memory_storage.h"

#include <string.h>

static iap_status_t memory_read(void *user, uint32_t address, void *buffer, uint32_t size)
{
    iap_memory_storage_t *storage = (iap_memory_storage_t *)user;

    if (storage == NULL || storage->memory == NULL || buffer == NULL || address > storage->size ||
        size > (storage->size - address)) {
        return IAP_ERR_INVALID_ARG;
    }

    memcpy(buffer, &storage->memory[address], size);
    return IAP_OK;
}

static iap_status_t memory_write(void *user, uint32_t address, const void *buffer, uint32_t size)
{
    iap_memory_storage_t *storage = (iap_memory_storage_t *)user;

    if (storage == NULL || storage->memory == NULL || buffer == NULL || address > storage->size ||
        size > (storage->size - address)) {
        return IAP_ERR_INVALID_ARG;
    }

    memcpy(&storage->memory[address], buffer, size);
    return IAP_OK;
}

static iap_status_t memory_erase(void *user, uint32_t address, uint32_t size)
{
    iap_memory_storage_t *storage = (iap_memory_storage_t *)user;

    if (storage == NULL || storage->memory == NULL || address > storage->size ||
        size > (storage->size - address)) {
        return IAP_ERR_INVALID_ARG;
    }

    memset(&storage->memory[address], storage->erased_value, size);
    return IAP_OK;
}

void iap_memory_storage_init(iap_memory_storage_t *storage,
                             uint8_t *memory,
                             uint32_t size,
                             uint8_t erased_value)
{
    if (storage == NULL) {
        return;
    }

    storage->memory = memory;
    storage->size = size;
    storage->erased_value = erased_value;

    if (memory != NULL) {
        memset(memory, erased_value, size);
    }
}

iap_storage_ops_t iap_memory_storage_ops(iap_memory_storage_t *storage)
{
    iap_storage_ops_t ops;

    ops.read = memory_read;
    ops.write = memory_write;
    ops.erase = memory_erase;
    ops.user = storage;

    return ops;
}
