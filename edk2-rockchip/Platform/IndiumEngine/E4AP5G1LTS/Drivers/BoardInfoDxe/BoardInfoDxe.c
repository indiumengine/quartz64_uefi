/** @file
 *
 *  Display-only "Board Information" HII page, shown under Device Manager.
 *
 *  Shows the SPI NOR flash (vendor, model, JEDEC ID, capacity, UEFI
 *  variable region offset) as reported by SpiNorFlashDxe, plus the board
 *  serial number and MAC address derived from the OTP.
 *
 *  Copyright (c) 2026, IndiumEngine
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HiiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>
#include <Library/OtpLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/HiiConfigAccess.h>
#include <Protocol/SpiNorFlashInfo.h>
#include <Uefi/UefiInternalFormRepresentation.h>

#include "BoardInfoFormSetGuid.h"

extern UINT8 BoardInfoHiiBin[];
extern UINT8 BoardInfoDxeStrings[];

typedef struct {
  VENDOR_DEVICE_PATH          VendorDevicePath;
  EFI_DEVICE_PATH_PROTOCOL    End;
} HII_VENDOR_DEVICE_PATH;

STATIC HII_VENDOR_DEVICE_PATH mVendorDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof (VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8)
      }
    },
    BOARD_INFO_FORM_SET_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      (UINT8)(END_DEVICE_PATH_LENGTH),
      (UINT8)((END_DEVICE_PATH_LENGTH) >> 8)
    }
  }
};

//
// Minimal ConfigAccess: the form is display-only, no configuration to
// extract or route.
//
STATIC
EFI_STATUS
EFIAPI
BoardInfoExtractConfig (
  IN CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN CONST EFI_STRING                      Request,
  OUT EFI_STRING                           *Progress,
  OUT EFI_STRING                           *Results
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
BoardInfoRouteConfig (
  IN CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN CONST EFI_STRING                      Configuration,
  OUT EFI_STRING                           *Progress
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
BoardInfoCallback (
  IN CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN     EFI_BROWSER_ACTION                Action,
  IN     EFI_QUESTION_ID                   QuestionId,
  IN     UINT8                             Type,
  IN OUT EFI_IFR_TYPE_VALUE                *Value,
  OUT    EFI_BROWSER_ACTION_REQUEST        *ActionRequest
  )
{
  return EFI_UNSUPPORTED;
}

STATIC EFI_HII_CONFIG_ACCESS_PROTOCOL mConfigAccess = {
  BoardInfoExtractConfig,
  BoardInfoRouteConfig,
  BoardInfoCallback
};


EFI_STATUS
EFIAPI
BoardInfoDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                           Status;
  EFI_HANDLE                           DriverHandle;
  EFI_HII_HANDLE                       HiiHandle;
  EDKII_SPI_NOR_FLASH_INFO_PROTOCOL    *NorInfo = NULL;
  UINT64                               Serial;
  UINT32                               MacLo;
  UINT32                               MacHi;
  UINT8                                OtpData[32];
  UINTN                                Index;
  CHAR16                               Buffer[64];

  Status = gBS->LocateProtocol (
                  &gEdkiiSpiNorFlashInfoProtocolGuid,
                  NULL,
                  (VOID **)&NorInfo
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BoardInfoDxe: SPI NOR info protocol not found: %r\n", Status));
  }

  Serial = OtpGetSerial ();
  OtpGetMacAddress (&MacLo, &MacHi);

  /*
   * Dump everything to the serial console for hardware verification.
   */
  DEBUG ((DEBUG_INFO, "BoardInfoDxe: serial %08x%08x\n",
    (UINT32)(Serial >> 32), (UINT32)Serial));
  DEBUG ((DEBUG_INFO, "BoardInfoDxe: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
    MacLo & 0xFF, (MacLo >> 8) & 0xFF, (MacLo >> 16) & 0xFF, (MacLo >> 24) & 0xFF,
    MacHi & 0xFF, (MacHi >> 8) & 0xFF));

  if (NorInfo != NULL) {
    DEBUG ((DEBUG_INFO,
      "BoardInfoDxe: NOR %s %s, JEDEC 0x%02x%02x%02x, %u MiB, var region @0x%x, available=%d\n",
      NorInfo->Vendor, NorInfo->Model,
      NorInfo->JedecId[0], NorInfo->JedecId[1], NorInfo->JedecId[2],
      NorInfo->Capacity / 1024 / 1024,
      NorInfo->VariableRegionOffset, NorInfo->NorAvailable));
  }

  /*
   * Raw OTP words 0x05..0x0c, to cross-check the serial against the
   * kernel device tree nvmem cell (which starts at a different offset).
   */
  OtpRead (0x05, sizeof (OtpData), OtpData);
  DEBUG ((DEBUG_INFO, "BoardInfoDxe: OTP 0x05..0x0c:"));
  for (Index = 0; Index < sizeof (OtpData); Index++) {
    DEBUG ((DEBUG_INFO, " %02x", OtpData[Index]));
  }
  DEBUG ((DEBUG_INFO, "\n"));

  //
  // Publish the HII package list on a vendor device path handle.
  //
  DriverHandle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &DriverHandle,
                  &gEfiDevicePathProtocolGuid,
                  &mVendorDevicePath,
                  &gEfiHiiConfigAccessProtocolGuid,
                  &mConfigAccess,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  HiiHandle = HiiAddPackages (
                &gBoardInfoFormSetGuid,
                DriverHandle,
                BoardInfoDxeStrings,
                BoardInfoHiiBin,
                NULL
                );
  if (HiiHandle == NULL) {
    DEBUG ((DEBUG_ERROR, "BoardInfoDxe: HiiAddPackages failed\n"));
    gBS->UninstallMultipleProtocolInterfaces (
           DriverHandle,
           &gEfiDevicePathProtocolGuid,
           &mVendorDevicePath,
           &gEfiHiiConfigAccessProtocolGuid,
           &mConfigAccess,
           NULL
         );
    return EFI_OUT_OF_RESOURCES;
  }

  DEBUG ((DEBUG_INFO, "BoardInfoDxe: HII packages added, handle 0x%lx\n", (UINTN)HiiHandle));

  //
  // Fill in the dynamic values.
  //
  if ((NorInfo != NULL) && NorInfo->NorAvailable) {
    UnicodeSPrint (Buffer, sizeof (Buffer), L"%s", NorInfo->Vendor);
    HiiSetString (HiiHandle, STRING_TOKEN (STR_NOR_VENDOR_VALUE), Buffer, NULL);

    UnicodeSPrint (Buffer, sizeof (Buffer), L"%s", NorInfo->Model);
    HiiSetString (HiiHandle, STRING_TOKEN (STR_NOR_MODEL_VALUE), Buffer, NULL);

    UnicodeSPrint (Buffer, sizeof (Buffer), L"0x%02X%02X%02X",
      NorInfo->JedecId[0], NorInfo->JedecId[1], NorInfo->JedecId[2]);
    HiiSetString (HiiHandle, STRING_TOKEN (STR_NOR_JEDEC_VALUE), Buffer, NULL);

    UnicodeSPrint (Buffer, sizeof (Buffer), L"%u MiB", NorInfo->Capacity / 1024 / 1024);
    HiiSetString (HiiHandle, STRING_TOKEN (STR_NOR_CAPACITY_VALUE), Buffer, NULL);

    UnicodeSPrint (Buffer, sizeof (Buffer), L"0x%X", NorInfo->VariableRegionOffset);
    HiiSetString (HiiHandle, STRING_TOKEN (STR_NOR_REGION_VALUE), Buffer, NULL);
  }

  UnicodeSPrint (Buffer, sizeof (Buffer), L"%08x%08x",
    (UINT32)(Serial >> 32), (UINT32)Serial);
  HiiSetString (HiiHandle, STRING_TOKEN (STR_SERIAL_VALUE), Buffer, NULL);

  UnicodeSPrint (Buffer, sizeof (Buffer), L"%02x:%02x:%02x:%02x:%02x:%02x",
    MacLo & 0xFF, (MacLo >> 8) & 0xFF, (MacLo >> 16) & 0xFF, (MacLo >> 24) & 0xFF,
    MacHi & 0xFF, (MacHi >> 8) & 0xFF);
  HiiSetString (HiiHandle, STRING_TOKEN (STR_MAC_VALUE), Buffer, NULL);

  //
  // The front page (the ESC menu) and the Device Manager enumerate every
  // HII package list, look up the formset GUID and fetch the title string.
  // Reproduce that walk so a page that stays invisible shows up in the
  // DEBUG build's serial log.
  //
  DEBUG_CODE_BEGIN ();
  {
    EFI_HII_HANDLE    *Handles;
    UINTN             HandleCount;
    UINTN             OursIndex;
    EFI_IFR_FORM_SET  *FormSet;
    UINTN             FormSetSize;
    EFI_STRING        Title;

    Handles = HiiGetHiiHandles (NULL);
    if (Handles == NULL) {
      DEBUG ((DEBUG_ERROR, "BoardInfoDxe: HiiGetHiiHandles() failed\n"));
    } else {
      for (HandleCount = 0; Handles[HandleCount] != NULL; HandleCount++) {
      }

      OursIndex = MAX_UINTN;
      for (Index = 0; Index < HandleCount; Index++) {
        if (Handles[Index] == HiiHandle) {
          OursIndex = Index;
          break;
        }
      }

      if (OursIndex == MAX_UINTN) {
        DEBUG ((DEBUG_ERROR, "BoardInfoDxe: our HII handle is not in the database\n"));
      } else {
        DEBUG ((DEBUG_INFO, "BoardInfoDxe: %u HII package lists, ours at index %u\n",
          HandleCount, OursIndex));
      }

      for (Index = 0; Index < HandleCount; Index++) {
        FormSet     = NULL;
        FormSetSize = 0;
        if (EFI_ERROR (HiiGetFormSetFromHiiHandle (Handles[Index], &FormSet, &FormSetSize)) ||
            (FormSet == NULL))
        {
          continue;
        }

        if (CompareGuid (&FormSet->Guid, &gBoardInfoFormSetGuid)) {
          Title = HiiGetString (Handles[Index], FormSet->FormSetTitle, NULL);
          DEBUG ((DEBUG_INFO,
            "BoardInfoDxe: formset visible at index %u, title '%s', classguid count %u\n",
            Index,
            (Title != NULL) ? Title : L"<missing>",
            (UINTN)(FormSet->Flags & 0x3)));
          if (Title != NULL) {
            FreePool (Title);
          }
        }

        FreePool (FormSet);
      }

      FreePool (Handles);
    }
  }
  DEBUG_CODE_END ();

  return EFI_SUCCESS;
}
