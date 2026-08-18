# A/B 双镜像 IAP MVP 设计

本文档定义仓库第一版可落地的最小 IAP 代码设计。目标不是绑定某个 MCU 或工具链，而是先把 A/B 双镜像升级流程、状态数据和平台抽象固定下来，后续可以据此实现 C 代码。

## 1. 目标与边界

MVP 目标：

* 使用 A/B 双镜像，始终保留一个可回退的可启动镜像。
* Bootloader 通过持久化 boot control 决定启动哪个 slot。
* APP 负责把新固件写入非当前运行 slot，并在自检通过后确认升级成功。
* 通过 CRC、镜像大小和 magic 做最小完整性校验。
* 通过启动尝试次数避免新固件反复启动失败导致设备不可用。

MVP 暂不实现：

* 真实 Flash 驱动。
* 具体 UART、CAN、TCP、USB 等传输协议。
* 具体芯片启动文件、链接脚本、向量表重定位。
* 加密签名、证书链、反回滚版本策略。
* RTOS 适配。

## 2. Flash 布局

最小布局如下：

```text
+--------------------------+
| Bootloader               |
+--------------------------+
| APP Slot A               |
+--------------------------+
| APP Slot B               |
+--------------------------+
| Boot Control             |
+--------------------------+
```

约束：

* Bootloader 区域不可被 APP 擦写。
* 当前运行 slot 不允许写入新固件。
* 新固件总是写入另一个 slot。
* Boot Control 必须使用非易失性存储，并建议做双份备份或追加写日志，避免掉电写坏。

## 3. 核心数据结构

```c
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
    iap_slot_t active_slot;
    iap_slot_t pending_slot;
    uint32_t boot_attempts;
    uint32_t max_boot_attempts;
    uint32_t image_crc32[2];
    uint32_t image_size[2];
    uint32_t confirmed[2];
} iap_boot_control_t;
```

默认值：

```c
#define IAP_BOOT_CONTROL_MAGIC 0x49415042u  /* "IAPB" */
#define IAP_IMAGE_MAGIC        0x49415049u  /* "IAPI" */
#define IAP_BOOT_VERSION       1u
#define IAP_MAX_BOOT_ATTEMPTS  3u
```

## 4. 模块接口

### 4.1 iap_storage

`iap_storage` 只抽象存储行为，不关心内部使用片内 Flash、外部 Flash 还是仿真文件。

```c
typedef enum {
    IAP_OK = 0,
    IAP_ERR_INVALID_ARG,
    IAP_ERR_STORAGE,
    IAP_ERR_VERIFY,
    IAP_ERR_NO_VALID_IMAGE
} iap_status_t;

iap_status_t iap_storage_read(uint32_t address, void *buffer, uint32_t size);
iap_status_t iap_storage_write(uint32_t address, const void *buffer, uint32_t size);
iap_status_t iap_storage_erase(uint32_t address, uint32_t size);
```

### 4.2 iap_verify

```c
iap_status_t iap_verify_image(const iap_context_t *ctx, iap_slot_t slot);
iap_status_t iap_get_image_boot_address(const iap_context_t *ctx,
                                        iap_slot_t slot,
                                        uint32_t *boot_address);
uint32_t iap_crc32_update(uint32_t crc, const void *data, uint32_t size);
uint32_t iap_crc32(const void *data, uint32_t size);
```

最小校验规则：

* 镜像头 `magic` 必须等于 `IAP_IMAGE_MAGIC`。
* `header_size` 至少容纳镜像头、按 4 字节对齐，并作为 payload 与向量表相对 slot 的偏移。
* `image_size` 必须大于 0，且不能超过 slot 容量。
* 计算出的 CRC32 必须等于镜像头中的 `image_crc32`。

### 4.3 iap_core

```c
iap_status_t iap_boot_control_load(const iap_context_t *ctx, iap_boot_control_t *control);
iap_status_t iap_boot_control_save(const iap_context_t *ctx, const iap_boot_control_t *control);

iap_status_t iap_select_boot_slot(const iap_context_t *ctx,
                                  iap_boot_control_t *control,
                                  iap_slot_t *selected_slot);
iap_status_t iap_mark_pending(const iap_context_t *ctx,
                              iap_boot_control_t *control,
                              iap_slot_t new_slot);
iap_status_t iap_confirm_current_image(const iap_context_t *ctx,
                                       iap_boot_control_t *control,
                                       iap_slot_t current_slot);
iap_status_t iap_rollback(const iap_context_t *ctx, iap_boot_control_t *control);
```

职责：

* 读取并修复 boot control 的默认值。
* 判断 pending slot 是否可启动。
* 管理启动尝试次数。
* 在确认失败时回滚到旧 active slot。
* 在 APP 自检成功后完成 confirmed 状态切换。

