# E4AP5G1LTS UEFI 调试记录

本文记录 2026-11 对 E4AP5G1LTS（RK3568）固件三项问题的完整排查过程：

1. UEFI 变量无法保存（原实现把变量 dump 到 eMMC，而板子从 SPI NOR 启动）
2. BIOS 界面看不到 NOR Flash / 序列号信息（Board Information 页面不出现）
3. eMMC 无响应时串口日志被 MMC 重试刷屏

对应固件版本：`E4AP5G1LTS_EFI_DEBUG_261108E.itb` / `E4AP5G1LTS_EFI_RELEASE_261108E.itb`。

---

## 1. UEFI 变量落入 SPI NOR Flash

### 现象

在 BIOS 设置里修改 Boot Timeout / BootOrder，Save & Exit 后断电重启，设置丢失。

### 根因

原变量栈工作在 RAM 模拟 NV 模式：

- `PcdFlashNvStorageVariableBase64`、`PcdFlashNvStorageFtwWorkingBase`、
  `PcdFlashNvStorageFtwSpareBase` 全为 0，`VariableRuntimeDxe` 只在 RAM 里维护变量区。
- `VarBlockServiceDxe` 在 `ReadyToBoot` 时把 RAM 变量区 dump 到 boot 设备的 `DiskIo`，
  但 `IsBootDevice` 只识别 SD/eMMC。本板从 SPI NOR 启动，永远找不到目标设备，
  dump 从未发生，于是每次重启都回到默认变量。

结论：需要让变量栈直接工作在 SPI NOR 上。

### SPI 驱动链修复

#### FspiLib（`Silicon/Rockchip/Rk356x/Library/FspiLib`）

原库无法编译：`FspiLib.c` include 了不存在的 `"Soc.h"`、`<Library/Fspi.h>`，
`FSPI_REG` 与移位宏未定义；`pReg->MODE = 0` 写的寄存器在该 IP 上并不存在。

修复内容（寄存器布局参考内核 `drivers/spi/spi-rockchip-sfc.c`）：

- 新增 `Include/IndustryStandard/Rk356xFspi.h`：`FSPI_REG` 寄存器组
  （CTRL/IMR/ICLR/FTLR/RCVR/AX/ABIT/ISR/FSR/SR/RISR/VER/DLL_CTRL0/DMA/LEN_CTRL/LEN_EXT/CMD/ADDR/DATA，
  CS1 组偏移 +0x200）及全部移位宏；该 IP **没有 MODE 寄存器**，相关读写删除。
- `FSPICTRL` 增加 WPEN（bit29），`HAL_FSPI_XferStart` 传输时置位。
- 修正 `FSPI_FIFO_STAT` 中 `txlevel` 位宽 5 → 6。
- `SpiMem.h`：改用 EDK2 类型（`UINT8`/`UINTN` 等）；`HAL_SPI_MEM_OP()` 改为复合字面量，
  使其能用于赋值而不只是初始化。
- `FspiLib.inf`：`Silicon/Rockchip/RockchipPkg.dec` → `Silicon/Rockchip/Rk356x/Rk356x.dec`，
  增加 `PcdLib`。

#### 新驱动 `SpiNorFlashDxe`（`Platform/Rockchip/Rk356x/Drivers/SpiNorFlashDxe`）

参考 Marvell `MvFvbDxe` 的"RAM 影子 + FVB 转发"模式，入口流程：

1. **CRU 配置**（内核 `clk-rk3568.c`）：`CLKGATE_CON(9)` 打开 HCLK_SFC(bit2)/
   SCLK_SFC(bit4)，`CLKSEL_CON(28)` mux 选 xin24m，`SOFTRST_CON(7)` 复位 SFC。
2. **SFC 初始化**：`HAL_FSPI_Init()`，CS0、1-1-1 模式、24 MHz（与 DTS 一致，无需 DLL 训练）。
3. **识别 NOR**：发送 RDID(0x9F) 读 JEDEC ID，查内核 `spi-nor` 的 ID 表得到厂商/型号/容量。
   本板实测 `0xEF6017` → Winbond `W25Q64FWSIG`，8 MiB。未知 ID 时降级为 RAM-only
   （等价于修复前的行为），并打印错误，**不会**阻止启动。
4. **影子内存**：`AllocateAlignedRuntimePages(0x20000, 64KB 对齐)`。
   变量区放在 NOR 顶部 `0x7E0000..0x800000`（可用 `PcdSpiNorVariableBase` 覆盖），布局：

   | 偏移 | 大小 | 用途 |
   | --- | --- | --- |
   | 0x00000 | 0xE000 | 变量 FV |
   | 0x0E000 | 0x1000 | Event log |
   | 0x0F000 | 0x1000 | FTW working |
   | 0x10000 | 0x10000 | FTW spare |

