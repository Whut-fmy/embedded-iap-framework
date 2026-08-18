# STM32F407ZG 开发板验证记录

## 测试环境

- 日期：2026-07-14 至 2026-07-15
- MCU：STM32F407ZGT6
- 调试器：ST-Link V2，固件 V2J47S7
- STM32CubeProgrammer：2.20.0
- STM32CubeCLT：1.19.0
- ARM GNU Toolchain：13.3.1
- STM32CubeF4：1.28.3
- 连接：SWD 4 MHz，目标电压 3.26 V

芯片识别为 Device ID `0x413`、1 MB Flash、Cortex-M4。RDP 为 Level 0
（`0xAA`），扇区 0 至 11 均未启用写保护，独立看门狗为软件启动，BOR
当前为 OFF。

## 原固件备份

首次写 Flash 前已完整读取原有 1 MB 固件：

- 文件：`artifacts/board-backups/stm32f407zg_flash_before_iap_20260714.bin`
- 大小：1,048,576 字节
- SHA256：`475D30BEE119BE2CE2C38ACDD871AF7B8F58DB78AD07372B85B9A8A3B6295C05`
- `artifacts/` 已被 Git 忽略，不会误提交备份和板测临时文件。

原固件向量为 MSP `0x20005440`、Reset Handler `0x08000255`。取得上述备份并
获得用户确认后，才擦除扇区 0 至 10 并写入 IAP 测试固件；扇区 11 始终保留。

## 当前构建产物

| 产物 | Flash/打包大小 | 载荷 CRC32 | 启动地址 |
| --- | ---: | --- | --- |
| Bootloader | 8,228 字节 | - | `0x08000000` |
| Slot A APP | 12,416 字节 | `0x2E5E3C81` | `0x08020200` |
| Slot B APP | 12,416 字节 | `0x99765109` | `0x08060200` |

主机 `cmake --build build --clean-first`、`ctest --test-dir build
--output-on-failure` 以及 STM32 clean build 均通过，未产生编译警告。主机测试为
`iap_core_tests` 和 `iap_tool_tests`，共 2/2 通过。

## 基础硬件测试

| 编号 | 结果 | 板上证据 |
| --- | --- | --- |
| HW-01 调试连接 | 通过 | ST-Link 稳定识别 `0x413`、1 MB Flash、3.26 V |
| HW-02 Flash 擦写 | 通过 | 扇区 0 至 10 擦除、下载和逐文件 verify 均成功 |
| HW-03 边界保护 | 通过 | `boundary-guards-passed`；当前 slot、Bootloader、未对齐写及越界读写均被拒绝 |
| HW-04 单镜像启动 | 通过 | 默认控制块下反复启动 A，跳转地址 `0x08020200` |
| HW-05 看门狗 | 通过 | 停止喂狗后约 8 秒复位；复位标志包含 IWDG，3 次后回滚 A |

## IAP 闭环测试

| 编号 | 结果 | 板上证据 |
| --- | --- | --- |
| IAP-01 正常升级 | 通过 | A 片内擦写并校验 B；pending=B、attempts=1，跳转 `0x08060200` |
| IAP-02 升级确认 | 通过 | `confirmed`；复位后仍为 B，active=B、pending=none、attempts=0 |
| IAP-03 未确认回滚 | 通过 | attempts 依次为 1、2、3，随后 active=A、pending=none，状态 `rolled-back` |
| IAP-04 CRC 损坏 | 通过 | B 载荷字节 `0x19` 改为 `0x00` 后被拒绝并启动 A |
| IAP-05 镜像头损坏 | 通过 | magic、header size、version、image size 四种损坏均返回 `IAP_ERR_VERIFY`，原 B 未被擦除 |
| IAP-06 Boot Control 损坏 | 通过 | 提交字、控制 magic、slot、尝试次数配置损坏均回退上一有效记录；双扇区全空时初始化默认 A |
| IAP-07 无有效镜像 | 通过 | A/B magic 同时破坏后状态为 `recovery`，reason=`IAP_ERR_NO_VALID_IMAGE` |
| IAP-08 存储读失败 | 通过 | `read-failure-passed`；返回 `IAP_ERR_STORAGE`，未误清 pending 或增加 attempts |
| IAP-09 控制块写失败 | 通过 | `write-failure-passed`；未记录的 pending 未生效，active 保持不变 |
| IAP-10 非 pending 确认 | 通过 | active=A、B 有效且非 pending 时确认 B 被拒绝，状态 `non-pending-confirm-passed` |

