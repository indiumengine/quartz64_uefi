# UEFI for Rockchip RK356X based Single Board Computers

This repository contains a port of Tianocore EDK II to various Rockchip RK356x based boards:

* [PINE64 Quartz64 Model A](https://www.pine64.org/quartz64a/) / [PINE64 Quartz64 Model B](https://www.pine64.org/quartz64b/) / [PINE64 SOQuartz](https://wiki.pine64.org/wiki/SOQuartz)
* [Firefly ROC-RK3566-PC](https://en.t-firefly.com/product/industry/rocrk3566pc.html) / [StationPC Station M2](https://www.stationpc.com/product/stationm2)
* [Firefly ROC-RK3568-PC](https://en.t-firefly.com/product/industry/rocrk3568pc.html) / [StationPC Station P2](https://www.stationpc.com/product/stationp2)
* [Radxa ROCK3 Computing Module](https://wiki.radxa.com/Rock3/CM3)
* [Hardkernel ODROID-M1S](https://wiki.odroid.com/odroid-m1s/odroid-m1s)
* IndiumEngine E4AP5G1 / E4AP5G1LTS (RK3568) — added by this fork

## IndiumEngine E4AP5G1 / E4AP5G1LTS

This fork adds UEFI support for the IndiumEngine E4AP5G1 family of RK3568
boards. The two platforms are kept as separate EDK II packages:

| Board | Platform package | Build target | PCIe |
| --- | --- | --- | --- |
| E4AP5G1 | `edk2-rockchip/Platform/IndiumEngine/E4AP5G1` | `E4AP5G1` | enabled |
| E4AP5G1LTS | `edk2-rockchip/Platform/IndiumEngine/E4AP5G1LTS` | `E4AP5G1LTS` | disabled (`PCIE_ENABLE = FALSE`) |

### Repository layout

| Path | Contents |
| --- | --- |
| `edk2/`, `edk2-platforms/`, `edk2-non-osi/` | upstream submodules |
| `edk2-rockchip/` | Rockchip silicon/platform code, including both IndiumEngine platforms |
| `edk2-rockchip-non-osi/` | Rockchip binaries and `rkbin` tools |
| `dtb/` | board device trees packed into the FIT image |
| `uefi.its`, `scripts/extractbl31.py` | FIT template and BL31 extraction helper |
| `build.sh`, `Makefile` | build entry points |

### Building

Tested on Debian/Ubuntu. Required packages:

```sh
sudo apt install build-essential uuid-dev python3 python3-pyelftools \
                 device-tree-compiler iasl gcc-aarch64-linux-gnu
```

Build the release firmware:

```sh
./build.sh RELEASE E4AP5G1LTS
```

Debug firmware (serial output enabled, asserts active):

```sh
./build.sh DEBUG E4AP5G1LTS
```

`build.sh` initialises the submodules, builds EDK II BaseTools once, builds
the platform, packs the FIT image and finally generates `idblock.bin`. The
equivalent Makefile form is `make BOARDS=E4AP5G1LTS TARGET=RELEASE uefi`.

Outputs:

* `E4AP5G1LTS_EFI.itb` — 2 MiB FIT image (BL31, OP-TEE, UEFI FD, board DTB).
* `idblock.bin` — Rockchip loader (DDR init and flash boot, UART 115200).
* `Build/E4AP5G1LTS/<TARGET>_GCC5/FV/RK356X_EFI.fd` — raw UEFI firmware volume.

The SD/eMMC image target (`make sdcard`) produces `E4AP5G1LTS_EFI.img` with
`idblock.bin` at sector 64 (32 KiB) and the FIT image at sector 20480
(10 MiB). Flash that image directly, or program the two files at the same
offsets with the vendor flashing tool.

### Firmware file naming

Archived builds use the following names:

```
E4AP5G1LTS_EFI_RELEASE_<YYMMDD><rev>.itb
E4AP5G1LTS_EFI_DEBUG_<YYMMDD><rev>.itb
```

`<rev>` is a letter incremented for each build of the same day. For example,
`E4AP5G1LTS_EFI_RELEASE_261019B.itb` is the second release build of
2026-10-19.

### Board-specific notes

* **RTC**: RX8025 on I2C1, address `0x32`
  (`PcdRtcI2cBusBase = 0xFE5A0000`, `PcdRtcI2cAddr = 0x32`), implemented by
  `Silicon/Rockchip/Rk356x/Library/Rx8025RtcLib`.
* **Ethernet**: the GMAC MAC address is written into the device tree on every
  boot by `Platform/Rockchip/Rk356x/Drivers/FdtDxe`, so the address stays
  stable across reboots.
* **USB keyboard hotkey**: `BoardInitDxe` registers the short-form USB keyboard
  device path in `ConIn` at EndOfDxe, before BDS connects the USB host
  controllers. Without this, ConPlatform rejects the keyboard during
  enumeration, the USB bus marks the interface as managed, and ESC is never
  delivered at the boot splash (the Caps Lock LED still works). Currently
  implemented for E4AP5G1LTS only.
* **PCIe**: disabled in the E4AP5G1LTS build; the E4AP5G1 build keeps PCIe
  enabled.
* **SPI flash**: `Silicon/Rockchip/Rk356x/Library/FspiLib` provides FSPI
  read/write helpers for the RK3568 SFC controller. On E4AP5G1LTS it is
  consumed by `Platform/Rockchip/Rk356x/Drivers/SpiNorFlashDxe`, which keeps
  the UEFI variable store in the onboard SPI NOR flash (so BIOS settings
  survive power cycles), and exposes the flash ID/model/capacity through
  `gEdkiiSpiNorFlashInfoProtocolGuid`. A "Board Information" page
  (`Platform/IndiumEngine/E4AP5G1LTS/Drivers/BoardInfoDxe`) is listed directly
  on the ESC front page and in the Device Manager, showing the NOR flash
  vendor/model/JEDEC ID/capacity plus the board serial number and MAC address.
  The debugging notes for the SPI NOR variable store, the Board Information
  page and the eMMC retry handling are in
  [`docs/E4AP5G1LTS-debug-notes.md`](docs/E4AP5G1LTS-debug-notes.md) (Chinese).

### Serial console and boot menu

The debug UART is UART2 (`0xFE660000`) at `115200 8n1`, TTYTERM. Press `ESC`
at the `Press ESCAPE for boot options` splash to enter the Boot Manager; this
works from the serial console and from a USB keyboard.

**Note:** release builds map `DebugLib` to `BaseDebugLibNull`, so they produce
no serial debug output. Use a DEBUG build to diagnose boot issues.

## Building an SD card / eMMC image

If you want to build the image, checkout the repository and run:
`$ make sdcard`

Prebuild images are also provided for stable ports and are available in the [release section](https://github.com/jaredmcneill/quartz64_uefi/releases).

**Note:** The ROCK3 Compute Module port is still work in progress: as such no prebuild images are released for those boards.

## Installing

In order to have the board booting in UEFI, the firmware has to be located in one of the supported boot devices. If the device (SD card, eMMC) are removable, the easiest way is to use `dd`, [Etcher](https://www.balena.io/etcher), etc. to write the appropriate .img file to the device.

### Board specific installation guides

* [Firefly ROC-RK3566-PC and ROC-RK3568-PC, StationPC Station M2 and P2](docs/firefly-ROC-RK356x-PC.md)

## Running

Connect a serial console to UART2 using settings `115200 8n1`.

## Operating system support

| OS | Version | Supported hardware | Notes |
| --- | --- | --- | --- |
| ESXi-Arm | 1.12 | HDMI, USB2, USB3, serial, PCIe, ethernet | |
| Fedora | 38 | HDMI, USB2, USB3, serial, PCIe, thermal sensors | Needs `irqchip.gicv3_nolpi=1` for MSI support |
| FreeBSD | 14.0-CURRENT | ? | Mangled serial output, boot stuck waiting for random seed |
| NetBSD | 9.99.x | HDMI, USB2, USB3, serial, SD card, PCIe, eMMC, SATA, ethernet, thermal sensors, watchdog | |
| OpenBSD | 7.0-current | HDMI, USB2, USB3, serial | To use HDMI console, enter `set tty fb0` at the bootloader prompt. |
| Ubuntu | 21.04 | HDMI, USB2, USB3, serial, PCIe, thermal sensors | Needs `irqchip.gicv3_nolpi=1` for MSI support |
| Windows PE | ? | HDMI, USB3, PCIe | BSOD when plugging device in to USB2 port (#2) |

## eMMC controller Device-Specific Method (_DSM)

The divider register in the RK356x's SDHCI controller does not work, so to change the eMMC card clock we have to do it through the CRU. This is exposed to the OS as an ACPI Device-Specific Method (_DSM).

### Function 1: Set Card Clock

The _DSM control method parameters for the Set Card Clock function are as follows:

#### Arguments

 - **Arg0**: UUID = 434addb0-8ff3-49d5-a724-95844b79ad1f
 - **Arg1**: Revision = 0
 - **Arg2**: Function Index = 1
 - **Arg3**: Target card clock rate in Hz.

#### Return

The actual card clock rate in Hz. Will be less than or equal to the target clock rate. Returns 0 if the target clock rate could not be set.
