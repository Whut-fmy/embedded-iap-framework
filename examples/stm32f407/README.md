# STM32F407ZG 上板示例

本示例面向 STM32F407ZGT6（1 MB Flash、128 KB SRAM + 64 KB CCM）。它使用内部 HSI，不依赖开发板晶振、LED 或串口，通过 ST-Link 读写固定 RAM 控制块来观察测试状态。

真实冷启动前必须将开发板 `BOOT0` 固定为 `0/GND`。`BOOT0=1` 时 F407 会进入
`0x1FFFxxxx` 系统 ROM，而不是执行 `0x08000000` 的 Bootloader；软件复位测试可能
掩盖这个跳帽问题，因此首次真实断电后应读取 PC 或 RAM 状态确认启动源。ST 的
[RM0090 参考手册](https://www.st.com/resource/en/reference_manual/rm0090-stm32f4xx-reference-manual-stmicroelectronics.pdf)
说明 BOOT 引脚在复位后的第 4 个 SYSCLK 上升沿锁存；F407 的 BOOT0 是专用输入，
不能在应用中当作普通 GPIO 回读。

## Flash 分区

| 区域 | 地址范围 | 扇区 | 大小 |
| --- | --- | --- | --- |
| Bootloader | `0x08000000` - `0x0801FFFF` | 0 - 4 | 128 KB |
| Slot A | `0x08020000` - `0x0805FFFF` | 5 - 6 | 256 KB |
| Slot B | `0x08060000` - `0x0809FFFF` | 7 - 8 | 256 KB |
| Boot Control A | `0x080A0000` - `0x080BFFFF` | 9 | 128 KB |
| Boot Control B | `0x080C0000` - `0x080DFFFF` | 10 | 128 KB |
| 保留/Recovery | `0x080E0000` - `0x080FFFFF` | 11 | 128 KB |

每个 APP slot 预留 `0x200` 字节镜像头区域，向量表分别位于 `0x08020200` 和 `0x08060200`，满足 Cortex-M4 VTOR 对齐要求。Boot Control 使用双扇区、64 字节追加记录、CRC32 和最后提交字，切换扇区时保留上一扇区作为掉电回退副本。

## RAM 测试通道

| 区域 | 地址范围 | 用途 |
| --- | --- | --- |
| 固件运行 RAM | `0x20000000` - `0x20007FFF` | data、bss、heap/stack |
| ST-Link staging | `0x20008000` - `0x2001FEFF` | 暂存待安装 IAP 镜像 |
| 测试控制块 | `0x2001FF00` - `0x2001FFFF` | 命令、状态和复位信息 |

测试控制块属于链接脚本中的 `NOLOAD` 区域，软件复位后保持不变。命令字位于控制块最后 4 字节，ST-Link 先写完参数和状态区、最后才发布命令，避免目标运行时读到半写入参数。它仅用于当前上板验证，不是产品通信协议。

## 构建

依赖：ARM GNU Toolchain、CMake、Ninja、Python 3，以及 STM32CubeF4 固件包。

```powershell
cmake -S examples/stm32f407 -B build-stm32f407 -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=E:/projects/.codex/problem/embedded-iap-framework/examples/stm32f407/cmake/arm-none-eabi-toolchain.cmake `
  -DCMAKE_MAKE_PROGRAM=E:/SoftWares/STM32CubeCLT/STM32CubeCLT_1.19.0/Ninja/bin/ninja.exe `
  -DSTM32_CUBE_F4_ROOT=C:/Users/yfm/STM32Cube/Repository/STM32Cube_FW_F4_V1.28.3
cmake --build build-stm32f407 --clean-first
```

输出文件：

- `build-stm32f407/iap_bootloader.bin`：烧录到 `0x08000000`。
- `build-stm32f407/iap_app_a.iap.bin`：烧录到 Slot A 起始地址 `0x08020000`。
- `build-stm32f407/iap_app_b.iap.bin`：先下载到 staging RAM，由 Slot A 在片内安装到 Slot B。

## 烧录前备份

下面的读取命令不会修改芯片：

```powershell
STM32_Programmer_CLI.exe -c port=SWD freq=4000 `
  -u 0x08000000 0x100000 artifacts/board-backups/stm32f407zg_flash_before_iap.bin
```

确认备份大小为 1,048,576 字节并保存 SHA256。擦除扇区和下载 Flash 镜像属于破坏性操作，必须在用户明确确认后执行。

## 首次烧录

以下命令会擦除扇区 0 至 10，覆盖原固件：

```powershell
STM32_Programmer_CLI.exe -c port=SWD freq=4000 -e "[0 10]"
STM32_Programmer_CLI.exe -c port=SWD freq=4000 `
  -d build-stm32f407/iap_bootloader.bin 0x08000000 -v
STM32_Programmer_CLI.exe -c port=SWD freq=4000 `
  -d build-stm32f407/iap_app_a.iap.bin 0x08020000 -v -rst
```

## 读取状态

```powershell
STM32_Programmer_CLI.exe -c port=SWD mode=HOTPLUG freq=4000 `
  -u 0x2001FF00 0x40 artifacts/iap-test-control.bin -run
python tools/iap_test_control.py decode --input artifacts/iap-test-control.bin
```

运行时读写必须使用 `mode=HOTPLUG`。读取命令在结束时会暂停内核，因此应在同一条
命令末尾添加 `-run`；否则 IWDG 在内核暂停期间仍可能复位目标。

首次启动的预期结果是 `status_name: app-a-running`、`last_slot: 0`、`last_boot_address: 0x08020200`。

## 安装 Slot B

先将完整的 IAP 镜像放入 staging RAM，再写入安装命令：

```powershell
$imageSize = (Get-Item build-stm32f407/iap_app_b.iap.bin).Length
STM32_Programmer_CLI.exe -c port=SWD mode=HOTPLUG freq=4000 `
  -d build-stm32f407/iap_app_b.iap.bin 0x20008000 -v -run
python tools/iap_test_control.py make --command install-b --argument0 $imageSize `
  --output artifacts/iap-command.bin
STM32_Programmer_CLI.exe -c port=SWD mode=HOTPLUG freq=4000 `
  -d artifacts/iap-command.bin 0x2001FF00 -run
```

控制命令不要使用 `-v`。目标可能在 CubeProgrammer 回读校验前消费并修改命令块，
从而造成并非下载失败的 verify mismatch。

Slot A 会通过框架接口擦除并写入 Slot B、校验 CRC、标记 pending 并复位。Slot B 默认停在 `pending-held` 状态并持续喂狗，便于读取状态。

## 确认或触发回滚

确认当前 Slot B：

```powershell
python tools/iap_test_control.py make --command confirm `
  --output artifacts/iap-command.bin
STM32_Programmer_CLI.exe -c port=SWD mode=HOTPLUG freq=4000 `
  -d artifacts/iap-command.bin 0x2001FF00 -run
```

模拟新固件不喂狗：

```powershell
python tools/iap_test_control.py make --command fail `
  --output artifacts/iap-command.bin
STM32_Programmer_CLI.exe -c port=SWD mode=HOTPLUG freq=4000 `
  -d artifacts/iap-command.bin 0x2001FF00 -run
```

IWDG 超时约 8 秒。连续 3 次未确认启动后，下一次 Bootloader 启动应回滚到 Slot A，最终状态为 `rolled-back`。

## 确定性掉电停点

掉电测试命令会在指定阶段停住并持续喂狗。主机读到 `power-cut-armed` 后再切断真实
电源，避免依赖人工抢时间。支持的阶段如下：

| 名称 | 阶段值 | 停点 |
| --- | ---: | --- |
| `erase` | 1 | 擦除 Slot B 第一个扇区后 |
| `header` | 2 | 写入镜像头前 8 字节后 |
| `payload-25` | 3 | 写入约 25% 载荷后 |
| `payload-50` | 4 | 写入约 50% 载荷后 |
| `payload-90` | 5 | 写入约 90% 载荷后 |
| `before-pending` | 6 | 镜像完整写入并验证后、标记 pending 前 |
| `pending-control` | 7 | pending 控制记录正文写入后、提交字前 |
| `first-boot` | 8 | B 首次启动、确认前 |
| `confirm-control` | 9 | 确认记录正文写入后、提交字前 |

以擦除阶段为例：

```powershell
$imageSize = (Get-Item build-stm32f407/iap_app_b.iap.bin).Length
STM32_Programmer_CLI.exe -c port=SWD mode=HOTPLUG freq=4000 `
  -d build-stm32f407/iap_app_b.iap.bin 0x20008000 -v -run
python tools/iap_test_control.py make --command power-install `
  --argument0 $imageSize --power-phase erase `
  --output artifacts/iap-command-power.bin
STM32_Programmer_CLI.exe -c port=SWD mode=HOTPLUG freq=4000 `
  -d artifacts/iap-command-power.bin 0x2001FF00 -run
```

确认阶段使用 `--command power-confirm`，不需要 `--power-phase`。每次重新上电后先读取
控制块和两个 slot，再恢复下一轮的基线。不要用软件复位代替清单要求的真实断电。

若重新上电后 RAM 控制块仍为随机值，可读取核心 PC：

```powershell
STM32_Programmer_CLI.exe -c port=SWD mode=HOTPLUG freq=4000 `
  -score -coreReg PC MSP XPSR -run
```

PC 位于 `0x1FFFxxxx` 表示进入了系统 ROM，应先将 `BOOT0` 改为 `0/GND` 再重新冷启动；
此时不要把该轮计入 IAP 自动恢复通过次数。

## 恢复原固件

测试结束后可用烧录前的 1 MB 备份恢复整片 Flash。恢复同样会擦除当前 IAP 测试固件，执行前需要明确确认：

```powershell
STM32_Programmer_CLI.exe -c port=SWD freq=4000 -e all
STM32_Programmer_CLI.exe -c port=SWD freq=4000 `
  -d artifacts/board-backups/stm32f407zg_flash_before_iap.bin 0x08000000 -v -rst
```