候选镜像预校验还做了额外验证：先在 RAM 中损坏候选 B 的载荷，再调用安装接口，
结果在擦除前返回 `IAP_ERR_VERIFY`；Slot B 对应 Flash 字节仍为原值 `0x19`。

## Boot Control 掉电安全

Boot Control 使用两个 128 KB 扇区和 64 字节追加记录。记录包含序号、完整控制块、
CRC32，并将 `0x434F4D54` 提交字最后写入。板上将最新记录提交字清零后，Bootloader
忽略该记录并从上一条有效记录继续启动；随后可正常追加新记录并重新确认镜像。

运行时状态读取必须使用 `mode=HOTPLUG`，并在同一条 CubeProgrammer 命令末尾添加
`-run`。否则调试器暂停内核期间 IWDG 仍计时，会人为制造额外复位和启动次数。

## 已修复的板上问题

1. 镜像头预留区大于 C 结构体时，APP 启动地址必须使用 `slot + header_size`，不能
   直接跳到 slot 起点。
2. F407 启动 IWDG 前 LSI 尚未稳定，等待状态寄存器会卡死；现先启动 IWDG，再做
   有界等待并重载。
3. ST-Link 原先先写命令再写参数，运行中的 APP 可能读到半写入控制块；命令字现位于
   64 字节控制块最后一字，协议版本为 2。
4. F407 Flash 写驱动现拒绝未对齐地址，并持续保护当前运行 slot 和 Bootloader。
5. 候选镜像现在先完整验证头部、尺寸和 CRC，再执行目标 slot 擦除。

## 掉电测试进度

已加入确定性掉电测试停点：目标 slot 擦除、镜像头、载荷 25%/50%/90%、写完但未
pending、pending 控制记录、首次启动未确认、确认控制记录。停点状态统一为
`power-cut-armed`，固件在停点持续喂狗，等待人工切断真实电源。

第 1 次“目标 slot 擦除”已执行真实断电：重新上电后 Slot B 头部为全
`0xFFFFFFFF`，Boot Control 仍为 `active=A, pending=none`。首次冷启动因 BOOT0 实际为
高电平而进入 STM32 系统 ROM；随后将 BOOT0 对 GND 实测调整为 `0.33 V`，由 ST-Link
执行硬件复位后读取到 PC `0x080212DE`、`SYSCFG_MEMRMP=0x00000000`，测试控制块报告
`app-a-running`、启动地址 `0x08020200`。这证明擦除中断未破坏活动镜像或 Boot Control，
Bootloader 能在下一次有效冷启动时安全运行 Slot A，该轮计为通过。

