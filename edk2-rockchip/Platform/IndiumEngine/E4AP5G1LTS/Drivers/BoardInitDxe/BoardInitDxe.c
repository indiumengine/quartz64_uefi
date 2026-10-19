/** @file
 *
 *  Board init for the RK3568-E4AP5G1LTS Platform
 *
 *  Copyright (c) 2026, Jack Pan <panxingyu@indiumengine.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <Base.h>
#include <Library/ArmLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/CruLib.h>
#include <Library/GpioLib.h>
#include <Library/I2cLib.h>
#include <Library/MultiPhyLib.h>
#include <Library/OtpLib.h>
#include <Library/SocLib.h>
#include <Library/Pcie30PhyLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>

#include <IndustryStandard/Rk356x.h>
#include <IndustryStandard/Rk356xCru.h>

#include <Guid/EventGroup.h>
#include <Library/UefiBootManagerLib.h>
#include <Protocol/DevicePath.h>

#include "EthernetPhy.h"

#define DP_NODE_LEN(Type)  { (UINT8)sizeof (Type), (UINT8)(sizeof (Type) >> 8) }

#pragma pack (1)
typedef struct {
  USB_CLASS_DEVICE_PATH       Keyboard;
  EFI_DEVICE_PATH_PROTOCOL    End;
} PLATFORM_USB_KEYBOARD;
#pragma pack ()

STATIC CONST PLATFORM_USB_KEYBOARD  mUsbKeyboard = {
  {
    {
      MESSAGING_DEVICE_PATH, MSG_USB_CLASS_DP,
      DP_NODE_LEN (USB_CLASS_DEVICE_PATH)
    },
    0xFFFF, // VendorId: any
    0xFFFF, // ProductId: any
    3,      // DeviceClass: HID
    1,      // DeviceSubClass: boot
    1       // DeviceProtocol: keyboard
  },
  {
    END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    DP_NODE_LEN (EFI_DEVICE_PATH_PROTOCOL)
  }
};

/**
  Register the short-form USB keyboard device path as a ConIn device before BDS
  connects the USB host controllers.

  PlatformBootManagerBeforeConsole() connects the non-discoverable USB host
  controllers and only afterwards adds the short-form USB keyboard path to
  ConIn. USB enumeration therefore runs while ConIn does not match the
  keyboard: ConPlatform closes SimpleTextIn and the USB bus marks the interface
  as managed, so the keyboard is never re-probed and never reaches ConSplitter.
  The Caps Lock LED works (UsbKbDxe is bound) but no ESC hotkey is delivered.
  Adding the path at EndOfDxe makes ConPlatform accept the keyboard during
  enumeration, before the hotkey service is started.

  @param[in] Event    The EndOfDxe event.
  @param[in] Context  Not used.
**/
STATIC
VOID
EFIAPI
BoardInitEndOfDxeCallback (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;

  Status = EfiBootManagerUpdateConsoleVariable (
             ConIn,
             (EFI_DEVICE_PATH_PROTOCOL *)&mUsbKeyboard,
             NULL
             );
  DEBUG ((DEBUG_INFO, "%a: add USB keyboard to ConIn - %r\n", __func__, Status));

  gBS->CloseEvent (Event);
}

/*
 * GMAC registers
 */
#define GMAC0_MAC_ADDRESS0_LOW  (GMAC0_BASE + 0x0304)
#define GMAC0_MAC_ADDRESS0_HIGH (GMAC0_BASE + 0x0300)
#define GMAC1_MAC_ADDRESS0_LOW  (GMAC1_BASE + 0x0304)
#define GMAC1_MAC_ADDRESS0_HIGH (GMAC1_BASE + 0x0300)

