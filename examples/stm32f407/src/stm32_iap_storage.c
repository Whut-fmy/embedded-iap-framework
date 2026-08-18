#include "stm32_iap_storage.h"

#include "stm32_iap_config.h"
#include "stm32_iap_platform.h"
#include "stm32f4xx_hal.h"

#include <stddef.h>
#include <string.h>

#define STM32_IAP_RECORD_MAGIC 0x4941504Au
#define STM32_IAP_RECORD_COMMIT 0x434F4D54u

typedef struct {
    uint32_t magic;
    uint32_t sequence;
    iap_boot_control_t control;
    uint32_t control_crc32;
    uint32_t commit;
} stm32_iap_control_record_t;

typedef char stm32_iap_record_size_must_be_64[
    sizeof(stm32_iap_control_record_t) == 64u ? 1 : -1];

typedef struct {
    const stm32_iap_control_record_t *record;
    uint32_t address;
} stm32_iap_latest_record_t;

static int range_is_within(uint32_t address,
                           uint32_t size,
                           uint32_t region_address,
                           uint32_t region_size)
{
    uint64_t end = (uint64_t)address + size;
    uint64_t region_end = (uint64_t)region_address + region_size;

    return size > 0u && address >= region_address && end <= region_end;
}

static int range_is_slot(uint32_t address, uint32_t size, iap_slot_t *slot)
{
    if (range_is_within(address, size, STM32_IAP_SLOT_A_ADDRESS, STM32_IAP_SLOT_SIZE)) {
        if (slot != NULL) {
            *slot = IAP_SLOT_A;
        }
        return 1;
    }

    if (range_is_within(address, size, STM32_IAP_SLOT_B_ADDRESS, STM32_IAP_SLOT_SIZE)) {
        if (slot != NULL) {
            *slot = IAP_SLOT_B;
        }
        return 1;
    }

    return 0;
}

static int sequence_is_newer(uint32_t candidate, uint32_t current)
{
    return (int32_t)(candidate - current) > 0;
}

static int record_is_erased(const stm32_iap_control_record_t *record)
{
    const uint32_t *words = (const uint32_t *)record;
    uint32_t index;

    for (index = 0u; index < sizeof(*record) / sizeof(words[0]); index++) {
        if (words[index] != UINT32_MAX) {
            return 0;
        }
    }
    return 1;
}

static int record_is_valid(const stm32_iap_control_record_t *record)
{
    uint32_t crc;

    if (record->magic != STM32_IAP_RECORD_MAGIC ||
        record->commit != STM32_IAP_RECORD_COMMIT) {
        return 0;
    }

    crc = iap_crc32(&record->sequence,
                    (uint32_t)(sizeof(record->sequence) + sizeof(record->control)));
    return crc == record->control_crc32;
}

static void scan_control_sector(uint32_t sector_address,
                                stm32_iap_latest_record_t *latest)
{
    uint32_t offset;

    for (offset = 0u;
         offset + sizeof(stm32_iap_control_record_t) <= STM32_IAP_CONTROL_SECTOR_SIZE;
         offset += (uint32_t)sizeof(stm32_iap_control_record_t)) {
        const stm32_iap_control_record_t *record =
            (const stm32_iap_control_record_t *)(uintptr_t)(sector_address + offset);

        if (record_is_erased(record)) {
            break;
        }

        if (record_is_valid(record) &&
            (latest->record == NULL ||
             sequence_is_newer(record->sequence, latest->record->sequence))) {
            latest->record = record;
            latest->address = sector_address + offset;
        }
    }
}

static stm32_iap_latest_record_t find_latest_control_record(void)
{
    stm32_iap_latest_record_t latest;

    latest.record = NULL;
    latest.address = 0u;
    scan_control_sector(STM32_IAP_CONTROL_A_ADDRESS, &latest);
    scan_control_sector(STM32_IAP_CONTROL_B_ADDRESS, &latest);
    return latest;
}

