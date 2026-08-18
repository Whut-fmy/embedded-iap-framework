#include "iap/iap.h"
#include "iap/iap_bootloader.h"
#include "iap/iap_memory_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEMORY_SIZE 4096u
#define SLOT_SIZE 1024u
#define SLOT_A_ADDR 0u
#define SLOT_B_ADDR 1024u
#define BOOT_CONTROL_ADDR 3072u
#define IMAGE_HEADER_AREA_SIZE 128u

#define ASSERT_OK(expr) assert_status((expr), IAP_OK, #expr, __LINE__)
#define ASSERT_EQ_U32(actual, expected) assert_u32((actual), (expected), #actual, __LINE__)

typedef struct {
    uint32_t watchdog_started;
    uint32_t jumped;
    uint32_t recovered;
    iap_slot_t jumped_slot;
    uint32_t jumped_address;
    iap_status_t recovery_reason;
} test_bootloader_platform_t;

static void assert_status(iap_status_t actual, iap_status_t expected, const char *expr, int line)
{
    if (actual != expected) {
        fprintf(stderr, "line %d: %s expected %d got %d\n", line, expr, expected, actual);
        exit(1);
    }
}

static void assert_u32(uint32_t actual, uint32_t expected, const char *expr, int line)
{
    if (actual != expected) {
        fprintf(stderr, "line %d: %s expected %lu got %lu\n",
                line,
                expr,
                (unsigned long)expected,
                (unsigned long)actual);
        exit(1);
    }
}

static iap_status_t fail_storage_read(void *user,
                                      uint32_t address,
                                      void *buffer,
                                      uint32_t size)
{
    (void)user;
    (void)address;
    (void)buffer;
    (void)size;
    return IAP_ERR_STORAGE;
}

static iap_status_t fail_storage_write(void *user,
                                       uint32_t address,
                                       const void *buffer,
                                       uint32_t size)
{
    (void)user;
    (void)address;
    (void)buffer;
    (void)size;
    return IAP_ERR_STORAGE;
}

static uint32_t make_image(uint8_t *out, uint32_t out_size, uint8_t seed)
{
    iap_image_header_t header;
    uint32_t payload_size = 96u;
    uint32_t i;

    if (out_size < IMAGE_HEADER_AREA_SIZE + payload_size) {
        return 0u;
    }

    memset(out, 0, out_size);
    for (i = 0; i < payload_size; i++) {
        out[IMAGE_HEADER_AREA_SIZE + i] = (uint8_t)(seed + i);
    }

    header.magic = IAP_IMAGE_MAGIC;
    header.header_size = IMAGE_HEADER_AREA_SIZE;
    header.image_size = payload_size;
    header.image_crc32 = iap_crc32(&out[IMAGE_HEADER_AREA_SIZE], payload_size);
    header.version = IAP_IMAGE_HEADER_VERSION;
    header.flags = 0u;
    memcpy(out, &header, sizeof(header));

    return IMAGE_HEADER_AREA_SIZE + payload_size;
}

static void make_context(iap_context_t *ctx,
                         iap_memory_storage_t *storage,
                         uint8_t *memory)
{
    iap_storage_ops_t ops;
    iap_slot_desc_t slots[IAP_SLOT_COUNT];

    iap_memory_storage_init(storage, memory, MEMORY_SIZE, 0xFFu);
    ops = iap_memory_storage_ops(storage);

    slots[IAP_SLOT_A].address = SLOT_A_ADDR;
    slots[IAP_SLOT_A].size = SLOT_SIZE;
    slots[IAP_SLOT_B].address = SLOT_B_ADDR;
    slots[IAP_SLOT_B].size = SLOT_SIZE;

    ASSERT_OK(iap_init(ctx, &ops, slots, BOOT_CONTROL_ADDR, IAP_DEFAULT_MAX_BOOT_ATTEMPTS));
}

static void install_initial_active(iap_context_t *ctx, iap_slot_t active_slot)
{
    uint8_t image[256];
    uint32_t image_size = make_image(image, (uint32_t)sizeof(image), 0x10u);
    iap_boot_control_t control = iap_default_boot_control(active_slot, IAP_DEFAULT_MAX_BOOT_ATTEMPTS);

    ASSERT_OK(iap_write_candidate_image(ctx, active_slot, image, image_size));
    ASSERT_OK(iap_boot_control_save(ctx, &control));
}