系统 ROM 诊断依据 ST [RM0090](https://www.st.com/resource/en/reference_manual/rm0090-stm32f4xx-reference-manual-stmicroelectronics.pdf)
的启动模式和内存映射定义；后续掉电轮次均保持 BOOT0 接地，并同时检查 PC、MEMRMP 和
测试控制块，避免把启动源异常误判为 IAP 恢复失败。

第 1 次“镜像头写入”停点已完成真实断电。恢复后 Slot B 仅前 8 字节为
`49 50 41 49 00 02 00 00`，其余已检查头部字节保持 `0xFF`；Boot Control 仍为
`active=A, pending=none`，测试控制块报告 `app-a-running`、启动地址 `0x08020200`，该轮通过。

第 1 次“载荷写入约 25%”停点已完成真实断电。恢复后 Slot B 与打包镜像相同的连续前缀
为 `3488` 字节，其中头部 `512` 字节、载荷 `2976` 字节，恰为载荷的 `25%`；其余区域
全部为 `0xFF`。Boot Control 的前 128 字节与上一轮 SHA-256 完全相同，测试控制块报告
`app-a-running`、启动地址 `0x08020200`，该轮通过。

第 1 次“载荷写入约 50%”停点已完成真实断电。恢复后 Slot B 与打包镜像相同的连续前缀
为 `6464` 字节，其中载荷 `5952` 字节，恰为载荷的 `50%`，其余区域全部为 `0xFF`。
Boot Control 保持相同 SHA-256，测试控制块报告 `app-a-running`、启动地址 `0x08020200`，
该轮通过。

第 1 次“载荷写入约 90%”停点已完成真实断电。Slot B 与打包镜像相同的连续前缀为
`11228` 字节，其中载荷 `10716` 字节（按 Flash 双字对齐为 `90.02%`），其余区域全部为
`0xFF`，Boot Control 保持相同 SHA-256。首次重新上电因 BOOT0 接触问题进入系统 ROM；
恢复 BOOT0 低电平并重新冷启动后，PC 为 `0x080212DA`、`SYSCFG_MEMRMP=0`，测试控制块
报告 `app-a-running`、启动地址 `0x08020200`，该轮计为通过。

第 1 次“完整镜像验证后、标记 pending 前”停点已完成真实断电。恢复后读回的 Slot B
与打包镜像 SHA-256 完全相同，Boot Control 仍为 `active=A, pending=none`，测试控制块报告
`app-a-running`、启动地址 `0x08020200`。完整但未提交的 B 不会被擅自激活，该轮通过。

第 1 次“pending 控制记录正文写入后、提交字前”停点已完成真实断电。恢复后新记录的
提交字仍为 `0xFFFFFFFF`，128 字节记录区与断电前完全相同；Bootloader 忽略该未提交
记录并启动 Slot A，测试控制块报告 `app-a-running`、启动地址 `0x08020200`，该轮通过。

第 1 次“Slot B 首次启动、确认前”停点已完成真实断电。恢复后测试控制块报告
`app-b-running`、启动地址 `0x08060200`；Boot Control 新增序号 `3` 的有效记录，仍为
pending B，尝试次数从 `1` 增至 `2`，证明未确认镜像在掉电后按策略重试，该轮通过。

随后 IWDG 触发下一次启动，序号 `4` 记录将 pending B 尝试次数增至 `3`。第 1 次“确认
控制记录正文写入后、提交字前”停点已完成真实断电。序号 `5` 的残缺记录正文为
`active=B, pending=none, attempts=0`，提交字保持 `0xFFFFFFFF`；Bootloader 忽略它并在
达到尝试上限后追加有效的序号 `5` 回退记录 `active=A, pending=none, attempts=0`。测试
控制块报告 `app-a-running`、启动地址 `0x08020200`，该轮通过。

首轮九个确定性停点已全部通过。按照清单“每阶段至少 10 次”的门禁，当前计数如下：

| 停点 | 通过次数 | 目标次数 |
| --- | ---: | ---: |
| 目标 slot 擦除后 | 1 | 10 |
| 镜像头写入后 | 1 | 10 |
| 载荷约 25% | 1 | 10 |
| 载荷约 50% | 1 | 10 |
| 载荷约 90% | 1 | 10 |
| 完整镜像验证后、pending 前 | 1 | 10 |
| pending 控制记录提交前 | 1 | 10 |
| Slot B 首次启动、确认前 | 1 | 10 |
| 确认控制记录提交前 | 1 | 10 |

其余掉电重复测试尚未完成，因此当前工作区仍不允许提交或推送远端。