5. **首启播种**：NOR 该区域全 0xFF / FV 头无效时，从 FD 镜像内的默认变量区
   （`0xAE0000`，即 FDF 的 NV region）`CopyMem` 到影子，并一次性擦除+写回 NOR。
6. **PcdSet64S**：把三个 NV 基址 PCD 指向影子。`VariableRuntimeDxe` 对 NV 存储是直接
   `CopyMem` 读取的（`VariableNonVolatile.c`），FTW 还要求 working/spare 与变量区在同一
   FVB 范围内，所以必须让 CPU 可直接读写的影子与 FVB 内容始终一致。
7. **FVB**：BlockSize 0x1000 × 0x20 块；`Write` = WREN(0x06) + PP(0x02) 按 256 字节页写，
   轮询 RDSR WIP，并同步影子；`EraseBlocks` = SE(0x20) + 影子填 0xFF。
   SFC 单次传输上限 64 KiB（`SFC_MAX_IOSIZE_VER4`），驱动内按 `0x10000` 分块。
8. **协议/事件**：同一句柄安装 `gEdkiiSpiNorFlashInfoProtocolGuid`（JEDEC/厂商/型号/容量/
   变量区偏移）；`gImageHandle` 上安装 `gEdkiiNvVarStoreFormattedGuid`；
   SFC MMIO `0xFE300000` 以 UC|RUNTIME 加入 GCD（`SetMemorySpaceAttributes` 延迟到
   `gEfiCpuArchProtocolGuid` 就绪后执行）。

> 说明：本板内核 DTS 没有 sfc 节点，Linux 不会访问该控制器，不存在运行时争用。

### 验证

DEBUG 版串口输出：

```
SpiNorFlashDxe: JEDEC ID 0xef6017
SpiNorFlashDxe: Winbond W25Q64FWSIG, 8 MiB, variable region @0x7e0000
SpiNorFlashDxe: no valid variable store, seeding defaults
SpiNorFlashDxe: variable store seeded to NOR
SpiNorFlashDxe: shadow @0x..., NOR available
```

在 BIOS 中修改设置 → Save → **断电重启** → 设置保留。再次启动时不再打印 seeding。

---

## 2. Board Information 页面不出现

### 现象

`BoardInfoDxe` 已加载，串口能看到：

```
Loading driver at 0x000EFD3A000 EntryPoint=0x... BoardInfoDxe.efi
BoardInfoDxe: serial 3CA28097EFA75F68
BoardInfoDxe: MAC 76:9E:FD:4E:12:CD
BoardInfoDxe: NOR Winbond W25Q64FWSIG, JEDEC 0xEF6017, 8 MiB, var region @0x7E0000
BoardInfoDxe: HII packages added, handle 0xEEFA9798
```

但 BIOS 界面里找不到 "Board Information"。

### 排查过程

1. **确认 HII 注册成功**：`HiiAddPackages()` 返回了有效句柄，说明 FORMS 包和 STRINGS 包
   都进了 HII 数据库。
2. **逐字节校验编译产物**：解码 `Build/.../BoardInfoHii.c` 里的 IFR 数组和 `AutoGen.c` 里的
   字符串包：
   - FORMS 包长度 0x8B；FORM_SET opcode 长度 0x37（= 23 字节固定部分 + 2×16 字节 class GUID）；
   - opcode 链能走到包尾，没有截断或错位；
   - 字符串包 token 与 `BoardInfoDxeStrDefs.h` 定义一一对应（token 3 = "Board Information"）。
3. **结论**：formset 本身完全合法，问题是"没有被 UI 列出"，不是"没有注册"。

### 根因

UiApp 的 ESC 主页在 `UiCustomizeFrontPage()` 中调用
`UiListThirdPartyDrivers(HiiHandle, &gEfiIfrFrontPageGuid, ...)`，其 `RequiredDriver()`
遍历所有 HII 句柄，**只列出 class GUID 列表中包含 `gEfiIfrFrontPageGuid`
（`E58809F8-FBC1-48E2-883A-A30FDC4B441E`）的 formset**。

BoardInfo 的 VFR 原来只声明了自己的 GUID：

```
classguid = BOARD_INFO_FORM_SET_GUID,
```

所以它不在主页列表里。Device Manager 的 device list（`CreateDeviceManagerForm()`）判断条件
不同——只要 class GUID 不是 `gEfiHiiPlatformSetupFormsetGuid` 就会列出——这也是两处行为
容易混淆的地方。

