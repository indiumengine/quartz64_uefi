/** @file
 *
 *  SPI NOR flash support for the UEFI variable store.
 *
 *  The RK356x SFC controller is driven through the (repaired) FspiLib HAL
 *  using plain 1-1-1 commands at 24 MHz (xin24m), matching the board device
 *  tree. Chips larger than 16 MB use the stateless 4-byte opcodes so no
 *  non-volatile EN4B mode bit has to be touched.
 *
 *  Copyright (c) 2026, IndiumEngine
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <IndustryStandard/Rk356x.h>
#include <IndustryStandard/Rk356xCru.h>
#include <IndustryStandard/Rk356xFspi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/CruLib.h>
#include <Library/SpiMem.h>
#include <Library/FspiLib.h>
#include <Protocol/Cpu.h>
#include <Protocol/SpiNorFlashInfo.h>

#include "SpiNorFlashDxe.h"

//
// SPI NOR opcodes (1-1-1). 4-byte addressing variants are stateless.
//
#define SPINOR_OP_RDID      0x9F
#define SPINOR_OP_RDSR      0x05
#define SPINOR_OP_WREN      0x06
#define SPINOR_OP_READ      0x03
#define SPINOR_OP_PP        0x02
#define SPINOR_OP_SE        0x20
#define SPINOR_OP_BE        0xD8
#define SPINOR_OP_READ_4B   0x13
#define SPINOR_OP_PP_4B     0x12
#define SPINOR_OP_SE_4B     0x21
#define SPINOR_OP_BE_4B     0xDC

#define SPINOR_WIP          BIT0
#define SPINOR_WIP_TIMEOUT  (3 * 1000 * 1000)  // us
#define SPINOR_PAGE_SIZE    0x100

//
// The SFC controller must not be asked for more than 64KB of data in a
// single operation (kernel spi-rockchip-sfc.c: SFC_MAX_IOSIZE_VER4).
// Larger transfers are split into chunks by the upper layer there; the
// HAL used here does not split, so do it in the driver.
//
#define SPINOR_MAX_XFER     0x10000

#define SFC_MMIO_SIZE       SIZE_64KB

//
// SFC clock and reset bits (kernel clk-rk3568.c):
//   HCLK_SFC        CLKGATE_CON(9) bit 2, SRST_CON(7) bit 2
//   HCLK_SFC_XIP    CLKGATE_CON(9) bit 3, SRST_CON(7) bit 3
//   SCLK_SFC        CLKSEL_CON(28) mux <6:4>, CLKGATE_CON(9) bit 4,
//                   SRST_CON(7) bit 4
//
#define SFC_CLKGATE_CON        9
#define SFC_CLKGATE_HCLK       2
#define SFC_CLKGATE_HCLK_XIP   3
#define SFC_CLKGATE_SCLK       4
#define SFC_SOFTRST_CON        7
#define SFC_SOFTRST_HCLK       2
#define SFC_SOFTRST_HCLK_XIP   3
#define SFC_SOFTRST_SCLK       4
#define SFC_CLKSEL_CON         28
#define SFC_CLKSEL_SCLK_SHIFT  4
#define SFC_CLKSEL_SCLK_MASK   (0x7U << SFC_CLKSEL_SCLK_SHIFT)
#define SFC_SCLK_XIN24M        0

//
// Total size of the NV region (var + event log + FTW working + FTW spare),
// as laid out by Rk356x.fdf.
//
#define NV_TOTAL_SIZE  (FixedPcdGet32 (PcdFlashNvStorageVariableSize) + \
                        FixedPcdGet32 (PcdNvStorageEventLogSize) + \
                        FixedPcdGet32 (PcdFlashNvStorageFtwWorkingSize) + \
                        FixedPcdGet32 (PcdFlashNvStorageFtwSpareSize))
#define NV_SHADOW_ALIGN  SIZE_64KB

typedef struct {
  UINT32    JedecId;
  CHAR8     *Vendor;
  CHAR8     *Model;
  UINT32    Capacity;
} SPI_NOR_DEVICE;

STATIC SPI_NOR_DEVICE  mSpiNorTable[] = {
  { 0x0B4017, "XTX",       "XT25F64F",    8 * 1024 * 1024  },
  { 0x0B4018, "XTX",       "XT25F128B",   16 * 1024 * 1024 },
  { 0x0B4019, "XTX",       "XT25F256B",   32 * 1024 * 1024 },
  { 0x0B6017, "XTX",       "XT25Q64D",    8 * 1024 * 1024  },
  { 0x0B6018, "XTX",       "XT25Q128D",   16 * 1024 * 1024 },
  { 0x0B6019, "XTX",       "XT25Q256F",   32 * 1024 * 1024 },
  { 0x1C3817, "EON",       "EN25S64",     8 * 1024 * 1024  },
  { 0x1C7018, "EON",       "EN25QH128",   16 * 1024 * 1024 },
  { 0x1C7019, "EON",       "EN25QH256A",  32 * 1024 * 1024 },
  { 0x1C7117, "EON",       "EN25QX64A",   8 * 1024 * 1024  },
  { 0x1C7118, "EON",       "EN25QX128A",  16 * 1024 * 1024 },
  { 0x1C7119, "EON",       "EN25QX256A",  32 * 1024 * 1024 },
  { 0x204017, "XMC",       "XM25QH64C",   8 * 1024 * 1024  },
  { 0x204018, "XMC",       "XM25QH128C",  16 * 1024 * 1024 },
  { 0x204019, "XMC",       "XM25QH256C",  32 * 1024 * 1024 },
  { 0x206018, "XMC",       "XM25QH128B",  16 * 1024 * 1024 },
  { 0x207017, "XMC",       "XM25QH64A",   8 * 1024 * 1024  },
  { 0x207018, "XMC",       "XM25QH128A",  16 * 1024 * 1024 },
  { 0x684017, "Boya",      "BY25Q64ES",   8 * 1024 * 1024  },
  { 0x684919, "Boya",      "BY25Q256FS",  32 * 1024 * 1024 },
  { 0x852017, "Puya",      "PY25Q64HA",   8 * 1024 * 1024  },
  { 0x852018, "Puya",      "PY25Q128HA",  16 * 1024 * 1024 },
  { 0x852019, "Puya",      "PY25Q256HB",  32 * 1024 * 1024 },
  { 0x856017, "Puya",      "P25Q64H",     8 * 1024 * 1024  },
  { 0x856018, "Puya",      "P25Q128H",    16 * 1024 * 1024 },
  { 0x8C4117, "ESMT",      "F25L64QA",    8 * 1024 * 1024  },
  { 0x9D7019, "ISSI",      "IS25WP256",   32 * 1024 * 1024 },
  { 0xA14017, "FudanMicro","FM25Q64A",    8 * 1024 * 1024  },
  { 0xA14018, "FudanMicro","FM25Q128A",   16 * 1024 * 1024 },
  { 0xA14019, "FudanMicro","FM25Q256I3",  32 * 1024 * 1024 },
  { 0xC22018, "Macronix",  "MX25L12805D", 16 * 1024 * 1024 },
  { 0xC22019, "Macronix",  "MX25L25635E", 32 * 1024 * 1024 },
  { 0xC22538, "Macronix",  "MX25U12835F", 16 * 1024 * 1024 },
  { 0xC22539, "Macronix",  "MX25U25635F", 32 * 1024 * 1024 },
  { 0xC84017, "GigaDevice","GD25Q64",     8 * 1024 * 1024  },
  { 0xC84018, "GigaDevice","GD25Q128",    16 * 1024 * 1024 },
  { 0xC84019, "GigaDevice","GD25Q256",    32 * 1024 * 1024 },
  { 0xC84020, "GigaDevice","GD25Q512",    64 * 1024 * 1024 },
  { 0xEF4017, "Winbond",   "W25Q64JV",    8 * 1024 * 1024  },
  { 0xEF4018, "Winbond",   "W25Q128JV",   16 * 1024 * 1024 },
  { 0xEF4019, "Winbond",   "W25Q256JV",   32 * 1024 * 1024 },
  { 0xEF6017, "Winbond",   "W25Q64FWSIG", 8 * 1024 * 1024  },
  { 0xF83217, "Dosilicon", "FM25Q64A",    8 * 1024 * 1024  },
  { 0xF84218, "Dosilicon", "FM25M4AA",    16 * 1024 * 1024 },
};

EFI_FW_VOL_INSTANCE  *mFvInstance = NULL;

STATIC SPI_NOR_DEVICE            *mNorDevice = NULL;
STATIC UINT32                    mNorVarOffset = 0;
EDKII_SPI_NOR_FLASH_INFO_PROTOCOL  mNorInfo = {
  EDKII_SPI_NOR_FLASH_INFO_PROTOCOL_REVISION,
  FALSE,
  { 0, 0, 0 },
  0,
  0,
  0,
  L"",
  L"",
};

struct HAL_FSPI_HOST  mHost = {
  NULL,
  (UINT32 *)(UINTN)CRU_BASE,
  0,
  HAL_SPI_MODE_1,   /* sps = PHASE_SEL_NEGETIVE, as in kernel/u-boot */
  0,
};

