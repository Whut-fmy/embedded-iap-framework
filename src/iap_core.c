#include "iap/iap.h"

#include <string.h>

#define IAP_CRC32_POLY 0xEDB88320u

static int iap_is_valid_slot(iap_slot_t slot)
{
    return slot == IAP_SLOT_A || slot == IAP_SLOT_B;
}

static iap_slot_t iap_other_slot(iap_slot_t slot)
{
    return slot == IAP_SLOT_A ? IAP_SLOT_B : IAP_SLOT_A;
}

static int iap_range_is_valid(uint32_t address, uint32_t size)
{
    return size > 0u && size <= (UINT32_MAX - address);
}

static int iap_ranges_overlap(uint32_t first_address,
                              uint32_t first_size,
                              uint32_t second_address,
                              uint32_t second_size)
{
    uint32_t first_end = first_address + first_size;
    uint32_t second_end = second_address + second_size;

    return first_address < second_end && second_address < first_end;
}

static int iap_image_header_is_valid(const iap_slot_desc_t *desc,
                                     const iap_image_header_t *header)
{
    return desc != NULL && header != NULL &&
           desc->size > (uint32_t)sizeof(*header) &&
           header->magic == IAP_IMAGE_MAGIC &&
           header->header_size >= (uint32_t)sizeof(*header) &&
           (header->header_size % (uint32_t)sizeof(uint32_t)) == 0u &&
           header->header_size < desc->size &&
           header->version == IAP_IMAGE_HEADER_VERSION &&
           header->image_size > 0u &&
           header->image_size <= (desc->size - header->header_size);
}

static iap_status_t iap_read_header(const iap_context_t *ctx,
                                    iap_slot_t slot,
                                    iap_image_header_t *header)
{
    if (ctx == NULL || header == NULL || !iap_is_valid_slot(slot)) {
        return IAP_ERR_INVALID_ARG;
    }

    return ctx->storage.read(ctx->storage.user,
                             ctx->slots[(uint32_t)slot].address,
                             header,
                             (uint32_t)sizeof(*header));
}

static int iap_boot_control_is_valid(const iap_boot_control_t *control)
{
    iap_slot_t active;
    iap_slot_t pending;

    if (control == NULL) {
        return 0;
    }

    if (control->magic != IAP_BOOT_CONTROL_MAGIC || control->version != IAP_BOOT_VERSION) {
        return 0;
    }

    active = (iap_slot_t)control->active_slot;
    pending = (iap_slot_t)control->pending_slot;

    if (!iap_is_valid_slot(active)) {
        return 0;
    }

    if (control->pending_slot != (uint32_t)IAP_SLOT_NONE &&
        !iap_is_valid_slot(pending)) {
        return 0;
    }

    if (control->max_boot_attempts == 0u ||
        control->boot_attempts > control->max_boot_attempts ||
        control->confirmed[IAP_SLOT_A] > 1u ||
        control->confirmed[IAP_SLOT_B] > 1u ||
        control->confirmed[(uint32_t)active] != 1u) {
        return 0;
    }

    if (pending == IAP_SLOT_NONE) {
        return control->boot_attempts == 0u;
    }

    return pending != active && control->confirmed[(uint32_t)pending] == 0u;
}

uint32_t iap_crc32_update(uint32_t crc, const void *data, uint32_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t i;
    uint32_t bit;

    if (data == NULL && size > 0u) {
        return crc;
    }

    for (i = 0; i < size; i++) {
        crc ^= bytes[i];
        for (bit = 0; bit < 8u; bit++) {
            if ((crc & 1u) != 0u) {
                crc = (crc >> 1u) ^ IAP_CRC32_POLY;
            } else {
                crc >>= 1u;
            }
        }
    }

    return crc;
}

uint32_t iap_crc32(const void *data, uint32_t size)
{
    return iap_crc32_update(IAP_CRC32_INITIAL, data, size) ^ IAP_CRC32_INITIAL;
}