static uint32_t find_free_record_address(uint32_t sector_address)
{
    uint32_t offset;

    for (offset = 0u;
         offset + sizeof(stm32_iap_control_record_t) <= STM32_IAP_CONTROL_SECTOR_SIZE;
         offset += (uint32_t)sizeof(stm32_iap_control_record_t)) {
        const stm32_iap_control_record_t *record =
            (const stm32_iap_control_record_t *)(uintptr_t)(sector_address + offset);
        if (record_is_erased(record)) {
            return sector_address + offset;
        }
    }

    return 0u;
}

static void clear_flash_errors(void)
{
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
}

static iap_status_t erase_sector(uint32_t sector)
{
    FLASH_EraseInitTypeDef erase;
    uint32_t sector_error = UINT32_MAX;
    HAL_StatusTypeDef status;

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Banks = FLASH_BANK_1;
    erase.Sector = sector;
    erase.NbSectors = 1u;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return IAP_ERR_STORAGE;
    }
    clear_flash_errors();
    status = HAL_FLASHEx_Erase(&erase, &sector_error);
    (void)HAL_FLASH_Lock();

    return status == HAL_OK ? IAP_OK : IAP_ERR_STORAGE;
}

static void pause_during_program_if_requested(stm32_iap_storage_t *storage,
                                              uint32_t bytes_written)
{
    if (storage != NULL && storage->program_pause_after_bytes > 0u &&
        bytes_written >= storage->program_pause_after_bytes) {
        uint32_t phase = storage->pause_phase;

        storage->program_pause_after_bytes = 0u;
        (void)HAL_FLASH_Lock();
        stm32_iap_power_cut_pause(phase);
    }
}

static iap_status_t program_words(stm32_iap_storage_t *storage,
                                  uint32_t address,
                                  const void *data,
                                  uint32_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t offset = 0u;

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return IAP_ERR_STORAGE;
    }
    clear_flash_errors();

    while (offset + sizeof(uint32_t) <= size) {
        uint32_t word;
        memcpy(&word, &bytes[offset], sizeof(word));
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address + offset, word) != HAL_OK) {
            (void)HAL_FLASH_Lock();
            return IAP_ERR_STORAGE;
        }
        offset += (uint32_t)sizeof(word);
        pause_during_program_if_requested(storage, offset);
    }

    while (offset < size) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE,
                              address + offset,
                              bytes[offset]) != HAL_OK) {
            (void)HAL_FLASH_Lock();
            return IAP_ERR_STORAGE;
        }
        offset++;
        pause_during_program_if_requested(storage, offset);
    }

    (void)HAL_FLASH_Lock();
    return memcmp((const void *)(uintptr_t)address, data, size) == 0
               ? IAP_OK
               : IAP_ERR_STORAGE;
}