static iap_slot_t select_boot_slot(iap_context_t *ctx, iap_boot_control_t *control)
{
    iap_slot_t selected_slot = IAP_SLOT_NONE;

    ASSERT_OK(iap_select_boot_slot(ctx, control, &selected_slot));
    return selected_slot;
}

static void test_watchdog_start(void *user)
{
    test_bootloader_platform_t *platform = (test_bootloader_platform_t *)user;
    platform->watchdog_started++;
}

static void test_jump_to_slot(void *user, iap_slot_t slot, uint32_t address)
{
    test_bootloader_platform_t *platform = (test_bootloader_platform_t *)user;
    platform->jumped++;
    platform->jumped_slot = slot;
    platform->jumped_address = address;
}

static void test_enter_recovery(void *user, iap_status_t reason)
{
    test_bootloader_platform_t *platform = (test_bootloader_platform_t *)user;
    platform->recovered++;
    platform->recovery_reason = reason;
}

static iap_bootloader_ops_t make_bootloader_ops(test_bootloader_platform_t *platform)
{
    iap_bootloader_ops_t ops;

    memset(platform, 0, sizeof(*platform));
    platform->jumped_slot = IAP_SLOT_NONE;
    platform->recovery_reason = IAP_OK;

    ops.watchdog_start = test_watchdog_start;
    ops.jump_to_slot = test_jump_to_slot;
    ops.enter_recovery = test_enter_recovery;
    ops.user = platform;

    return ops;
}

static void test_boots_active_without_pending(void)
{
    uint8_t memory[MEMORY_SIZE];
    iap_memory_storage_t storage;
    iap_context_t ctx;
    iap_boot_control_t control;

    make_context(&ctx, &storage, memory);
    install_initial_active(&ctx, IAP_SLOT_A);
    ASSERT_OK(iap_boot_control_load(&ctx, &control));

    ASSERT_EQ_U32((uint32_t)select_boot_slot(&ctx, &control), (uint32_t)IAP_SLOT_A);
    ASSERT_EQ_U32(control.boot_attempts, 0u);
}

static void test_pending_slot_is_selected_and_confirmed(void)
{
    uint8_t memory[MEMORY_SIZE];
    uint8_t image[256];
    iap_memory_storage_t storage;
    iap_context_t ctx;
    iap_boot_control_t control;
    uint32_t image_size;

    make_context(&ctx, &storage, memory);
    install_initial_active(&ctx, IAP_SLOT_A);
    ASSERT_OK(iap_boot_control_load(&ctx, &control));

    image_size = make_image(image, (uint32_t)sizeof(image), 0x40u);
    ASSERT_OK(iap_write_candidate_image(&ctx, IAP_SLOT_B, image, image_size));
    ASSERT_OK(iap_mark_pending(&ctx, &control, IAP_SLOT_B));

    ASSERT_OK(iap_boot_control_load(&ctx, &control));
    ASSERT_EQ_U32((uint32_t)select_boot_slot(&ctx, &control), (uint32_t)IAP_SLOT_B);
    ASSERT_EQ_U32(control.boot_attempts, 1u);

    ASSERT_OK(iap_confirm_current_image(&ctx, &control, IAP_SLOT_B));
    ASSERT_OK(iap_boot_control_load(&ctx, &control));
    ASSERT_EQ_U32(control.active_slot, (uint32_t)IAP_SLOT_B);
    ASSERT_EQ_U32(control.pending_slot, (uint32_t)IAP_SLOT_NONE);
    ASSERT_EQ_U32(control.boot_attempts, 0u);
    ASSERT_EQ_U32(control.confirmed[IAP_SLOT_B], 1u);
}