#define GRF_MAC0_CON0           (SYS_GRF + 0x0380)
#define GRF_MAC1_CON0           (SYS_GRF + 0x0388)
#define  CLK_RX_DL_CFG_SHIFT    8
#define  CLK_TX_DL_CFG_SHIFT    0
#define GRF_MAC0_CON1           (SYS_GRF + 0x0384)
#define GRF_MAC1_CON1           (SYS_GRF + 0x038C)
#define  PHY_INTF_SEL_SHIFT     4
#define  PHY_INTF_SEL_MASK      (0x7U << PHY_INTF_SEL_SHIFT)
#define  PHY_INTF_SEL_RGMII     (1U << PHY_INTF_SEL_SHIFT)
#define  FLOWCTRL               BIT3
#define  MAC_SPEED              BIT2
#define  RXCLK_DLY_ENA          BIT1
#define  TXCLK_DLY_ENA          BIT0

#define TX_DELAY_GMAC0          0x3C
#define RX_DELAY_GMAC0          0x2F
#define TX_DELAY_GMAC1          0x4F
#define RX_DELAY_GMAC1          0x26

/*
 * PMIC registers
*/
#define PMIC_I2C_ADDR           0x20

#define PMIC_CHIP_NAME          0xed
#define PMIC_CHIP_VER           0xee
#define PMIC_POWER_EN1          0xb2
#define PMIC_POWER_EN2          0xb3
#define PMIC_POWER_EN3          0xb4
#define PMIC_BUCK1_ON_VSEL      0xbb
#define PMIC_BUCK2_ON_VSEL      0xbe
#define PMIC_BUCK3_ON_VSEL      0xc1
#define PMIC_BUCK4_ON_VSEL      0xc4
#define PMIC_LDO1_ON_VSEL       0xcc
#define PMIC_LDO2_ON_VSEL       0xce
#define PMIC_LDO3_ON_VSEL       0xd0
#define PMIC_LDO4_ON_VSEL       0xd2
#define PMIC_LDO6_ON_VSEL       0xd6
#define PMIC_LDO7_ON_VSEL       0xd8
#define PMIC_LDO8_ON_VSEL       0xda
#define PMIC_LDO9_ON_VSEL       0xdc

/*
 * CPU_GRF registers
*/
#define GRF_CPU_COREPVTPLL_CON0               (CPU_GRF + 0x0010)
#define  CORE_PVTPLL_RING_LENGTH_SEL_SHIFT    3
#define  CORE_PVTPLL_RING_LENGTH_SEL_MASK     (0x1FU << CORE_PVTPLL_RING_LENGTH_SEL_SHIFT)
#define  CORE_PVTPLL_OSC_EN                   BIT1
#define  CORE_PVTPLL_START                    BIT0

/*
 * SYS_GRF registers
 */
#define GRF_IOFUNC_SEL0                       (SYS_GRF + 0x0300)
#define  GMAC1_IOMUX_SEL                      BIT8
#define GRF_IOFUNC_SEL3                       (SYS_GRF + 0x030c)
#define  UART3_IOMUX_SEL                      BIT14
#define  UART4_IOMUX_SEL                      BIT12
#define GRF_IOFUNC_SEL5                       (SYS_GRF + 0x0314)
#define  PCIE30X2_IOMUX_SEL_MASK              (BIT7|BIT6)
#define  PCIE30X2_IOMUX_SEL_M1                BIT6

/*
 * PMU registers
 */
#define PMU_NOC_AUTO_CON0                     (PMU_BASE + 0x0070)
#define PMU_NOC_AUTO_CON1                     (PMU_BASE + 0x0074)


STATIC CONST GPIO_IOMUX_CONFIG mGmac0IomuxConfig[] = {
  { "gmac0_mdcm",         2, GPIO_PIN_PC3, 2, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "gmac0_mdio",         2, GPIO_PIN_PC4, 2, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "gmac0_txd0",         2, GPIO_PIN_PB3, 1, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_2 },
  { "gmac0_txd1",         2, GPIO_PIN_PB4, 1, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_2 },
  { "gmac0_txen",         2, GPIO_PIN_PB5, 1, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "gmac0_rxd0",         2, GPIO_PIN_PB6, 1, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "gmac0_rxd1",         2, GPIO_PIN_PB7, 2, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "gmac0_rxdvcrs",      2, GPIO_PIN_PC0, 2, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "gmac0_rxclk",        2, GPIO_PIN_PA5, 2, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "gmac0_txclk",        2, GPIO_PIN_PB0, 2, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_1 },
  { "gmac0_mclkinout",    2, GPIO_PIN_PC2, 2, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "gmac0_rxd2",         2, GPIO_PIN_PA3, 2, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "gmac0_rxd3",         2, GPIO_PIN_PA4, 2, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "gmac0_txd2",         2, GPIO_PIN_PA6, 2, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_2 },
  { "gmac0_txd3",         2, GPIO_PIN_PA7, 2, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_2 },
};
STATIC CONST GPIO_IOMUX_CONFIG mGmac1IomuxConfig[] = {  
  { "gmac1_mdcm0",        3, GPIO_PIN_PC4, 3, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "gmac1_mdiom0",       3, GPIO_PIN_PC5, 3, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "gmac1_txd0m0",       3, GPIO_PIN_PB5, 3, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_2 },
  { "gmac1_txd1m0",       3, GPIO_PIN_PB6, 3, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_2 },
  { "gmac1_txenm0",       3, GPIO_PIN_PB7, 3, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "gmac1_rxd0m0",       3, GPIO_PIN_PB1, 3, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "gmac1_rxd1m0",       3, GPIO_PIN_PB2, 3, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "gmac1_rxdvcrsm0",    3, GPIO_PIN_PB3, 3, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "gmac1_rxclkm0",      3, GPIO_PIN_PA7, 3, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "gmac1_txclkm0",      3, GPIO_PIN_PA6, 3, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_1 },
  { "gmac1_mclkinoutm0",  3, GPIO_PIN_PC0, 3, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "gmac1_rxd2m0",       3, GPIO_PIN_PA4, 3, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "gmac1_rxd3m0",       3, GPIO_PIN_PA5, 3, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "gmac1_txd2m0",       3, GPIO_PIN_PA2, 3, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_2 },
  { "gmac1_txd3m0",       3, GPIO_PIN_PA3, 3, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_2 },

};
STATIC CONST GPIO_IOMUX_CONFIG mPcie30x2IomuxConfig[] = {
  { "pcie30x2_clkreqnm1", 2, GPIO_PIN_PD4, 4, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "pcie30x2_perstnm1",  2, GPIO_PIN_PD6, 0, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },   //FIXME:set to GPIO mode to test
  { "pcie30x2_wakenm1",   2, GPIO_PIN_PD5, 4, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
};
STATIC
VOID
BoardInitGmac (
  VOID
  )
{
  UINT32 MacLo, MacHi;

  /* Assert reset */
  CruAssertSoftReset (13, 7);  // GMAC0
  CruAssertSoftReset (14, 12); // GMAC1

  /* Keep M0 mux solution for GMAC1 */
  //MmioWrite32 (GRF_IOFUNC_SEL0, (GMAC1_IOMUX_SEL << 16) | GMAC1_IOMUX_SEL);

  /* Configure pins */
  GpioSetIomuxConfig (mGmac0IomuxConfig, ARRAY_SIZE (mGmac0IomuxConfig));
  GpioSetIomuxConfig (mGmac1IomuxConfig, ARRAY_SIZE (mGmac1IomuxConfig));

  /* Setup GMAC0 clocks */
  MmioWrite32 (CRU_CLKSEL_CON (31), 0x00370000);  // Set rmii1_mode to rgmii mode FIXME: here need double check
                                                  // Set rgmii1_clk_sel to 125M
                                                  // Set rmii1_extclk_sel to mac1 clock from SoC
  /* Setup GMAC1 clocks */
  MmioWrite32 (CRU_CLKSEL_CON (33), 0x00370000);  // Set rmii1_mode to rgmii mode
                                                  // Set rgmii1_clk_sel to 125M
                                                  // Set rmii1_extclk_sel to mac1 clock from SoC

  /* Configure GMAC1 */
  MmioWrite32 (GRF_MAC0_CON0,
               0x7F7F0000U |
               (TX_DELAY_GMAC0 << CLK_TX_DL_CFG_SHIFT) |
               (RX_DELAY_GMAC0 << CLK_RX_DL_CFG_SHIFT));
  MmioWrite32 (GRF_MAC0_CON1,
               ((PHY_INTF_SEL_MASK | TXCLK_DLY_ENA | RXCLK_DLY_ENA) << 16) |
               PHY_INTF_SEL_RGMII |
               TXCLK_DLY_ENA |
               RXCLK_DLY_ENA);

  /* Configure GMAC1 */
  MmioWrite32 (GRF_MAC1_CON0,
               0x7F7F0000U |
               (TX_DELAY_GMAC1 << CLK_TX_DL_CFG_SHIFT) |
               (RX_DELAY_GMAC1 << CLK_RX_DL_CFG_SHIFT));
  MmioWrite32 (GRF_MAC1_CON1,
               ((PHY_INTF_SEL_MASK | TXCLK_DLY_ENA | RXCLK_DLY_ENA) << 16) |
               PHY_INTF_SEL_RGMII |
               TXCLK_DLY_ENA |
               RXCLK_DLY_ENA);

  /* Reset GMAC0 PHY */
  GpioPinSetDirection (2, GPIO_PIN_PB1, GPIO_PIN_OUTPUT);
  MicroSecondDelay (1000);
  GpioPinWrite (2, GPIO_PIN_PB1, 0);
  MicroSecondDelay (20000);
  GpioPinWrite (2, GPIO_PIN_PB1, 1);
  MicroSecondDelay (100000);

  /* Reset GMAC1 PHY */
  GpioPinSetDirection (3, GPIO_PIN_PC2, GPIO_PIN_OUTPUT);
  MicroSecondDelay (1000);
  GpioPinWrite (3, GPIO_PIN_PC2, 0);
  MicroSecondDelay (20000);
  GpioPinWrite (3, GPIO_PIN_PC2, 1);
  MicroSecondDelay (100000);

  /* Deassert reset */
  CruDeassertSoftReset (13, 7);  // GMAC0
  CruDeassertSoftReset (14, 12); // GMAC1

  /* Generate MAC addresses from the first 32 bytes in the OTP and write it to GMAC0 and GMAC1 */
  OtpGetMacAddress (&MacLo, &MacHi);

  /* Use sequential MAC addresses. Last byte is even for GMAC0, and odd for GMAC1. */
  MacHi &= ~(1 << 8);
  MmioWrite32 (GMAC0_MAC_ADDRESS0_LOW, MacLo);
  MmioWrite32 (GMAC0_MAC_ADDRESS0_HIGH, MacHi);

  MacHi |= 1 << 8;
  MmioWrite32 (GMAC1_MAC_ADDRESS0_LOW, MacLo);
  MmioWrite32 (GMAC1_MAC_ADDRESS0_HIGH, MacHi);

  EthernetPhyInit (GMAC0_BASE);
  EthernetPhyInit (GMAC1_BASE);
}

STATIC
VOID
BoardInitPcie (
  VOID
  )
{
  GpioSetIomuxConfig (mPcie30x2IomuxConfig, ARRAY_SIZE (mPcie30x2IomuxConfig));

  /* PCIe30x2 IO mux selection - M1 */
  MmioWrite32 (GRF_IOFUNC_SEL5, (PCIE30X2_IOMUX_SEL_MASK << 16) | PCIE30X2_IOMUX_SEL_M1);

}

STATIC
EFI_STATUS
PmicRead (
  IN UINT8 Register,
  OUT UINT8 *Value
  )
{
  return I2cRead (I2C0_BASE, PMIC_I2C_ADDR,
                  &Register, sizeof (Register),
                  Value, sizeof (*Value));
}

STATIC
EFI_STATUS
PmicWrite (
  IN UINT8 Register,
  IN UINT8 Value
  )
{
  return I2cWrite (I2C0_BASE, PMIC_I2C_ADDR,
                  &Register, sizeof (Register),
                  &Value, sizeof (Value));
}

STATIC
VOID
BoardInitPmic (
  VOID
  )
{
  EFI_STATUS Status;
  UINT16 ChipName;
  UINT8 ChipVer;
  UINT8 Value;

  DEBUG ((DEBUG_INFO, "BOARD: PMIC init\n"));

  GpioPinSetPull (0, GPIO_PIN_PB1, GPIO_PIN_PULL_NONE);
  GpioPinSetInput (0, GPIO_PIN_PB1, GPIO_PIN_INPUT_SCHMITT);
  GpioPinSetFunction (0, GPIO_PIN_PB1, 1);
  GpioPinSetPull (0, GPIO_PIN_PB2, GPIO_PIN_PULL_NONE);
  GpioPinSetInput (0, GPIO_PIN_PB2, GPIO_PIN_INPUT_SCHMITT);
  GpioPinSetFunction (0, GPIO_PIN_PB2, 1);

  Status = PmicRead (PMIC_CHIP_NAME, &Value);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "Failed to read PMIC chip name! %r\n", Status));
    ASSERT (FALSE);
  }
  ChipName = (UINT16)Value << 4;

  Status = PmicRead (PMIC_CHIP_VER, &Value);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "Failed to read PMIC chip version! %r\n", Status));
    ASSERT (FALSE);
  }
  ChipName |= (Value >> 4) & 0xF;
  ChipVer = Value & 0xF;

  DEBUG ((DEBUG_INFO, "PMIC: Detected RK%03X ver 0x%X\n", ChipName, ChipVer));
  ASSERT (ChipName == 0x809);

  /* Initialize PMIC */
  PmicWrite (PMIC_BUCK1_ON_VSEL, 0x2c); /* 1.05V - vdd_logic */
  PmicWrite (PMIC_BUCK2_ON_VSEL, 0x2c); /* 1.05V - vdd_gpu   */
  PmicWrite (PMIC_LDO1_ON_VSEL, 0x0c);  /* 0.9V - vcca0v9_img */
  PmicWrite (PMIC_LDO2_ON_VSEL, 0x0c);  /* 0.9V - vdda_0v9 */
  PmicWrite (PMIC_LDO3_ON_VSEL, 0x0c);  /* 0.9V - vdd0v9_pmu */
  /* Skip LDO4  */
  /* Skip LDO5 for now; 1.8V/3.3V - vccio_sd */
  PmicWrite (PMIC_LDO6_ON_VSEL, 0x6c);  /* 3.3V - vcc3v3_pmu */
  PmicWrite (PMIC_LDO7_ON_VSEL, 0x30);  /* 1.8V - vcca_1v8 */
  /* Skip LDO8 LDO9 since it is not use in E4AP5G1LTS */

  PmicWrite (PMIC_POWER_EN1, 0xff); /* LDO1, LDO2, LDO3, LDO4 */
  PmicWrite (PMIC_POWER_EN2, 0xee); /* LDO6, LDO7, LDO8 */
  PmicWrite (PMIC_POWER_EN3, 0x33); /* LDO9, BOOST */
}

