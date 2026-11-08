/** @file
 *
 *  Board SPI NOR flash information protocol.
 *
 *  Produced by SpiNorFlashDxe on the same handle as the FVB protocol,
 *  consumed by the BoardInfoDxe HII page.
 *
 *  Copyright (c) 2026, IndiumEngine
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#ifndef SPI_NOR_FLASH_INFO_PROTOCOL_H__
#define SPI_NOR_FLASH_INFO_PROTOCOL_H__

#define EDKII_SPI_NOR_FLASH_INFO_PROTOCOL_REVISION  1

typedef struct _EDKII_SPI_NOR_FLASH_INFO_PROTOCOL EDKII_SPI_NOR_FLASH_INFO_PROTOCOL;

struct _EDKII_SPI_NOR_FLASH_INFO_PROTOCOL {
  UINT32    Revision;
  BOOLEAN   NorAvailable;          ///< FALSE when the driver fell back to RAM-only storage
  UINT8     JedecId[3];            ///< Manufacturer / type / capacity id
  UINT8     Pad;
  UINT32    Capacity;              ///< NOR capacity in bytes
  UINT32    VariableRegionOffset;  ///< NOR offset of the UEFI variable region
  CHAR16    Vendor[16];            ///< E.g. L"Winbond"
  CHAR16    Model[24];             ///< E.g. L"W25Q128JV"
};

extern EFI_GUID  gEdkiiSpiNorFlashInfoProtocolGuid;

#endif /* SPI_NOR_FLASH_INFO_PROTOCOL_H__ */