static void test_failed_pending_rolls_back_after_attempt_limit(void)
{
    uint8_t memory[MEMORY_SIZE];
    uint8_t image[256];
    iap_memory_storage_t storage;
    iap_context_t ctx;
    iap_boot_control_t control;
    uint32_t image_size;

    make_context(&ctx, &storage, memory);
    install_initial_active(&ctx, IAP_SLOT_A);
    ASSERT_OK(iap_boot_control_load(&ctx, &control));

    image_size = make_image(image, (uint32_t)sizeof(image), 0x70u);
    ASSERT_OK(iap_write_candidate_image(&ctx, IAP_SLOT_B, image, image_size));
    ASSERT_OK(iap_mark_pending(&ctx, &control, IAP_SLOT_B));

    ASSERT_OK(iap_boot_control_load(&ctx, &control));
    ASSERT_EQ_U32((uint32_t)select_boot_slot(&ctx, &control), (uint32_t)IAP_SLOT_B);
    ASSERT_EQ_U32((uint32_t)select_boot_slot(&ctx, &control), (uint32_t)IAP_SLOT_B);
    ASSERT_EQ_U32((uint32_t)select_boot_slot(&ctx, &control), (uint32_t)IAP_SLOT_B);
    ASSERT_EQ_U32((uint32_t)select_boot_slot(&ctx, &control), (uint32_t)IAP_SLOT_A);
    ASSERT_EQ_U32(control.pending_slot, (uint32_t)IAP_SLOT_NONE);
    ASSERT_EQ_U32(control.boot_attempts, 0u);
}

static void test_corrupt_pending_image_rolls_back(void)
{
    uint8_t memory[MEMORY_SIZE];
    uint8_t image[256];
    iap_memory_storage_t storage;
    iap_context_t ctx;
    iap_boot_control_t control;
    uint32_t image_size;

    make_context(&ctx, &storage, memory);
    install_initial_active(&ctx, IAP_SLOT_A);
    ASSERT_OK(iap_boot_control_load(&ctx, &control));

    image_size = make_image(image, (uint32_t)sizeof(image), 0xA0u);
    ASSERT_OK(iap_write_candidate_image(&ctx, IAP_SLOT_B, image, image_size));
    ASSERT_OK(iap_mark_pending(&ctx, &control, IAP_SLOT_B));

    memory[SLOT_B_ADDR + IMAGE_HEADER_AREA_SIZE + 4u] ^= 0x5Au;

    ASSERT_OK(iap_boot_control_load(&ctx, &control));
    ASSERT_EQ_U32((uint32_t)select_boot_slot(&ctx, &control), (uint32_t)IAP_SLOT_A);
    ASSERT_EQ_U32(control.pending_slot, (uint32_t)IAP_SLOT_NONE);
}

static void assert_boot_control_rejected(iap_context_t *ctx,
                                         uint8_t *memory,
                                         const iap_boot_control_t *control,
                                         const char *description)
{
    iap_boot_control_t loaded;

    memcpy(&memory[BOOT_CONTROL_ADDR], control, sizeof(*control));
    assert_status(iap_boot_control_load(ctx, &loaded),
                  IAP_ERR_BOOT_CONTROL,
                  description,
                  __LINE__);
}

static void test_invalid_boot_control_is_detected(void)
{
    uint8_t memory[MEMORY_SIZE];
    iap_memory_storage_t storage;
    iap_context_t ctx;
    iap_boot_control_t control;

    make_context(&ctx, &storage, memory);
    assert_status(iap_boot_control_load(&ctx, &control), IAP_ERR_BOOT_CONTROL, "iap_boot_control_load", __LINE__);

    control = iap_default_boot_control(IAP_SLOT_A, IAP_DEFAULT_MAX_BOOT_ATTEMPTS);
    control.magic = 0u;
    assert_boot_control_rejected(&ctx, memory, &control, "control magic");

    control = iap_default_boot_control(IAP_SLOT_A, IAP_DEFAULT_MAX_BOOT_ATTEMPTS);
    control.active_slot = 2u;
    assert_boot_control_rejected(&ctx, memory, &control, "active slot");

    control = iap_default_boot_control(IAP_SLOT_A, IAP_DEFAULT_MAX_BOOT_ATTEMPTS);
    control.pending_slot = 2u;
    assert_boot_control_rejected(&ctx, memory, &control, "pending slot");

    control = iap_default_boot_control(IAP_SLOT_A, IAP_DEFAULT_MAX_BOOT_ATTEMPTS);
    control.max_boot_attempts = 0u;
    assert_boot_control_rejected(&ctx, memory, &control, "zero attempt limit");

    control = iap_default_boot_control(IAP_SLOT_A, IAP_DEFAULT_MAX_BOOT_ATTEMPTS);
    control.boot_attempts = control.max_boot_attempts + 1u;
    assert_boot_control_rejected(&ctx, memory, &control, "attempts over limit");

    control = iap_default_boot_control(IAP_SLOT_A, IAP_DEFAULT_MAX_BOOT_ATTEMPTS);
    control.confirmed[IAP_SLOT_A] = 0u;
    assert_boot_control_rejected(&ctx, memory, &control, "active not confirmed");
}

