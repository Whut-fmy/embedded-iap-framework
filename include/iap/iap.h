#ifndef IAP_IAP_H
#define IAP_IAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IAP_SLOT_COUNT 2u
#define IAP_BOOT_CONTROL_MAGIC 0x49415042u
#define IAP_IMAGE_MAGIC 0x49415049u
#define IAP_BOOT_VERSION 1u
#define IAP_IMAGE_HEADER_VERSION 1u
#define IAP_DEFAULT_MAX_BOOT_ATTEMPTS 3u
#define IAP_CRC32_INITIAL 0xFFFFFFFFu

typedef enum {
    IAP_SLOT_A = 0,
    IAP_SLOT_B = 1,
    IAP_SLOT_NONE = 0xFF
} iap_slot_t;

typedef enum {
    IAP_IMAGE_EMPTY = 0,
    IAP_IMAGE_VALID,
    IAP_IMAGE_PENDING,
    IAP_IMAGE_CONFIRMED,
    IAP_IMAGE_BAD
} iap_image_state_t;

typedef enum {
    IAP_OK = 0,
    IAP_ERR_INVALID_ARG,
    IAP_ERR_STORAGE,
    IAP_ERR_VERIFY,
    IAP_ERR_BOOT_CONTROL,
    IAP_ERR_NO_VALID_IMAGE
} iap_status_t;

typedef struct {
    uint32_t magic;
    uint32_t header_size;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t version;
    uint32_t flags;
} iap_image_header_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t active_slot;
    uint32_t pending_slot;
    uint32_t boot_attempts;
    uint32_t max_boot_attempts;
    uint32_t image_crc32[IAP_SLOT_COUNT];
    uint32_t image_size[IAP_SLOT_COUNT];
    uint32_t confirmed[IAP_SLOT_COUNT];
} iap_boot_control_t;

typedef struct {
    uint32_t address;
    uint32_t size;
} iap_slot_desc_t;

typedef iap_status_t (*iap_storage_read_fn)(void *user, uint32_t address, void *buffer, uint32_t size);
typedef iap_status_t (*iap_storage_write_fn)(void *user, uint32_t address, const void *buffer, uint32_t size);
typedef iap_status_t (*iap_storage_erase_fn)(void *user, uint32_t address, uint32_t size);

typedef struct {
    iap_storage_read_fn read;
    iap_storage_write_fn write;
    iap_storage_erase_fn erase;
    void *user;
} iap_storage_ops_t;

typedef struct {
    iap_storage_ops_t storage;
    iap_slot_desc_t slots[IAP_SLOT_COUNT];
    uint32_t boot_control_address;
    uint32_t max_boot_attempts;
} iap_context_t;

iap_status_t iap_init(iap_context_t *ctx,
                      const iap_storage_ops_t *storage,
                      const iap_slot_desc_t slots[IAP_SLOT_COUNT],
                      uint32_t boot_control_address,
                      uint32_t max_boot_attempts);

iap_boot_control_t iap_default_boot_control(iap_slot_t active_slot, uint32_t max_boot_attempts);

iap_status_t iap_boot_control_load(const iap_context_t *ctx, iap_boot_control_t *control);
iap_status_t iap_boot_control_save(const iap_context_t *ctx, const iap_boot_control_t *control);

iap_status_t iap_verify_image(const iap_context_t *ctx, iap_slot_t slot);
iap_status_t iap_get_image_boot_address(const iap_context_t *ctx,
                                        iap_slot_t slot,
                                        uint32_t *boot_address);
iap_status_t iap_select_boot_slot(const iap_context_t *ctx,
                                  iap_boot_control_t *control,
                                  iap_slot_t *selected_slot);
iap_status_t iap_mark_pending(const iap_context_t *ctx, iap_boot_control_t *control, iap_slot_t new_slot);
iap_status_t iap_confirm_current_image(const iap_context_t *ctx,
                                       iap_boot_control_t *control,
                                       iap_slot_t current_slot);
iap_status_t iap_rollback(const iap_context_t *ctx, iap_boot_control_t *control);
iap_status_t iap_write_candidate_image(const iap_context_t *ctx,
                                       iap_slot_t target_slot,
                                       const void *image,
                                       uint32_t size);

uint32_t iap_crc32_update(uint32_t crc, const void *data, uint32_t size);
uint32_t iap_crc32(const void *data, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif
