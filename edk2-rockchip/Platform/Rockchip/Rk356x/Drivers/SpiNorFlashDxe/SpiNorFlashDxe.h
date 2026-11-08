/** @file
 *
 *  SPI NOR backed firmware volume block driver for the RK356x variable
 *  store, modeled after edk2-platforms MvFvbDxe: the variable region lives
 *  in a runtime RAM shadow that is always readable by the CPU, while FVB
 *  writes and erases are forwarded to the SPI NOR flash.
 *
 *  Copyright (c) 2026, IndiumEngine
 *  Copyright (c) 2023, Jared McNeill <jmcneill@invisible.ca>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#ifndef SPI_NOR_FLASH_DXE_H__
#define SPI_NOR_FLASH_DXE_H__

#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/SpiMem.h>
#include <Library/FspiLib.h>
#include <Pi/PiFirmwareVolume.h>
#include <Protocol/FirmwareVolumeBlock.h>
#include <Protocol/SpiNorFlashInfo.h>

typedef struct {
  union {
    UINTN                      FvBase;
    EFI_FIRMWARE_VOLUME_HEADER *VolumeHeader;
  };
  UINTN    FvLength;
  UINTN    NumOfBlocks;
  BOOLEAN  NorAvailable;   ///< FALSE: RAM-only fallback (no NOR writes)
} EFI_FW_VOL_INSTANCE;

extern EFI_FW_VOL_INSTANCE  *mFvInstance;

extern EDKII_SPI_NOR_FLASH_INFO_PROTOCOL  mNorInfo;

typedef struct {
  MEMMAP_DEVICE_PATH          MemMapDevPath;
  EFI_DEVICE_PATH_PROTOCOL    EndDevPath;
} FV_MEMMAP_DEVICE_PATH;

typedef struct {
  EFI_DEVICE_PATH_PROTOCOL            *DevicePath;
  EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  FwVolBlockInstance;
} EFI_FW_VOL_BLOCK_DEVICE;

extern struct HAL_FSPI_HOST  mHost;

//
// SPI NOR accessors shared between the entry point and the FVB backend.
//
EFI_STATUS
SpiNorReadId (
  OUT UINT8  *JedecId
  );

EFI_STATUS
SpiNorRead (
  IN  UINT32  Offset,
  IN  UINT32  Length,
  OUT VOID    *Buffer
  );

EFI_STATUS
SpiNorWrite (
  IN UINT32        Offset,
  IN UINT32        Length,
  IN CONST VOID    *Buffer
  );

EFI_STATUS
SpiNorErase (
  IN UINT32  Offset,
  IN UINT32  Length
  );

VOID
SpiNorVariableRegionChanged (
  VOID
  );

//
// FVB implementation (SpiNorFlashFvb.c)
//
EFI_STATUS
EFIAPI
FvbInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  );

VOID
InstallVirtualAddressChangeHandler (
  VOID
  );

EFI_STATUS
ValidateFvHeader (
  IN EFI_FIRMWARE_VOLUME_HEADER  *FwVolHeader
  );

EFI_STATUS
GetFvbInfo (
  IN  UINT64                       FvLength,
  OUT EFI_FIRMWARE_VOLUME_HEADER   **FvbInfo
  );

EFI_STATUS
FvbGetVolumeAttributes (
  OUT EFI_FVB_ATTRIBUTES_2  *Attributes
  );

EFI_STATUS
FvbSetVolumeAttributes (
  IN OUT EFI_FVB_ATTRIBUTES_2  *Attributes
  );

EFI_STATUS
EFIAPI
FvbProtocolGetAttributes (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *This,
  OUT EFI_FVB_ATTRIBUTES_2                     *Attributes
  );

EFI_STATUS
EFIAPI
FvbProtocolSetAttributes (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *This,
  IN OUT EFI_FVB_ATTRIBUTES_2                  *Attributes
  );

EFI_STATUS
EFIAPI
FvbProtocolGetPhysicalAddress (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *This,
  OUT EFI_PHYSICAL_ADDRESS                     *Address
  );

EFI_STATUS
EFIAPI
FvbProtocolGetBlockSize (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *This,
  IN CONST EFI_LBA                             Lba,
  OUT UINTN                                    *BlockSize,
  OUT UINTN                                    *NumOfBlocks
  );

EFI_STATUS
EFIAPI
FvbProtocolRead (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *This,
  IN CONST EFI_LBA                             Lba,
  IN CONST UINTN                               Offset,
  IN OUT UINTN                                 *NumBytes,
  IN UINT8                                     *Buffer
  );

EFI_STATUS
EFIAPI
FvbProtocolWrite (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *This,
  IN       EFI_LBA                             Lba,
  IN       UINTN                               Offset,
  IN OUT   UINTN                               *NumBytes,
  IN       UINT8                               *Buffer
  );

EFI_STATUS
EFIAPI
FvbProtocolEraseBlocks (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *This,
  ...
  );

#endif /* SPI_NOR_FLASH_DXE_H__ */