//
// SFC clock/reset bring-up, mirroring kernel clk-rk3568.c.
//
STATIC
VOID
SfcClkInit (
  VOID
  )
{
  CruAssertSoftReset (SFC_SOFTRST_CON, SFC_SOFTRST_HCLK);
  CruAssertSoftReset (SFC_SOFTRST_CON, SFC_SOFTRST_HCLK_XIP);
  CruAssertSoftReset (SFC_SOFTRST_CON, SFC_SOFTRST_SCLK);

  CruEnableClock (SFC_CLKGATE_CON, SFC_CLKGATE_HCLK);
  CruEnableClock (SFC_CLKGATE_CON, SFC_CLKGATE_HCLK_XIP);
  CruEnableClock (SFC_CLKGATE_CON, SFC_CLKGATE_SCLK);

  /* Select xin24m for SCLK_SFC (matches the board device tree). */
  MmioWrite32 (
    CRU_CLKSEL_CON (SFC_CLKSEL_CON),
    (SFC_CLKSEL_SCLK_MASK << 16) | (SFC_SCLK_XIN24M << SFC_CLKSEL_SCLK_SHIFT)
    );

  CruDeassertSoftReset (SFC_SOFTRST_CON, SFC_SOFTRST_HCLK);
  CruDeassertSoftReset (SFC_SOFTRST_CON, SFC_SOFTRST_HCLK_XIP);
  CruDeassertSoftReset (SFC_SOFTRST_CON, SFC_SOFTRST_SCLK);
}