iap_status_t iap_init(iap_context_t *ctx,
                      const iap_storage_ops_t *storage,
                      const iap_slot_desc_t slots[IAP_SLOT_COUNT],
                      uint32_t boot_control_address,
                      uint32_t max_boot_attempts)
{
    if (ctx == NULL || storage == NULL || slots == NULL ||
        storage->read == NULL || storage->write == NULL || storage->erase == NULL) {
        return IAP_ERR_INVALID_ARG;
    }

    if (slots[IAP_SLOT_A].size <= sizeof(iap_image_header_t) ||
        slots[IAP_SLOT_B].size <= sizeof(iap_image_header_t) ||
        !iap_range_is_valid(slots[IAP_SLOT_A].address, slots[IAP_SLOT_A].size) ||
        !iap_range_is_valid(slots[IAP_SLOT_B].address, slots[IAP_SLOT_B].size) ||
        !iap_range_is_valid(boot_control_address, (uint32_t)sizeof(iap_boot_control_t)) ||
        iap_ranges_overlap(slots[IAP_SLOT_A].address,
                           slots[IAP_SLOT_A].size,
                           slots[IAP_SLOT_B].address,
                           slots[IAP_SLOT_B].size) ||
        iap_ranges_overlap(slots[IAP_SLOT_A].address,
                           slots[IAP_SLOT_A].size,
                           boot_control_address,
                           (uint32_t)sizeof(iap_boot_control_t)) ||
        iap_ranges_overlap(slots[IAP_SLOT_B].address,
                           slots[IAP_SLOT_B].size,
                           boot_control_address,
                           (uint32_t)sizeof(iap_boot_control_t))) {
        return IAP_ERR_INVALID_ARG;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->storage = *storage;
    ctx->slots[IAP_SLOT_A] = slots[IAP_SLOT_A];
    ctx->slots[IAP_SLOT_B] = slots[IAP_SLOT_B];
    ctx->boot_control_address = boot_control_address;
    ctx->max_boot_attempts = max_boot_attempts == 0u ? IAP_DEFAULT_MAX_BOOT_ATTEMPTS : max_boot_attempts;

    return IAP_OK;
}

iap_boot_control_t iap_default_boot_control(iap_slot_t active_slot, uint32_t max_boot_attempts)
{
    iap_boot_control_t control;

    memset(&control, 0, sizeof(control));
    control.magic = IAP_BOOT_CONTROL_MAGIC;
    control.version = IAP_BOOT_VERSION;
    control.active_slot = iap_is_valid_slot(active_slot) ? (uint32_t)active_slot : (uint32_t)IAP_SLOT_A;
    control.pending_slot = (uint32_t)IAP_SLOT_NONE;
    control.boot_attempts = 0u;
    control.max_boot_attempts = max_boot_attempts == 0u ? IAP_DEFAULT_MAX_BOOT_ATTEMPTS : max_boot_attempts;
    control.confirmed[control.active_slot] = 1u;

    return control;
}

iap_status_t iap_boot_control_load(const iap_context_t *ctx, iap_boot_control_t *control)
{
    iap_status_t status;

    if (ctx == NULL || control == NULL) {
        return IAP_ERR_INVALID_ARG;
    }

    status = ctx->storage.read(ctx->storage.user,
                               ctx->boot_control_address,
                               control,
                               (uint32_t)sizeof(*control));
    if (status != IAP_OK) {
        return status;
    }

    if (!iap_boot_control_is_valid(control)) {
        return IAP_ERR_BOOT_CONTROL;
    }

    return IAP_OK;
}

iap_status_t iap_boot_control_save(const iap_context_t *ctx, const iap_boot_control_t *control)
{
    if (ctx == NULL || control == NULL || !iap_boot_control_is_valid(control)) {
        return IAP_ERR_INVALID_ARG;
    }

    return ctx->storage.write(ctx->storage.user,
                              ctx->boot_control_address,
                              control,
                              (uint32_t)sizeof(*control));
}

iap_status_t iap_verify_image(const iap_context_t *ctx, iap_slot_t slot)
{
    iap_image_header_t header;
    const iap_slot_desc_t *desc;
    uint8_t buffer[64];
    uint32_t remaining;
    uint32_t offset;
    uint32_t crc;
    iap_status_t status;

    if (ctx == NULL || !iap_is_valid_slot(slot)) {
        return IAP_ERR_INVALID_ARG;
    }

    desc = &ctx->slots[(uint32_t)slot];
    status = iap_read_header(ctx, slot, &header);
    if (status != IAP_OK) {
        return status;
    }

    if (!iap_image_header_is_valid(desc, &header)) {
        return IAP_ERR_VERIFY;
    }

    remaining = header.image_size;
    offset = header.header_size;
    crc = IAP_CRC32_INITIAL;

    while (remaining > 0u) {
        uint32_t chunk = remaining > (uint32_t)sizeof(buffer) ? (uint32_t)sizeof(buffer) : remaining;
        status = ctx->storage.read(ctx->storage.user, desc->address + offset, buffer, chunk);
        if (status != IAP_OK) {
            return status;
        }
        crc = iap_crc32_update(crc, buffer, chunk);
        remaining -= chunk;
        offset += chunk;
    }

    crc ^= IAP_CRC32_INITIAL;
    return crc == header.image_crc32 ? IAP_OK : IAP_ERR_VERIFY;
}

iap_status_t iap_get_image_boot_address(const iap_context_t *ctx,
                                        iap_slot_t slot,
                                        uint32_t *boot_address)
{
    iap_image_header_t header;
    const iap_slot_desc_t *desc;
    iap_status_t status;

    if (ctx == NULL || boot_address == NULL || !iap_is_valid_slot(slot)) {
        return IAP_ERR_INVALID_ARG;
    }

    desc = &ctx->slots[(uint32_t)slot];
    status = iap_read_header(ctx, slot, &header);
    if (status != IAP_OK) {
        return status;
    }

    if (!iap_image_header_is_valid(desc, &header)) {
        return IAP_ERR_VERIFY;
    }

    *boot_address = desc->address + header.header_size;
    return IAP_OK;
}

iap_status_t iap_select_boot_slot(const iap_context_t *ctx,
                                  iap_boot_control_t *control,
                                  iap_slot_t *selected_slot)
{
    iap_boot_control_t updated;
    iap_slot_t pending;
    iap_slot_t active;
    iap_status_t status;

    if (ctx == NULL || control == NULL || selected_slot == NULL ||
        !iap_boot_control_is_valid(control)) {
        return IAP_ERR_INVALID_ARG;
    }

    *selected_slot = IAP_SLOT_NONE;
    pending = (iap_slot_t)control->pending_slot;
    active = (iap_slot_t)control->active_slot;

    if (pending != IAP_SLOT_NONE) {
        status = iap_verify_image(ctx, pending);
        if (status == IAP_OK && control->boot_attempts < control->max_boot_attempts) {
            updated = *control;
            updated.boot_attempts++;
            status = iap_boot_control_save(ctx, &updated);
            if (status != IAP_OK) {
                return status;
            }

            *control = updated;
            *selected_slot = pending;
            return IAP_OK;
        }

        if (status != IAP_OK && status != IAP_ERR_VERIFY) {
            return status;
        }

        status = iap_rollback(ctx, control);
        if (status != IAP_OK) {
            return status;
        }
    }

    status = iap_verify_image(ctx, active);
    if (status == IAP_OK) {
        *selected_slot = active;
        return IAP_OK;
    }

    return status == IAP_ERR_VERIFY ? IAP_ERR_NO_VALID_IMAGE : status;
}

iap_status_t iap_mark_pending(const iap_context_t *ctx, iap_boot_control_t *control, iap_slot_t new_slot)
{
    iap_boot_control_t updated;
    iap_image_header_t header;
    iap_status_t status;

    if (ctx == NULL || control == NULL || !iap_boot_control_is_valid(control) ||
        !iap_is_valid_slot(new_slot) || new_slot == (iap_slot_t)control->active_slot) {
        return IAP_ERR_INVALID_ARG;
    }

    status = iap_verify_image(ctx, new_slot);
    if (status != IAP_OK) {
        return status;
    }

    status = iap_read_header(ctx, new_slot, &header);
    if (status != IAP_OK) {
        return status;
    }

    updated = *control;
    updated.pending_slot = (uint32_t)new_slot;
    updated.boot_attempts = 0u;
    updated.image_crc32[(uint32_t)new_slot] = header.image_crc32;
    updated.image_size[(uint32_t)new_slot] = header.image_size;
    updated.confirmed[(uint32_t)new_slot] = 0u;

    status = iap_boot_control_save(ctx, &updated);
    if (status == IAP_OK) {
        *control = updated;
    }
    return status;
}

iap_status_t iap_confirm_current_image(const iap_context_t *ctx,
                                       iap_boot_control_t *control,
                                       iap_slot_t current_slot)
{
    iap_boot_control_t updated;
    iap_status_t status;

    if (ctx == NULL || control == NULL || !iap_boot_control_is_valid(control) ||
        !iap_is_valid_slot(current_slot)) {
        return IAP_ERR_INVALID_ARG;
    }

    if (current_slot != (iap_slot_t)control->active_slot &&
        current_slot != (iap_slot_t)control->pending_slot) {
        return IAP_ERR_INVALID_ARG;
    }

    status = iap_verify_image(ctx, current_slot);
    if (status != IAP_OK) {
        return status;
    }

    updated = *control;
    updated.active_slot = (uint32_t)current_slot;
    updated.pending_slot = (uint32_t)IAP_SLOT_NONE;
    updated.boot_attempts = 0u;
    updated.confirmed[IAP_SLOT_A] = current_slot == IAP_SLOT_A ? 1u : 0u;
    updated.confirmed[IAP_SLOT_B] = current_slot == IAP_SLOT_B ? 1u : 0u;

    status = iap_boot_control_save(ctx, &updated);
    if (status == IAP_OK) {
        *control = updated;
    }
    return status;
}

iap_status_t iap_rollback(const iap_context_t *ctx, iap_boot_control_t *control)
{
    iap_boot_control_t updated;
    iap_status_t status;

    if (ctx == NULL || control == NULL || !iap_boot_control_is_valid(control)) {
        return IAP_ERR_INVALID_ARG;
    }

    updated = *control;
    updated.pending_slot = (uint32_t)IAP_SLOT_NONE;
    updated.boot_attempts = 0u;
    updated.confirmed[(uint32_t)iap_other_slot((iap_slot_t)updated.active_slot)] = 0u;

    status = iap_boot_control_save(ctx, &updated);
    if (status == IAP_OK) {
        *control = updated;
    }
    return status;
}

iap_status_t iap_write_candidate_image(const iap_context_t *ctx,
                                       iap_slot_t target_slot,
                                       const void *image,
                                       uint32_t size)
{
    iap_image_header_t header;
    const iap_slot_desc_t *desc;
    const uint8_t *bytes;
    iap_status_t status;

    if (ctx == NULL || image == NULL || !iap_is_valid_slot(target_slot) ||
        size < sizeof(iap_image_header_t)) {
        return IAP_ERR_INVALID_ARG;
    }

    desc = &ctx->slots[(uint32_t)target_slot];
    if (size > desc->size) {
        return IAP_ERR_INVALID_ARG;
    }

    bytes = (const uint8_t *)image;
    memcpy(&header, image, sizeof(header));
    if (!iap_image_header_is_valid(desc, &header) ||
        size != header.header_size + header.image_size ||
        iap_crc32(&bytes[header.header_size], header.image_size) != header.image_crc32) {
        return IAP_ERR_VERIFY;
    }

    status = ctx->storage.erase(ctx->storage.user, desc->address, desc->size);
    if (status != IAP_OK) {
        return status;
    }

    status = ctx->storage.write(ctx->storage.user, desc->address, image, size);
    if (status != IAP_OK) {
        return status;
    }

    return iap_verify_image(ctx, target_slot);
}