static void test_bootloader_jumps_to_active_slot(void)
{
    uint8_t memory[MEMORY_SIZE];
    iap_memory_storage_t storage;
    iap_context_t ctx;
    test_bootloader_platform_t platform;
    iap_bootloader_ops_t ops;

    make_context(&ctx, &storage, memory);
    install_initial_active(&ctx, IAP_SLOT_A);
    ops = make_bootloader_ops(&platform);

    ASSERT_OK(iap_bootloader_run(&ctx, &ops, IAP_SLOT_A));
    ASSERT_EQ_U32(platform.watchdog_started, 1u);
    ASSERT_EQ_U32(platform.jumped, 1u);
    ASSERT_EQ_U32(platform.recovered, 0u);
    ASSERT_EQ_U32((uint32_t)platform.jumped_slot, (uint32_t)IAP_SLOT_A);
    ASSERT_EQ_U32(platform.jumped_address, SLOT_A_ADDR + IMAGE_HEADER_AREA_SIZE);
}

static void test_bootloader_initializes_missing_boot_control(void)
{
    uint8_t memory[MEMORY_SIZE];
    uint8_t image[256];
    iap_memory_storage_t storage;
    iap_context_t ctx;
    test_bootloader_platform_t platform;
    iap_bootloader_ops_t ops;
    uint32_t image_size;

    make_context(&ctx, &storage, memory);
    image_size = make_image(image, (uint32_t)sizeof(image), 0xB0u);
    ASSERT_OK(iap_write_candidate_image(&ctx, IAP_SLOT_A, image, image_size));
    ops = make_bootloader_ops(&platform);

    ASSERT_OK(iap_bootloader_run(&ctx, &ops, IAP_SLOT_A));
    ASSERT_EQ_U32(platform.jumped, 1u);
    ASSERT_EQ_U32((uint32_t)platform.jumped_slot, (uint32_t)IAP_SLOT_A);
    ASSERT_EQ_U32(platform.recovered, 0u);
}

static void test_bootloader_enters_recovery_when_no_image_is_valid(void)
{
    uint8_t memory[MEMORY_SIZE];
    iap_memory_storage_t storage;
    iap_context_t ctx;
    test_bootloader_platform_t platform;
    iap_bootloader_ops_t ops;

    make_context(&ctx, &storage, memory);
    ops = make_bootloader_ops(&platform);

    assert_status(iap_bootloader_run(&ctx, &ops, IAP_SLOT_A),
                  IAP_ERR_NO_VALID_IMAGE,
                  "iap_bootloader_run",
                  __LINE__);
    ASSERT_EQ_U32(platform.watchdog_started, 0u);
    ASSERT_EQ_U32(platform.jumped, 0u);
    ASSERT_EQ_U32(platform.recovered, 1u);
    ASSERT_EQ_U32((uint32_t)platform.recovery_reason, (uint32_t)IAP_ERR_NO_VALID_IMAGE);
}

static void assert_invalid_candidate_preserves_slot(iap_context_t *ctx,
                                                    uint8_t *image,
                                                    uint32_t image_size,
                                                    const char *description)
{
    assert_status(iap_write_candidate_image(ctx, IAP_SLOT_B, image, image_size),
                  IAP_ERR_VERIFY,
                  description,
                  __LINE__);
    ASSERT_OK(iap_verify_image(ctx, IAP_SLOT_B));
}