static iap_status_t append_control_record(stm32_iap_storage_t *storage,
                                          const iap_boot_control_t *control)
{
    stm32_iap_latest_record_t latest = find_latest_control_record();
    stm32_iap_control_record_t record;
    uint32_t sector_address;
    uint32_t record_address;
    uint32_t commit_offset = (uint32_t)offsetof(stm32_iap_control_record_t, commit);
    iap_status_t status;

    if (latest.record == NULL) {
        sector_address = STM32_IAP_CONTROL_A_ADDRESS;
        record_address = find_free_record_address(sector_address);
        if (record_address == 0u) {
            status = erase_sector(FLASH_SECTOR_9);
            if (status != IAP_OK) {
                return status;
            }
            record_address = sector_address;
        }
        record.sequence = 0u;
    } else {
        sector_address = latest.address >= STM32_IAP_CONTROL_B_ADDRESS
                             ? STM32_IAP_CONTROL_B_ADDRESS
                             : STM32_IAP_CONTROL_A_ADDRESS;
        record_address = find_free_record_address(sector_address);
        record.sequence = latest.record->sequence + 1u;

        if (record_address == 0u) {
            if (sector_address == STM32_IAP_CONTROL_A_ADDRESS) {
                sector_address = STM32_IAP_CONTROL_B_ADDRESS;
                status = erase_sector(FLASH_SECTOR_10);
            } else {
                sector_address = STM32_IAP_CONTROL_A_ADDRESS;
                status = erase_sector(FLASH_SECTOR_9);
            }
            if (status != IAP_OK) {
                return status;
            }
            record_address = sector_address;
        }
    }

    record.magic = STM32_IAP_RECORD_MAGIC;
    record.control = *control;
    record.control_crc32 = iap_crc32(&record.sequence,
                                     (uint32_t)(sizeof(record.sequence) +
                                                sizeof(record.control)));
    record.commit = STM32_IAP_RECORD_COMMIT;

    status = program_words(storage, record_address, &record, commit_offset);
    if (status != IAP_OK) {
        return status;
    }

    if (storage->pause_before_control_commit != 0u) {
        uint32_t phase = storage->pause_phase;

        storage->pause_before_control_commit = 0u;
        stm32_iap_power_cut_pause(phase);
    }

    status = program_words(storage,
                           record_address + commit_offset,
                           &record.commit,
                           (uint32_t)sizeof(record.commit));
    if (status != IAP_OK) {
        return status;
    }

    return record_is_valid((const stm32_iap_control_record_t *)(uintptr_t)record_address)
               ? IAP_OK
               : IAP_ERR_STORAGE;
}

static iap_status_t flash_read(void *user,
                               uint32_t address,
                               void *buffer,
                               uint32_t size)
{
    stm32_iap_storage_t *storage = (stm32_iap_storage_t *)user;
    stm32_iap_latest_record_t latest;

    if (storage == NULL || buffer == NULL || size == 0u) {
        return IAP_ERR_INVALID_ARG;
    }

    if (storage->read_failures_remaining > 0u) {
        storage->read_failures_remaining--;
        return IAP_ERR_STORAGE;
    }

    if (address == STM32_IAP_CONTROL_A_ADDRESS && size == sizeof(iap_boot_control_t)) {
        latest = find_latest_control_record();
        if (latest.record == NULL) {
            memset(buffer, 0xFF, size);
        } else {
            memcpy(buffer, &latest.record->control, size);
        }
        return IAP_OK;
    }

    if (!range_is_within(address,
                         size,
                         STM32_IAP_FLASH_BASE,
                         STM32_IAP_FLASH_END - STM32_IAP_FLASH_BASE)) {
        return IAP_ERR_INVALID_ARG;
    }

    memcpy(buffer, (const void *)(uintptr_t)address, size);
    return IAP_OK;
}

static iap_status_t flash_write(void *user,
                                uint32_t address,
                                const void *buffer,
                                uint32_t size)
{
    stm32_iap_storage_t *storage = (stm32_iap_storage_t *)user;
    iap_slot_t target_slot;

    if (storage == NULL || buffer == NULL || size == 0u) {
        return IAP_ERR_INVALID_ARG;
    }

    if (address == STM32_IAP_CONTROL_A_ADDRESS && size == sizeof(iap_boot_control_t)) {
        if (storage->write_failures_remaining > 0u) {
            storage->write_failures_remaining--;
            return IAP_ERR_STORAGE;
        }
        return append_control_record(storage, (const iap_boot_control_t *)buffer);
    }

    if ((address % (uint32_t)sizeof(uint32_t)) != 0u ||
        !range_is_slot(address, size, &target_slot) ||
        target_slot == storage->protected_slot) {
        return IAP_ERR_INVALID_ARG;
    }

    return program_words(storage, address, buffer, size);
}