STATIC
BOOLEAN
SpiNorNeed4Byte (
  VOID
  )
{
  /*
   * mNorInfo.Capacity is a plain value in a static struct and is accessed
   * PC-relative, so it stays valid after SetVirtualAddressMap. Do not
   * dereference mNorDevice here: it is a runtime-assigned pointer to a
   * static table and would still hold its physical address in virtual mode.
   */
  return mNorInfo.Capacity > (16U * 1024U * 1024U);
}

STATIC
EFI_STATUS
SpiNorWaitReady (
  VOID
  )
{
  UINT8   Status = SPINOR_WIP;
  UINT32  Timeout = SPINOR_WIP_TIMEOUT;
  struct HAL_SPI_MEM_OP Op = HAL_SPI_MEM_OP (
                                HAL_SPI_MEM_OP_CMD (SPINOR_OP_RDSR, 1),
                                HAL_SPI_MEM_OP_NO_ADDR,
                                HAL_SPI_MEM_OP_NO_DUMMY,
                                HAL_SPI_MEM_OP_DATA_IN (1, &Status, 1)
                                );

  while ((Status & SPINOR_WIP) && (Timeout > 0)) {
    MicroSecondDelay (100);
    Timeout -= 100;
    if (EFI_ERROR ((EFI_STATUS)HAL_FSPI_SpiXfer (&mHost, &Op))) {
      return EFI_DEVICE_ERROR;
    }
  }

  return (Status & SPINOR_WIP) ? EFI_TIMEOUT : EFI_SUCCESS;
}