static void test_rejects_invalid_candidates_without_erasing_valid_slot(void)
{
    uint8_t memory[MEMORY_SIZE];
    uint8_t image[256];
    iap_memory_storage_t storage;
    iap_context_t ctx;
    iap_image_header_t header;
    uint32_t image_size;

    make_context(&ctx, &storage, memory);
    image_size = make_image(image, (uint32_t)sizeof(image), 0x20u);
    ASSERT_OK(iap_write_candidate_image(&ctx, IAP_SLOT_B, image, image_size));

    image_size = make_image(image, (uint32_t)sizeof(image), 0x30u);
    memcpy(&header, image, sizeof(header));
    header.magic = 0u;
    memcpy(image, &header, sizeof(header));
    assert_invalid_candidate_preserves_slot(&ctx, image, image_size, "invalid magic");

    image_size = make_image(image, (uint32_t)sizeof(image), 0x30u);
    memcpy(&header, image, sizeof(header));
    header.header_size = (uint32_t)sizeof(header) - 4u;
    memcpy(image, &header, sizeof(header));
    assert_invalid_candidate_preserves_slot(&ctx, image, image_size, "short header");

    image_size = make_image(image, (uint32_t)sizeof(image), 0x30u);
    memcpy(&header, image, sizeof(header));
    header.header_size++;
    memcpy(image, &header, sizeof(header));
    assert_invalid_candidate_preserves_slot(&ctx, image, image_size, "unaligned header");

    image_size = make_image(image, (uint32_t)sizeof(image), 0x30u);
    memcpy(&header, image, sizeof(header));
    header.version = IAP_IMAGE_HEADER_VERSION + 1u;
    memcpy(image, &header, sizeof(header));
    assert_invalid_candidate_preserves_slot(&ctx, image, image_size, "unsupported version");

    image_size = make_image(image, (uint32_t)sizeof(image), 0x30u);
    memcpy(&header, image, sizeof(header));
    header.image_size = SLOT_SIZE;
    memcpy(image, &header, sizeof(header));
    assert_invalid_candidate_preserves_slot(&ctx, image, image_size, "oversized payload");

    image_size = make_image(image, (uint32_t)sizeof(image), 0x30u);
    image[IMAGE_HEADER_AREA_SIZE + 4u] ^= 0x5Au;
    assert_invalid_candidate_preserves_slot(&ctx, image, image_size, "payload crc");
}

static void test_storage_read_error_does_not_rollback_pending_image(void)
{
    uint8_t memory[MEMORY_SIZE];
    uint8_t image[256];
    iap_memory_storage_t storage;
    iap_context_t ctx;
    iap_boot_control_t control;
    iap_slot_t selected_slot = IAP_SLOT_A;
    uint32_t image_size;

    make_context(&ctx, &storage, memory);
    install_initial_active(&ctx, IAP_SLOT_A);
    ASSERT_OK(iap_boot_control_load(&ctx, &control));
    image_size = make_image(image, (uint32_t)sizeof(image), 0x40u);
    ASSERT_OK(iap_write_candidate_image(&ctx, IAP_SLOT_B, image, image_size));
    ASSERT_OK(iap_mark_pending(&ctx, &control, IAP_SLOT_B));

    ctx.storage.read = fail_storage_read;
    assert_status(iap_select_boot_slot(&ctx, &control, &selected_slot),
                  IAP_ERR_STORAGE,
                  "iap_select_boot_slot",
                  __LINE__);
    ASSERT_EQ_U32((uint32_t)selected_slot, (uint32_t)IAP_SLOT_NONE);
    ASSERT_EQ_U32(control.pending_slot, (uint32_t)IAP_SLOT_B);
    ASSERT_EQ_U32(control.boot_attempts, 0u);
}

static void test_boot_attempt_is_not_committed_when_control_save_fails(void)
{
    uint8_t memory[MEMORY_SIZE];
    uint8_t image[256];
    iap_memory_storage_t storage;
    iap_context_t ctx;
    iap_boot_control_t control;
    iap_slot_t selected_slot = IAP_SLOT_A;
    uint32_t image_size;

    make_context(&ctx, &storage, memory);
    install_initial_active(&ctx, IAP_SLOT_A);
    ASSERT_OK(iap_boot_control_load(&ctx, &control));
    image_size = make_image(image, (uint32_t)sizeof(image), 0x50u);
    ASSERT_OK(iap_write_candidate_image(&ctx, IAP_SLOT_B, image, image_size));
    ASSERT_OK(iap_mark_pending(&ctx, &control, IAP_SLOT_B));

    ctx.storage.write = fail_storage_write;
    assert_status(iap_select_boot_slot(&ctx, &control, &selected_slot),
                  IAP_ERR_STORAGE,
                  "iap_select_boot_slot",
                  __LINE__);
    ASSERT_EQ_U32((uint32_t)selected_slot, (uint32_t)IAP_SLOT_NONE);
    ASSERT_EQ_U32(control.pending_slot, (uint32_t)IAP_SLOT_B);
    ASSERT_EQ_U32(control.boot_attempts, 0u);
}