### 修复

`BoardInfoHii.vfr`：

```
classguid = gEfiIfrFrontPageGuid | BOARD_INFO_FORM_SET_GUID,
```

VFR 的 classguid 列表用 `|` 分隔，最多 4 个。`BoardInfoDxe.inf` 的 `[Guids]` 同时增加
`gEfiIfrFrontPageGuid`，否则 VFR 预处理器无法把名字展开成 GUID 字面量。

编译后 IFR 验证：

```
0E B7                          FORM_SET；0xB7 = 长度 0x37 | scope 位
F4A6E5C1 ...                   formset GUID
03 00 04 00                    title/help token
02                              Flags：2 个 class GUID
F8 09 88 E5 C1 FB E2 48 ...    gEfiIfrFrontPageGuid
C1 F4 A6 E5 8D 2B 3A 4E ...    BOARD_INFO_FORM_SET_GUID
```

现在 "Board Information" 直接出现在 **ESC 主页菜单**（与 Boot Manager / Device Manager 并列），
Device Manager 中也仍然可见。

### 调试辅助

`BoardInfoDxe.c` 里加了只在 DEBUG 版编译的自检块（`DEBUG_CODE_BEGIN/END`），模拟 UI 的枚举过程：

```
BoardInfoDxe: 16 HII package lists, ours at index 15
BoardInfoDxe: formset visible at index 15, title 'Board Information', classguid count 2
```

若页面再次消失，这两行可以直接区分"HII 句柄没进库"、"标题取不到"和"classguid 不匹配"。

### 踩过的坑

- `HiiGetFormSetFromHiiHandle()` 的原型在本 edk2 版本中位于
  `MdeModulePkg/Include/Library/HiiLib.h`（MdePkg 没有 `HiiLib.h`）。
- `CompareGuid()` 在 `BaseMemoryLib.h` 中，GCC 14 下隐式声明是**错误**，
  除加 include 外还必须把 `BaseMemoryLib` 写进 INF 的 `[LibraryClasses]`。
- `OtpLib` → `BaseCryptLib` → `RngLib` 会把 `gEfiRngProtocolGuid` 拉进模块 depex，
  而平台默认 `RngLib` 是 `DxeRngLib`。BoardInfoDxe 不做密钥运算，在 DSC 里给该模块单独
  换成 `BaseRngLibNull`，depex 只保留 `SpiNorFlashInfoProtocol AND HiiDatabase`。

### 页面内容

| 项目 | 来源 |
| --- | --- |
| NOR 厂商 / 型号 / JEDEC ID / 容量 / 变量区偏移 | `gEdkiiSpiNorFlashInfoProtocolGuid` |
| 序列号（16 位十六进制） | `OtpGetSerial()`：OTP word 0x07 起 16 字节，双 CRC32（与内核 `/proc/cpuinfo` 一致） |
| MAC 地址 | `OtpGetMacAddress()`：OTP 前 32 字节 SHA256 派生，置本地管理位 |

实测本板：序列号 `3CA28097EFA75F68`，MAC `76:9E:FD:4E:12:CD`。

---

## 3. eMMC 无响应导致日志刷屏

### 现象

串口持续循环输出：

```
MshcSetClock(): ClockFreq = 400000 Hz, Rate = 100000000 Hz, Divider = 125
SendCommand(): EFI_DEVICE_ERROR DWEMMC_RINTSTS=0x100 MmcCmd 0xA0000041(1), Argument 0x40FF8080
SendCommand(): EFI_DEVICE_ERROR DWEMMC_RINTSTS=0x100 MmcCmd 0xA0000145(5), Argument 0x0
SendCommand(): EFI_DEVICE_ERROR DWEMMC_RINTSTS=0x100 MmcCmd 0xA0002148(8), Argument 0x1CE
Not a SD2.0 Card
Card should be MMC
...
```

`RINTSTS=0x100` 是响应超时/无响应。该控制器上的 eMMC 无应答，但 MmcDxe 的
`MmcIdentificationMode()` 对 CMD1/CMD55 最多重试 `MAX_RETRY_COUNT = 1000` 次，
每次失败 `MshcDxe` 都打印一行，串口因此被刷屏，也拖慢启动。

### 修复（`Silicon/Rockchip/Rk356x/Drivers/MshcDxe/MshcDxe.c`）

- 驱动入口完成控制器初始化后**只探测一次**卡：CMD0，然后最多 5 轮 CMD55 / CMD1。
- `SendCommand()` 对连续失败计数 `mConsecutiveErrors`，成功即清零；
  连续 5 次失败（`MSHC_PROBE_RETRIES`）后置 `mNoCard = TRUE` 并打印一次：
  `MshcDxe: 5 consecutive command timeouts, disabling MMC host`
