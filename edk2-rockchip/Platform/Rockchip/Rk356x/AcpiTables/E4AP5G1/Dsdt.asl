/** @file
*  Differentiated System Description Table Fields (DSDT) for the IndiumEngine E4AP5G1LTS
*
*  Copyright (C) 2026, Jack Pan <panxingyu@indiumengine.com>
*
*  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <IndustryStandard/Acpi60.h>

DefinitionBlock ("DsdtTable.aml", "DSDT",
                 EFI_ACPI_6_0_DIFFERENTIATED_SYSTEM_DESCRIPTION_TABLE_REVISION,
                 "RKCP  ", "RK356X  ", FixedPcdGet32 (PcdAcpiDefaultOemRevision)) {
  Scope (_SB) {

    include ("Cpu.asl")
    include ("Tsadc.asl")
    include ("Uart.asl")
    include ("Wdt.asl")
    include ("Usb2.asl")
    include ("Usb3.asl")
    include ("Gmac.asl")
    include ("Mshc.asl")
    include ("Emmc.asl")
    include ("Sata.asl")
    include ("Pcie2x1.asl")

  } // Scope (_SB)
}