EFI_STATUS
EFIAPI
BoardInitDriverEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   EndOfDxeEvent;

  DEBUG ((DEBUG_INFO, "BOARD: BoardInitDriverEntryPoint() called\n"));

  SocSetDomainVoltage (PMUIO2, VCC_3V3);  /* VCC3V3_PMU */
  SocSetDomainVoltage (VCCIO1, VCC_3V3);  /* VCCIO_ACODEC (PCIe) */
  SocSetDomainVoltage (VCCIO2, VCC_3V3);  /* VCCIO_FLASH */
  SocSetDomainVoltage (VCCIO3, VCC_3V3);  /* VCCIO_SD */
  SocSetDomainVoltage (VCCIO4, VCC_3V3);
  SocSetDomainVoltage (VCCIO5, VCC_3V3);
  SocSetDomainVoltage (VCCIO6, VCC_3V3);
  SocSetDomainVoltage (VCCIO7, VCC_3V3);

  BoardInitPmic ();

  /* Set GPIO4 PC1 (WORK_LED) output high to enable LED */
  GpioPinSetDirection (4, GPIO_PIN_PC1, GPIO_PIN_OUTPUT);
  GpioPinWrite (4, GPIO_PIN_PC1, TRUE);

  /* Enable automatic clock gating */
  MmioWrite32 (PMU_NOC_AUTO_CON0, 0xFFFFFFFFU);
  MmioWrite32 (PMU_NOC_AUTO_CON1, 0x000F000FU);

  /* Set core_pvtpll ring length */
  MmioWrite32 (GRF_CPU_COREPVTPLL_CON0,
               ((CORE_PVTPLL_RING_LENGTH_SEL_MASK | CORE_PVTPLL_OSC_EN | CORE_PVTPLL_START) << 16) |
               (5U << CORE_PVTPLL_RING_LENGTH_SEL_SHIFT) | CORE_PVTPLL_OSC_EN | CORE_PVTPLL_START);

  /* Configure MULTI-PHY modes */
  GpioPinSetDirection (0, GPIO_PIN_PA5, GPIO_PIN_OUTPUT); //multiphy0 mux sel 1:USB 0:SATA
  GpioPinSetDirection (0, GPIO_PIN_PA6, GPIO_PIN_OUTPUT); //multiphy1 mux sel 1:USB 0:SATA
  /*
  if (PcdGet32 (PcdMultiPhy1Mode) == MULTIPHY_MODE_SEL_USB3) {
    GpioPinWrite (0, GPIO_PIN_PA5, TRUE);
    GpioPinWrite (0, GPIO_PIN_PA6, TRUE);
    MultiPhySetMode (0, MULTIPHY_MODE_USB3);
    MultiPhySetMode (1, MULTIPHY_MODE_USB3);
    DEBUG ((DEBUG_INFO, "Set multiphy to USB3 Mode.\n"));
    DEBUG
  } else {
    ASSERT (PcdGet32 (PcdMultiPhy1Mode) == MULTIPHY_MODE_SEL_SATA);
    GpioPinWrite (0, GPIO_PIN_PA5, FALSE);
    GpioPinWrite (0, GPIO_PIN_PA6, FALSE);
    MultiPhySetMode (0, MULTIPHY_MODE_SATA);
    MultiPhySetMode (1, MULTIPHY_MODE_SATA);
    DEBUG ((DEBUG_INFO, "Set multiphy to SATA Mode.\n"));
  }
  */
  GpioPinWrite (0, GPIO_PIN_PA5, FALSE);
  GpioPinWrite (0, GPIO_PIN_PA6, FALSE);
  MultiPhySetMode (0, MULTIPHY_MODE_SATA);
  MultiPhySetMode (1, MULTIPHY_MODE_SATA);
  MultiPhySetMode (2, MULTIPHY_MODE_SATA);


  /* Set GPIO3 PA0 PA1 (USB2_HOST_PWREN & USB3_HOST_PWREN) output high to power USB ports */
  GpioPinSetDirection (3, GPIO_PIN_PA0, GPIO_PIN_OUTPUT);
  GpioPinSetDirection (3, GPIO_PIN_PA1, GPIO_PIN_OUTPUT);
  GpioPinWrite (3, GPIO_PIN_PA0, TRUE);
  GpioPinWrite (3, GPIO_PIN_PA1, TRUE);
  DEBUG ((DEBUG_INFO, "INFO: USB power now ready.\n"));

  /* PCIe setup */
  BoardInitPcie ();

  /* GMAC setup */
  BoardInitGmac ();

  //
  // Add the USB keyboard to ConIn before BDS connects the USB host controllers,
  // so ConPlatform accepts it when the USB bus is enumerated.
  //
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  BoardInitEndOfDxeCallback,
                  NULL,
                  &gEfiEndOfDxeEventGroupGuid,
                  &EndOfDxeEvent
                  );
  ASSERT_EFI_ERROR (Status);

  DEBUG ((DEBUG_INFO, "INFO: Exit board init.\n"));

  return EFI_SUCCESS;
}
