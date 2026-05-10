/** @file
 *
 *  Copyright (c) 2026, Jack Pan <panxingyu@indiumengine.com>
 *  Copyright (c) 2021, Jared McNeill <jmcneill@invisible.ca>
 *  Copyright (c) 2017,2021 Andrey Warkentin <andrey.warkentin@gmail.com>
 *  Copyright (c) 2016, Linaro, Ltd. All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/
#include <PiDxe.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/OtpLib.h>
#include <IndustryStandard/Rk356x.h>
#include <libfdt.h>
#include <Guid/Fdt.h>
#include <ConfigVars.h>

STATIC VOID *mFdtImage;

STATIC
EFI_STATUS
CleanMemoryNodes (
  VOID
  )
{
  INTN Node;
  INT32 Retval;

  Node = fdt_path_offset (mFdtImage, "/memory");
  if (Node < 0) {
    return EFI_SUCCESS;
  }

  /*
   * Remove bogus memory nodes which can make the booted
   * OS go crazy and ignore the UEFI map.
   */
  DEBUG ((DEBUG_INFO, "Removing bogus /memory\n"));
  Retval = fdt_del_node (mFdtImage, Node);
  if (Retval != 0) {
    DEBUG ((DEBUG_ERROR, "Failed to remove /memory\n"));
    return EFI_NOT_FOUND;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
FixUartSpeed (
  VOID
  )
{
  INTN Node;
  const char *StdOutPath;
  char Buf[80];
  UINT8 UartIndex;

  Node = fdt_path_offset (mFdtImage, "/chosen");
  if (Node < 0) {
    return EFI_SUCCESS;
  }

  StdOutPath = fdt_getprop (mFdtImage, Node, "stdout-path", NULL);
  if (StdOutPath == NULL) {
    return EFI_SUCCESS;
  }

  if (FixedPcdGet64 (PcdSerialRegisterBase) == UART_BASE (0)) {
    UartIndex = 0;
  } else {
    UartIndex = 1 + (FixedPcdGet64 (PcdSerialRegisterBase) - UART_BASE (1)) / 0x10000;
  }
  AsciiSPrint (Buf, sizeof (Buf), "serial%u:%lun8", UartIndex, FixedPcdGet64 (PcdUartDefaultBaudRate));
  fdt_setprop_string (mFdtImage, Node, "stdout-path", Buf);

  return EFI_SUCCESS;
}

#ifdef QUARTZ64
STATIC
EFI_STATUS
FixMultiPhy1Mode (
  VOID
  )
{
  INTN Node;

  Node = fdt_path_offset (mFdtImage, "/sata@fc400000");
  if (Node < 0) {
    DEBUG ((DEBUG_ERROR, "Node /sata@fc400000 not found"));
    return EFI_SUCCESS;
  }
  fdt_setprop_string (mFdtImage, Node, "status",
                      PcdGet32 (PcdMultiPhy1Mode) == MULTIPHY_MODE_SEL_SATA ? "okay" : "disabled");

  Node = fdt_path_offset (mFdtImage, "/usb@fd000000");
  if (Node < 0) {
    DEBUG ((DEBUG_ERROR, "Node /usb@fd000000 not found"));
    return EFI_SUCCESS;
  }
  fdt_setprop_string (mFdtImage, Node, "status",
                      PcdGet32 (PcdMultiPhy1Mode) == MULTIPHY_MODE_SEL_USB3 ? "okay" : "disabled");

  return EFI_SUCCESS;
}
#endif

// 将 MacLo/MacHi 转换为 6 字节数组（小端序）
STATIC VOID MacLoHiToBytes(IN UINT32 MacLo, IN UINT32 MacHi, OUT UINT8 MacAddress[6]) {
    MacAddress[0] = (UINT8)(MacLo >> 0);
    MacAddress[1] = (UINT8)(MacLo >> 8);
    MacAddress[2] = (UINT8)(MacLo >> 16);
    MacAddress[3] = (UINT8)(MacLo >> 24);
    MacAddress[4] = (UINT8)(MacHi >> 0);
    MacAddress[5] = (UINT8)(MacHi >> 8);
}

// 向设备树节点添加 MAC 地址
STATIC EFI_STATUS AddMacToFdtNode(IN VOID *FdtBlob, IN CHAR8 *NodePath, IN UINT8 MacAddress[6]) {
    INT32 NodeOffset;
    INT32 Ret;

    NodeOffset = fdt_path_offset(FdtBlob, NodePath);
    if (NodeOffset < 0) {
        DEBUG((DEBUG_WARN, "FDT: Node %a not found (error=%d)\n", NodePath, NodeOffset));
        return EFI_NOT_FOUND;
    }

    // 删除已存在的属性
    if (fdt_getprop(FdtBlob, NodeOffset, "mac-address", NULL)) {
        fdt_delprop(FdtBlob, NodeOffset, "mac-address");
    }
    if (fdt_getprop(FdtBlob, NodeOffset, "local-mac-address", NULL)) {
        fdt_delprop(FdtBlob, NodeOffset, "local-mac-address");
    }

    // 添加新属性
    Ret = fdt_setprop(FdtBlob, NodeOffset, "mac-address", MacAddress, 6);
    if (Ret != 0) {
        DEBUG((DEBUG_ERROR, "FDT: Failed to set mac-address for %a, error=%d\n", NodePath, Ret));
        return EFI_DEVICE_ERROR;
    }
    Ret = fdt_setprop(FdtBlob, NodeOffset, "local-mac-address", MacAddress, 6);
    if (Ret != 0) {
        DEBUG((DEBUG_WARN, "FDT: Failed to set local-mac-address for %a\n", NodePath));
    }

    DEBUG((DEBUG_INFO, "FDT: MAC %02X:%02X:%02X:%02X:%02X:%02X -> %a\n",
           MacAddress[0], MacAddress[1], MacAddress[2],
           MacAddress[3], MacAddress[4], MacAddress[5], NodePath));

    return EFI_SUCCESS;
}

// 为 GMAC0 和 GMAC1 注入 MAC 地址
STATIC EFI_STATUS InjectMacAddresses(IN VOID *FdtBlob) {
    EFI_STATUS Status;
    UINT32 MacLo, MacHi;
    UINT32 MacHiGmac0, MacHiGmac1;
    UINT8 MacAddress[6];

    // 从 OTP 获取基础 MAC 地址
    OtpGetMacAddress(&MacLo, &MacHi);
  
    // GMAC0：清除第8位（偶数 MAC），与寄存器写入逻辑一致
    MacHiGmac0 = MacHi & ~(1 << 8);
    MacLoHiToBytes(MacLo, MacHiGmac0, MacAddress);

    // 尝试注入 GMAC0
    Status = AddMacToFdtNode(FdtBlob, "/ethernet@fe2a0000", MacAddress);
    if (EFI_ERROR(Status)) {
        Status = AddMacToFdtNode(FdtBlob, "/gmac0", MacAddress);
    }
    if (EFI_ERROR(Status)) {
        DEBUG((DEBUG_WARN, "FDT: No GMAC0 node found for MAC injection\n"));
    }

    // GMAC1：设置第8位（奇数 MAC），与寄存器写入逻辑一致
    MacHiGmac1 = MacHi | (1 << 8);
    MacLoHiToBytes(MacLo, MacHiGmac1, MacAddress);

    // 尝试注入 GMAC1
    Status = AddMacToFdtNode(FdtBlob, "/ethernet@fe010000", MacAddress);
    if (EFI_ERROR(Status)) {
        Status = AddMacToFdtNode(FdtBlob, "/gmac1", MacAddress);
    }
    if (EFI_ERROR(Status)) {
        DEBUG((DEBUG_WARN, "FDT: No GMAC1 node found for MAC injection\n"));
    }

    return EFI_SUCCESS;
}

/**
  @param  ImageHandle   of the loaded driver
  @param  SystemTable   Pointer to the System Table

  @retval EFI_SUCCESS           Protocol registered
  @retval EFI_OUT_OF_RESOURCES  Cannot allocate protocol data structure
  @retval EFI_DEVICE_ERROR      Hardware problems

**/
EFI_STATUS
EFIAPI
FdtDxeInitialize (
  IN EFI_HANDLE         ImageHandle,
  IN EFI_SYSTEM_TABLE   *SystemTable
  )
{
  INT32      Retval;
  EFI_STATUS Status;
  UINTN      FdtSize;
  VOID       *FdtImage = NULL;

  switch (PcdGet32 (PcdSystemTableMode)) {
  case SYSTEM_TABLE_MODE_BOTH:
  case SYSTEM_TABLE_MODE_DT:
    break;
  default:
    DEBUG ((DEBUG_ERROR, "Not installing FDT (user config)\n"));
    return EFI_SUCCESS;
  }

  FdtImage = (VOID*)(UINTN)PcdGet64 (PcdFdtBaseAddress);
  Retval = fdt_check_header (FdtImage);
  if (Retval != 0) {
    DEBUG ((DEBUG_ERROR, "No devicetree passed from loader.\n"));
    return EFI_NOT_FOUND;
  }

  FdtSize = fdt_totalsize (FdtImage);

  /*
   * Probably overkill.
   */
  FdtSize += EFI_PAGE_SIZE * 2;
  Status = gBS->AllocatePages (AllocateAnyPages, EfiBootServicesData,
                  EFI_SIZE_TO_PAGES (FdtSize), (EFI_PHYSICAL_ADDRESS*)&mFdtImage);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to allocate devicetree: %r\n", Status));
    goto out;
  }

  Retval = fdt_open_into (FdtImage, mFdtImage, FdtSize);
  if (Retval != 0) {
     DEBUG ((DEBUG_ERROR, "fdt_open_into failed: %d\n", Retval));
     goto out;
  }

  /*
   * These are all best-effort.
   */

  Status = CleanMemoryNodes ();
  if (EFI_ERROR (Status)) {
    Print (L"Failed to clean memory nodes: %r\n", Status);
  }

  Status = FixUartSpeed ();
  if (EFI_ERROR (Status)) {
    Print (L"Failed to fix UART speed: %r\n", Status);
  }

  InjectMacAddresses(mFdtImage);

#ifdef QUARTZ64
  Status = FixMultiPhy1Mode ();
  if (EFI_ERROR (Status)) {
    Print (L"Failed to fix MULTI-PHY1 mode: %r\n", Status);
  }
#endif

  DEBUG ((DEBUG_INFO, "Installed devicetree at address %p\n", mFdtImage));
  Status = gBS->InstallConfigurationTable (&gFdtTableGuid, mFdtImage);
  if (EFI_ERROR (Status)) {
     DEBUG ((DEBUG_ERROR, "Couldn't register devicetree: %r\n", Status));
     goto out;
  }

out:
  if (EFI_ERROR(Status)) {
    if (mFdtImage != NULL) {
      gBS->FreePages ((EFI_PHYSICAL_ADDRESS) mFdtImage, EFI_SIZE_TO_PAGES (FdtSize));
    }
  }
  return Status;
}