static iap_status_t flash_erase(void *user, uint32_t address, uint32_t size)
{
    stm32_iap_storage_t *storage = (stm32_iap_storage_t *)user;
    FLASH_EraseInitTypeDef erase;
    uint32_t sector_error = UINT32_MAX;
    iap_slot_t target_slot;
    HAL_StatusTypeDef status;

    if (storage == NULL ||
        size != STM32_IAP_SLOT_SIZE ||
        !range_is_slot(address, size, &target_slot) ||
        target_slot == storage->protected_slot) {
        return IAP_ERR_INVALID_ARG;
    }

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Banks = FLASH_BANK_1;
    erase.Sector = target_slot == IAP_SLOT_A ? FLASH_SECTOR_5 : FLASH_SECTOR_7;
    erase.NbSectors = storage->pause_after_first_erase != 0u ? 1u : 2u;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return IAP_ERR_STORAGE;
    }
    clear_flash_errors();
    status = HAL_FLASHEx_Erase(&erase, &sector_error);
    (void)HAL_FLASH_Lock();

    if (status == HAL_OK && storage->pause_after_first_erase != 0u) {
        uint32_t phase = storage->pause_phase;

        storage->pause_after_first_erase = 0u;
        stm32_iap_power_cut_pause(phase);
    }

    return status == HAL_OK ? IAP_OK : IAP_ERR_STORAGE;
}

void stm32_iap_storage_init(stm32_iap_storage_t *storage, iap_slot_t protected_slot)
{
    if (storage != NULL) {
        storage->protected_slot = protected_slot;
        storage->read_failures_remaining = 0u;
        storage->write_failures_remaining = 0u;
        storage->pause_after_first_erase = 0u;
        storage->program_pause_after_bytes = 0u;
        storage->pause_before_control_commit = 0u;
        storage->pause_phase = 0u;
    }
}

void stm32_iap_storage_fail_next_read(stm32_iap_storage_t *storage)
{
    if (storage != NULL) {
        storage->read_failures_remaining = 1u;
    }
}

void stm32_iap_storage_fail_next_write(stm32_iap_storage_t *storage)
{
    if (storage != NULL) {
        storage->write_failures_remaining = 1u;
    }
}

void stm32_iap_storage_pause_after_first_erase(stm32_iap_storage_t *storage,
                                               uint32_t phase)
{
    if (storage != NULL) {
        storage->pause_after_first_erase = 1u;
        storage->pause_phase = phase;
    }
}

void stm32_iap_storage_pause_during_program(stm32_iap_storage_t *storage,
                                            uint32_t bytes_written,
                                            uint32_t phase)
{
    if (storage != NULL && bytes_written > 0u) {
        storage->program_pause_after_bytes = bytes_written;
        storage->pause_phase = phase;
    }
}

void stm32_iap_storage_pause_before_control_commit(stm32_iap_storage_t *storage,
                                                   uint32_t phase)
{
    if (storage != NULL) {
        storage->pause_before_control_commit = 1u;
        storage->pause_phase = phase;
    }
}

iap_storage_ops_t stm32_iap_storage_ops(stm32_iap_storage_t *storage)
{
    iap_storage_ops_t ops;

    ops.read = flash_read;
    ops.write = flash_write;
    ops.erase = flash_erase;
    ops.user = storage;
    return ops;
}

iap_status_t stm32_iap_context_init(iap_context_t *ctx,
                                    stm32_iap_storage_t *storage,
                                    iap_slot_t protected_slot)
{
    iap_storage_ops_t ops;
    iap_slot_desc_t slots[IAP_SLOT_COUNT];

    if (ctx == NULL || storage == NULL) {
        return IAP_ERR_INVALID_ARG;
    }

    stm32_iap_storage_init(storage, protected_slot);
    ops = stm32_iap_storage_ops(storage);
    stm32_iap_slot_descriptors(slots);
    return iap_init(ctx,
                    &ops,
                    slots,
                    STM32_IAP_CONTROL_A_ADDRESS,
                    STM32_IAP_MAX_BOOT_ATTEMPTS);
}