EFI_STATUS
SpiNorReadId (
  OUT UINT8  *JedecId
  )
{
  struct HAL_SPI_MEM_OP Op = HAL_SPI_MEM_OP (
                                HAL_SPI_MEM_OP_CMD (SPINOR_OP_RDID, 1),
                                HAL_SPI_MEM_OP_NO_ADDR,
                                HAL_SPI_MEM_OP_NO_DUMMY,
                                HAL_SPI_MEM_OP_DATA_IN (3, JedecId, 1)
                                );

  return (EFI_STATUS)HAL_FSPI_SpiXfer (&mHost, &Op);
}

EFI_STATUS
SpiNorRead (
  IN  UINT32  Offset,
  IN  UINT32  Length,
  OUT VOID    *Buffer
  )
{
  EFI_STATUS            Status;
  UINT8                 *Dst       = Buffer;
  UINT8                 AddrBytes  = SpiNorNeed4Byte () ? 4 : 3;

  while (Length > 0) {
    UINT32                Chunk = MIN (Length, SPINOR_MAX_XFER);
    struct HAL_SPI_MEM_OP Op    = HAL_SPI_MEM_OP (
                                    HAL_SPI_MEM_OP_CMD (
                                      SpiNorNeed4Byte () ? SPINOR_OP_READ_4B : SPINOR_OP_READ,
                                      1
                                      ),
                                    HAL_SPI_MEM_OP_ADDR (AddrBytes, Offset, 1),
                                    HAL_SPI_MEM_OP_NO_DUMMY,
                                    HAL_SPI_MEM_OP_DATA_IN (Chunk, Dst, 1)
                                    );

    Status = (EFI_STATUS)HAL_FSPI_SpiXfer (&mHost, &Op);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Dst    += Chunk;
    Offset += Chunk;
    Length -= Chunk;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SpiNorWriteEnable (
  VOID
  )
{
  struct HAL_SPI_MEM_OP Op = HAL_SPI_MEM_OP (
                                HAL_SPI_MEM_OP_CMD (SPINOR_OP_WREN, 1),
                                HAL_SPI_MEM_OP_NO_ADDR,
                                HAL_SPI_MEM_OP_NO_DUMMY,
                                HAL_SPI_MEM_OP_NO_DATA
                                );

  return (EFI_STATUS)HAL_FSPI_SpiXfer (&mHost, &Op);
}

EFI_STATUS
SpiNorWrite (
  IN UINT32        Offset,
  IN UINT32        Length,
  IN CONST VOID    *Buffer
  )
{
  EFI_STATUS            Status;
  CONST UINT8           *Src = Buffer;
  struct HAL_SPI_MEM_OP Op;
  UINT8                 AddrBytes = SpiNorNeed4Byte () ? 4 : 3;

  while (Length > 0) {
    UINT32  Chunk = MIN (Length, SPINOR_PAGE_SIZE);

    /* Page program must not cross a 256-byte page boundary. */
    Chunk = MIN (Chunk, SPINOR_PAGE_SIZE - (Offset & (SPINOR_PAGE_SIZE - 1)));

    Op = HAL_SPI_MEM_OP (
           HAL_SPI_MEM_OP_CMD (SpiNorNeed4Byte () ? SPINOR_OP_PP_4B : SPINOR_OP_PP, 1),
           HAL_SPI_MEM_OP_ADDR (AddrBytes, Offset, 1),
           HAL_SPI_MEM_OP_NO_DUMMY,
           HAL_SPI_MEM_OP_DATA_OUT (Chunk, (VOID *)Src, 1)
           );

    Status = SpiNorWriteEnable ();
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = (EFI_STATUS)HAL_FSPI_SpiXfer (&mHost, &Op);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = SpiNorWaitReady ();
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Src    += Chunk;
    Offset += Chunk;
    Length -= Chunk;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
SpiNorErase (
  IN UINT32  Offset,
  IN UINT32  Length
  )
{
  EFI_STATUS            Status;
  struct HAL_SPI_MEM_OP Op;
  UINT8                 AddrBytes = SpiNorNeed4Byte () ? 4 : 3;

  while (Length > 0) {
    UINT32  EraseLen;
    UINT8   Opcode;

    /*
     * Prefer 64KB block erase over 4KB sector erase when the range
     * covers a whole 64KB-aligned block (e.g. the FTW spare region).
     */
    if (((Offset & (64 * 1024 - 1)) == 0) && (Length >= 64 * 1024)) {
      Opcode   = SpiNorNeed4Byte () ? SPINOR_OP_BE_4B : SPINOR_OP_BE;
      EraseLen = 64 * 1024;
    } else {
      Opcode   = SpiNorNeed4Byte () ? SPINOR_OP_SE_4B : SPINOR_OP_SE;
      EraseLen = 4 * 1024;
    }

    Op = HAL_SPI_MEM_OP (
           HAL_SPI_MEM_OP_CMD (Opcode, 1),
           HAL_SPI_MEM_OP_ADDR (AddrBytes, Offset, 1),
           HAL_SPI_MEM_OP_NO_DUMMY,
           HAL_SPI_MEM_OP_NO_DATA
           );

    Status = SpiNorWriteEnable ();
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = (EFI_STATUS)HAL_FSPI_SpiXfer (&mHost, &Op);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = SpiNorWaitReady ();
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Offset  += EraseLen;
    Length  -= EraseLen;
  }

  return EFI_SUCCESS;
}

VOID
SpiNorVariableRegionChanged (
  VOID
  )
{
  /*
   * The 4-byte addressing decision is fixed by the flash geometry, not by
   * the variable region, so there is nothing to reconfigure here. Kept as
   * a hook in case a flash with a non-volatile EN4B bit is added later.
   */
}

STATIC VOID  *mCpuArchRegistration = NULL;

/**
  Mark the SFC MMIO range as runtime uncacheable memory.

  This cannot be done from the entry point: the core delegates the page
  table update to the CPU arch protocol and returns EFI_NOT_AVAILABLE_YET
  while CpuDxe has not been dispatched. The range is nevertheless usable
  before then because the platform's initial translation tables map the
  whole 0xF0000000-0xFFFFFFFF window as device memory, so the flash is
  fully functional while this event is pending.
**/
STATIC
VOID
EFIAPI
SpiNorCpuArchNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;
  VOID        *CpuArch;

  /*
   * EfiCreateProtocolNotifyEvent() signals the event once immediately to
   * cover protocols that are already installed; bail out quietly until
   * CpuDxe really shows up.
   */
  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, &CpuArch);
  if (EFI_ERROR (Status)) {
    return;
  }

  Status = gDS->SetMemorySpaceAttributes (
                  FSPI_BASE,
                  SFC_MMIO_SIZE,
                  EFI_MEMORY_UC | EFI_MEMORY_RUNTIME
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SpiNorFlashDxe: SetMemorySpaceAttributes failed: %r\n", Status));
  } else {
    DEBUG ((DEBUG_INFO, "SpiNorFlashDxe: SFC MMIO mapped UC|RUNTIME\n"));
  }

  gBS->CloseEvent (Event);
  mCpuArchRegistration = NULL;
}

EFI_STATUS
EFIAPI
SpiNorFlashDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS            Status;
  RETURN_STATUS         PcdStatus;
  UINTN                 Index;
  UINT8                 JedecId[3];
  UINT32                JedecValue;
  UINT32                NorVarBase;
  VOID                  *Shadow;
  EFI_FIRMWARE_VOLUME_HEADER  *FvHeader;
  EFI_EVENT             CpuArchEvent;
  BOOLEAN               Seeded = FALSE;

  SfcClkInit ();

  /*
   * Register the SFC MMIO range in the GCD. Applying the UC|RUNTIME
   * attributes needs the CPU arch protocol (gDS returns
   * EFI_NOT_AVAILABLE_YET until CpuDxe runs), so that part is deferred
   * to SpiNorCpuArchNotify().
   */
  Status = gDS->AddMemorySpace (
                  EfiGcdMemoryTypeMemoryMappedIo,
                  FSPI_BASE,
                  SFC_MMIO_SIZE,
                  EFI_MEMORY_UC | EFI_MEMORY_RUNTIME
                  );
  if (EFI_ERROR (Status) && (Status != EFI_ACCESS_DENIED)) {
    DEBUG ((DEBUG_ERROR, "SpiNorFlashDxe: AddMemorySpace failed: %r\n", Status));
  }

  CpuArchEvent = EfiCreateProtocolNotifyEvent (
                   &gEfiCpuArchProtocolGuid,
                   TPL_CALLBACK,
                   SpiNorCpuArchNotify,
                   NULL,
                   &mCpuArchRegistration
                   );
  if (CpuArchEvent == NULL) {
    DEBUG ((DEBUG_ERROR, "SpiNorFlashDxe: failed to register CPU arch notify event\n"));
  }

  mHost.instance = (struct FSPI_REG *)(UINTN)FSPI_BASE;
  if (EFI_ERROR ((EFI_STATUS)HAL_FSPI_Init (&mHost))) {
    DEBUG ((DEBUG_ERROR, "SpiNorFlashDxe: FSPI init failed, falling back to RAM-only variables\n"));
  } else {
    /*
     * Identify the flash. Unknown IDs fall back to RAM-only variables
     * (equivalent to the previous VarBlockServiceDxe behaviour).
     */
    Status = SpiNorReadId (JedecId);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "SpiNorFlashDxe: RDID failed: %r\n", Status));
    } else {
      JedecValue = (JedecId[0] << 16) | (JedecId[1] << 8) | JedecId[2];
      DEBUG ((DEBUG_INFO, "SpiNorFlashDxe: JEDEC ID 0x%06x\n", JedecValue));

      for (Index = 0; Index < ARRAY_SIZE (mSpiNorTable); Index++) {
        if (mSpiNorTable[Index].JedecId == JedecValue) {
          mNorDevice = &mSpiNorTable[Index];
          break;
        }
      }

      if (mNorDevice != NULL) {
        mNorInfo.NorAvailable = TRUE;
        mNorInfo.JedecId[0]   = JedecId[0];
        mNorInfo.JedecId[1]   = JedecId[1];
        mNorInfo.JedecId[2]   = JedecId[2];
        mNorInfo.Capacity     = mNorDevice->Capacity;
        AsciiStrToUnicodeStrS (mNorDevice->Vendor, mNorInfo.Vendor, ARRAY_SIZE (mNorInfo.Vendor));
        AsciiStrToUnicodeStrS (mNorDevice->Model, mNorInfo.Model, ARRAY_SIZE (mNorInfo.Model));

        /*
         * Variables live in the last NV_TOTAL_SIZE bytes of the flash;
         * PcdSpiNorVariableBase can pin a different offset.
         */
        NorVarBase = FixedPcdGet32 (PcdSpiNorVariableBase);
        if ((NorVarBase == 0) || (NorVarBase > mNorDevice->Capacity - NV_TOTAL_SIZE)) {
          NorVarBase = mNorDevice->Capacity - NV_TOTAL_SIZE;
        }

        mNorVarOffset                  = NorVarBase;
        mNorInfo.VariableRegionOffset  = NorVarBase;

        DEBUG ((DEBUG_INFO,
          "SpiNorFlashDxe: %a %a, %d MiB, variable region @0x%x\n",
          mNorDevice->Vendor, mNorDevice->Model,
          mNorDevice->Capacity / 1024 / 1024, NorVarBase));
      } else {
        DEBUG ((DEBUG_ERROR,
          "SpiNorFlashDxe: unknown NOR flash, falling back to RAM-only variables\n"));
      }
    }
  }

  /*
   * Allocate the runtime shadow. The region is 64KB aligned so that the
   * FTW spare space at +0x10000 is aligned as well.
   */
  mFvInstance = AllocateRuntimeZeroPool (sizeof (EFI_FW_VOL_INSTANCE));
  ASSERT (mFvInstance != NULL);
  Shadow = AllocateAlignedRuntimePages (
             EFI_SIZE_TO_PAGES (NV_TOTAL_SIZE),
             NV_SHADOW_ALIGN
             );
  if (Shadow == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  mFvInstance->FvBase       = (UINTN)Shadow;
  mFvInstance->FvLength     = NV_TOTAL_SIZE;
  mFvInstance->NorAvailable = mNorInfo.NorAvailable;

  /*
   * Load the variable region from NOR, or seed it from the defaults baked
   * into the firmware image (Rk356x.fdf NV region) on first boot.
   */
  FvHeader = (EFI_FIRMWARE_VOLUME_HEADER *)Shadow;
  if (mFvInstance->NorAvailable) {
    Status = SpiNorRead (mNorVarOffset, NV_TOTAL_SIZE, Shadow);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "SpiNorFlashDxe: NOR read failed: %r\n", Status));
      mFvInstance->NorAvailable = FALSE;
    }
  }

  Status = ValidateFvHeader (FvHeader);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "SpiNorFlashDxe: no valid variable store, seeding defaults\n"));
    CopyMem (
      Shadow,
      (VOID *)(UINTN)(FixedPcdGet32 (PcdNvStorageVariableBase)),
      NV_TOTAL_SIZE
      );

    if (mFvInstance->NorAvailable) {
      Status = SpiNorErase (mNorVarOffset, NV_TOTAL_SIZE);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "SpiNorFlashDxe: seed erase failed: %r\n", Status));
      } else {
        Status = SpiNorWrite (mNorVarOffset, NV_TOTAL_SIZE, Shadow);
        if (EFI_ERROR (Status)) {
          DEBUG ((DEBUG_ERROR, "SpiNorFlashDxe: seed write failed: %r\n", Status));
        } else {
          DEBUG ((DEBUG_INFO, "SpiNorFlashDxe: variable store seeded to NOR\n"));
          Seeded = TRUE;
        }
      }
    }

    Status = ValidateFvHeader (FvHeader);
    ASSERT_EFI_ERROR (Status);
  }

  /*
   * Point the NV storage PCDs at the shadow. VariableRuntimeDxe reads
   * these PCDs after our FVB protocol shows up, and all of its CPU
   * accesses to the store hit the shadow directly.
   */
  PcdStatus = PcdSet64S (PcdFlashNvStorageVariableBase64, (UINTN)Shadow);
  ASSERT_RETURN_ERROR (PcdStatus);
  PcdStatus = PcdSet32S (PcdFlashNvStorageFtwWorkingBase,
    (UINT32)((UINTN)Shadow +
      FixedPcdGet32 (PcdNvStorageFtwWorkingBase) - FixedPcdGet32 (PcdNvStorageVariableBase)));
  ASSERT_RETURN_ERROR (PcdStatus);
  PcdStatus = PcdSet32S (PcdFlashNvStorageFtwSpareBase,
    (UINT32)((UINTN)Shadow +
      FixedPcdGet32 (PcdNvStorageFtwSpareBase) - FixedPcdGet32 (PcdNvStorageVariableBase)));
  ASSERT_RETURN_ERROR (PcdStatus);

  Status = FvbInitialize (ImageHandle, SystemTable);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /*
   * Tell the world the NV variable store is formatted.
   */
  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gEdkiiNvVarStoreFormattedGuid,
                  EFI_NATIVE_INTERFACE,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  if (Seeded) {
    DEBUG ((DEBUG_INFO, "SpiNorFlashDxe: seeded NV variable store at 0x%x\n", mNorVarOffset));
  }
  DEBUG ((DEBUG_INFO, "SpiNorFlashDxe: shadow @0x%lx, NOR %s\n",
    (UINTN)Shadow, mFvInstance->NorAvailable ? "available" : "unavailable (RAM-only)"));

  return EFI_SUCCESS;
}