- 锁定后：
  - `MshcIsCardPresent()` 返回 FALSE → MmcDxe 的 `CheckCardsCallback()` 不再触发识别；
  - `MshcSendCommand()` 直接返回 `EFI_NO_MEDIA`；
  - `MshcNotifyState(MmcHwInitializationState)` 返回 `EFI_DEVICE_ERROR`，防止重新初始化。
- 探测本身失败 5 轮也会锁定，并打印
  `MshcDxe: no card response after 5 attempts, disabling MMC host`。

有卡（SD/eMMC）时探测成功立即返回，后续流程与原来一致；只是提前完成了一次硬件初始化。

修复后串口最多出现 5 行 `SendCommand(): EFI_DEVICE_ERROR`，随后一行
`disabling MMC host`，不再循环。

> 注：edk2 是 git 子模块，未修改；所有改动都在 `edk2-rockchip` 内。

---

## 4. 构建与交付

构建环境（Debian，Python 3）：

```sh
export WORKSPACE=$PWD
export PACKAGES_PATH="$PWD/edk2:$PWD/edk2-platforms:$PWD/edk2-non-osi:$PWD/edk2-rockchip"
export GCC5_AARCH64_PREFIX=aarch64-linux-gnu-
export PYTHON_COMMAND=python3
export EDK_TOOLS_PATH="$PWD/edk2/BaseTools"
export CONF_PATH="$PWD/edk2/Conf"
export PATH="$PWD/edk2/BaseTools/BinWrappers/PosixLike:$PATH"

build -n $(getconf _NPROCESSORS_ONLN) -b DEBUG   -a AARCH64 -t GCC5 -p Platform/IndiumEngine/E4AP5G1LTS/E4AP5G1LTS.dsc
build -n $(getconf _NPROCESSORS_ONLN) -b RELEASE -a AARCH64 -t GCC5 -p Platform/IndiumEngine/E4AP5G1LTS/E4AP5G1LTS.dsc
```

FIT 打包（与 `build.sh` 的 `build_fit` 相同）：

```sh
./scripts/extractbl31.py edk2-rockchip-non-osi/rkbin/bin/rk35/rk3568_bl31_v1.32.elf
cat uefi.its | sed "s,@BOARDTYPE@,rk3568-indiumengine-e4ap5g1lts,g" > E4AP5G1LTS_EFI.its
./edk2-rockchip-non-osi/rkbin/tools/mkimage -f E4AP5G1LTS_EFI.its -E E4AP5G1LTS_EFI_DEBUG_261108E.itb
dd if=Build/E4AP5G1LTS/DEBUG_GCC5/FV/RK356X_EFI.fd of=E4AP5G1LTS_EFI_DEBUG_261108E.itb bs=512 seek=2048 conv=notrunc
rm -f bl31_0x*.bin E4AP5G1LTS_EFI.its
```

产物（2 MiB，FD 位于偏移 0x100000，已用 `cmp` 校验）：

| 文件 | md5 |
| --- | --- |
| `E4AP5G1LTS_EFI_DEBUG_261108E.itb` | `c561661ee5c82d42b04626c0fbfdcaca` |
| `E4AP5G1LTS_EFI_RELEASE_261108E.itb` | `5fcb306075a7c3f2a5cdf5411e9711e4` |

空间占用：`FVMAIN_COMPACT`（上限 0xE0000）DEBUG 89%、RELEASE 69%。

---

## 5. 注意事项 / 遗留

- 仅 E4AP5G1LTS 定义 `NOR_VAR_SUPPORT = TRUE`；其它 RK356x 平台继续使用
  `VarBlockServiceDxe`。共享的 `Rk356x.fdf` 中 `!if $(NOR_VAR_SUPPORT) == TRUE`
  在宏未定义时求值为 False，不影响其它板。
- RELEASE 版 `DebugLib` 为 `BaseDebugLibNull`，没有任何串口输出；排查问题请使用 DEBUG 版。
- OTP 原始字节与内核 DTS nvmem cell 的起始偏移不一致（内核 `0x0a` 字节 ≈ OTP word 0x05，
  `OtpGetSerial()` 从 word 0x07 起读）。当前序列号与内核 `/proc/cpuinfo` 输出一致，
  暂不调整；`BoardInfoDxe` 保留 OTP 原始字节打印便于硬件核对。
- `.itb` / `Build/` 在 `.gitignore` 中，不随源码提交。