static void test_cannot_confirm_a_non_pending_slot(void)
{
    uint8_t memory[MEMORY_SIZE];
    uint8_t image[256];
    iap_memory_storage_t storage;
    iap_context_t ctx;
    iap_boot_control_t control;
    uint32_t image_size;

    make_context(&ctx, &storage, memory);
    install_initial_active(&ctx, IAP_SLOT_A);
    image_size = make_image(image, (uint32_t)sizeof(image), 0x60u);
    ASSERT_OK(iap_write_candidate_image(&ctx, IAP_SLOT_B, image, image_size));
    ASSERT_OK(iap_boot_control_load(&ctx, &control));

    assert_status(iap_confirm_current_image(&ctx, &control, IAP_SLOT_B),
                  IAP_ERR_INVALID_ARG,
                  "iap_confirm_current_image",
                  __LINE__);
    ASSERT_EQ_U32(control.active_slot, (uint32_t)IAP_SLOT_A);
}

static void test_init_rejects_overlapping_or_wrapping_layouts(void)
{
    uint8_t memory[MEMORY_SIZE];
    iap_memory_storage_t storage;
    iap_storage_ops_t ops;
    iap_slot_desc_t slots[IAP_SLOT_COUNT];
    iap_context_t ctx;

    iap_memory_storage_init(&storage, memory, MEMORY_SIZE, 0xFFu);
    ops = iap_memory_storage_ops(&storage);
    slots[IAP_SLOT_A].address = SLOT_A_ADDR;
    slots[IAP_SLOT_A].size = SLOT_SIZE;
    slots[IAP_SLOT_B].address = SLOT_A_ADDR + (SLOT_SIZE / 2u);
    slots[IAP_SLOT_B].size = SLOT_SIZE;

    assert_status(iap_init(&ctx, &ops, slots, BOOT_CONTROL_ADDR, 3u),
                  IAP_ERR_INVALID_ARG,
                  "iap_init overlapping slots",
                  __LINE__);

    slots[IAP_SLOT_B].address = SLOT_B_ADDR;
    assert_status(iap_init(&ctx, &ops, slots, SLOT_A_ADDR + 128u, 3u),
                  IAP_ERR_INVALID_ARG,
                  "iap_init overlapping control",
                  __LINE__);

    slots[IAP_SLOT_B].address = UINT32_MAX - 32u;
    assert_status(iap_init(&ctx, &ops, slots, BOOT_CONTROL_ADDR, 3u),
                  IAP_ERR_INVALID_ARG,
                  "iap_init wrapping slot",
                  __LINE__);
}

static void test_memory_storage_rejects_a_null_backing_buffer(void)
{
    uint8_t byte = 0u;
    iap_memory_storage_t storage;
    iap_storage_ops_t ops;

    iap_memory_storage_init(&storage, NULL, 16u, 0xFFu);
    ops = iap_memory_storage_ops(&storage);

    assert_status(ops.read(ops.user, 0u, &byte, 1u),
                  IAP_ERR_INVALID_ARG,
                  "memory read",
                  __LINE__);
    assert_status(ops.write(ops.user, 0u, &byte, 1u),
                  IAP_ERR_INVALID_ARG,
                  "memory write",
                  __LINE__);
    assert_status(ops.erase(ops.user, 0u, 1u),
                  IAP_ERR_INVALID_ARG,
                  "memory erase",
                  __LINE__);
}

int main(void)
{
    test_boots_active_without_pending();
    test_pending_slot_is_selected_and_confirmed();
    test_failed_pending_rolls_back_after_attempt_limit();
    test_corrupt_pending_image_rolls_back();
    test_invalid_boot_control_is_detected();
    test_bootloader_jumps_to_active_slot();
    test_bootloader_initializes_missing_boot_control();
    test_bootloader_enters_recovery_when_no_image_is_valid();
    test_rejects_invalid_candidates_without_erasing_valid_slot();
    test_storage_read_error_does_not_rollback_pending_image();
    test_boot_attempt_is_not_committed_when_control_save_fails();
    test_cannot_confirm_a_non_pending_slot();
    test_init_rejects_overlapping_or_wrapping_layouts();
    test_memory_storage_rejects_a_null_backing_buffer();

    puts("iap_core_tests: PASS");
    return 0;
}