### 4.4 iap_platform

```c
void iap_platform_jump_to_slot(iap_slot_t slot, uint32_t boot_address);
void iap_platform_reset(void);
void iap_platform_watchdog_start(void);
void iap_platform_watchdog_feed(void);
```

该模块由具体平台实现。通用 IAP Core 不直接操作寄存器。

## 5. Bootloader 流程

```c
void bootloader_main(const iap_context_t *ctx)
{
    iap_boot_control_t control;
    iap_status_t status;
    iap_slot_t slot;
    uint32_t boot_address;

    status = iap_boot_control_load(ctx, &control);
    if (status == IAP_ERR_BOOT_CONTROL) {
        control = iap_default_boot_control(IAP_SLOT_A, ctx->max_boot_attempts);
        status = iap_boot_control_save(ctx, &control);
    }

    if (status != IAP_OK) {
        bootloader_enter_recovery_mode(status);
        return;
    }

    status = iap_select_boot_slot(ctx, &control, &slot);
    if (status != IAP_OK) {
        bootloader_enter_recovery_mode(status);
        return;
    }

    status = iap_get_image_boot_address(ctx, slot, &boot_address);
    if (status != IAP_OK) {
        bootloader_enter_recovery_mode(status);
        return;
    }

    iap_platform_watchdog_start();
    iap_platform_jump_to_slot(slot, boot_address);
}
```

`iap_select_boot_slot` 必须区分镜像校验失败和底层存储错误。只有明确的镜像校验失败或启动次数耗尽才允许清除 pending；瞬时读写错误需要向 Bootloader 返回，不能被误判为坏镜像。增加启动尝试次数时，应先成功持久化 Boot Control，再允许跳转到 pending 镜像。

## 6. APP 升级流程

APP 接收新固件时：

```c
iap_status_t app_install_candidate_image(const iap_context_t *ctx,
                                         iap_boot_control_t *control,
                                         const void *image,
                                         uint32_t size)
{
    iap_slot_t current = app_get_current_slot();
    iap_slot_t target = (current == IAP_SLOT_A) ? IAP_SLOT_B : IAP_SLOT_A;
    iap_status_t status;

    status = iap_write_candidate_image(ctx, target, image, size);
    if (status != IAP_OK) {
        return status;
    }

    status = iap_mark_pending(ctx, control, target);
    if (status != IAP_OK) {
        return status;
    }

    iap_platform_reset();

    return IAP_OK;
}
```

新 APP 启动并完成关键自检后：

```c
void app_after_self_test_passed(const iap_context_t *ctx,
                                iap_boot_control_t *control)
{
    (void)iap_confirm_current_image(ctx, control, app_get_current_slot());
}
```

确认成功时必须更新：

* `active_slot = current_slot`
* `pending_slot = IAP_SLOT_NONE`
* `confirmed[current_slot] = 1`
* `boot_attempts = 0`

## 7. 失败处理

### pending 镜像校验失败

Bootloader 必须清除 pending 状态并回滚到 active slot。

`iap_rollback(ctx, control)` 先持久化清除 pending 和启动次数的更新，保存成功后再更新调用方持有的控制块。

### pending 镜像启动失败

如果新 APP 没有调用 `iap_confirm_current_image`，设备会因为看门狗、主动复位或异常复位再次进入 Bootloader。Bootloader 继续递增 `boot_attempts`，超过 `max_boot_attempts` 后回滚。

### active 镜像也不可用

如果 active slot 校验失败，并且没有可用 pending slot，Bootloader 进入 recovery mode。MVP 只定义入口，不规定 recovery 的通信协议。

```c
void bootloader_enter_recovery_mode(iap_status_t reason);
```

## 8. 最小测试场景

后续实现代码时至少覆盖以下场景：

* 无 pending：Bootloader 启动 `active_slot`。
* 正常升级：A 运行，B 写入成功，重启后启动 B。
* 确认成功：B 自检通过后调用确认接口，B 成为 active。
* 启动失败：B 未确认并多次复位，超过 3 次后回滚 A。
* 镜像损坏：pending slot CRC 错误，拒绝启动并回滚。
* Boot Control 损坏：magic 无效时恢复默认控制块。
* 无可用镜像：active 与 pending 都不可用时进入 recovery mode。

## 9. 后续演进

MVP 后再逐步加入：

* 双份 boot control 与掉电安全提交。
* 固件 SHA256 与签名校验。
* 版本号与反回滚策略。
* 传输层适配：UART、CAN、TCP、USB、文件。
* 具体平台示例：STM32、RT-Thread、FreeRTOS 或裸机。
* Host 侧打包工具，用于生成带 header 的固件镜像。
