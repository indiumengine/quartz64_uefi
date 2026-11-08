/** @file
 *
 *  RK356x SFC (Serial Flash Controller) register definitions.
 *
 *  Register map verified against:
 *   - Linux: drivers/spi/spi-rockchip-sfc.c
 *   - U-Boot: drivers/spi/rockchip_sfc.c
 *
 *  Copyright (c) 2026, IndiumEngine
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#ifndef RK356XFSPI_H__
#define RK356XFSPI_H__

/* Register base address */
#define FSPI_BASE          0xFE300000UL

/* Version register values; RK356x SFC is version 5 */
#define FSPI_VER_VER_1     1
#define FSPI_VER_VER_3     3
#define FSPI_VER_VER_5     5
#define FSPI_VER           FSPI_VER_VER_5

/* FSPI_CTRL */
#define FSPI_CTRL_PHASE_SEL_NEGETIVE    (1UL << 1)
#define FSPI_CTRL_CMD_BITS_SHIFT       8
#define FSPI_CTRL_ADDR_BITS_SHIFT      10
#define FSPI_CTRL_DATA_BITS_SHIFT      12
#define FSPI_CTRL_WPEN                  (1UL << 29)

/* FSPI_RCVR */
#define FSPI_RCVR_RCVR_SHIFT           0

/* FSPI_ISR */
#define FSPI_ISR_RXFS_SHIFT            0
#define FSPI_ISR_RXUS_SHIFT            1
#define FSPI_ISR_TXOS_SHIFT            2
#define FSPI_ISR_TXES_SHIFT            3
#define FSPI_ISR_TRANSS_SHIFT          4
#define FSPI_ISR_AHBS_SHIFT            5
#define FSPI_ISR_NSPIS_SHIFT           6
#define FSPI_ISR_DMAS_SHIFT            7

/* FSPI_FSR */
#define FSPI_FSR_TXFS_SHIFT            0
#define FSPI_FSR_TXES_SHIFT            1
#define FSPI_FSR_RXES_SHIFT            2
#define FSPI_FSR_RXFS_SHIFT            3
#define FSPI_FSR_TXLV_SHIFT            8
#define FSPI_FSR_RXLV_SHIFT            16

/* FSPI_SR */
#define FSPI_SR_SR_SHIFT               0

/* FSPI_RISR */
#define FSPI_RISR_TRANSS_SHIFT         4

/* FSPI_DMA_TRIGGER */
#define FSPI_DMATR_DMATR_SHIFT         0

/* FSPI_EXT_AX / per-CS FSPI_AX */
#define FSPI_EXT_AX_AX_SETUP_PAT_SHIFT   0
#define FSPI_EXT_AX_AX_CANCEL_PAT_SHIFT  8
#define FSPI_AX0_AX_SHIFT              0
#define FSPI_AX1_AX_SHIFT              0

/* FSPI_LEN_CTRL */
#define FSPI_LEN_CTRL_TRB_SEL_MASK     1

/* FSPI_DLL_CTRL0 */
#define FSPI_DLL_CTRL0_SCLK_SMP_SEL_SHIFT  15

/* Per-CS register offset for chip select 1 */
#define FSPI_CS1_REG_OFFSET            0x200

typedef volatile struct FSPI_REG {
  UINT32    CTRL0;         /* 0x00: control */
  UINT32    IMR;           /* 0x04: interrupt mask */
  UINT32    ICLR;          /* 0x08: interrupt clear */
  UINT32    FTLR;          /* 0x0c: FIFO threshold level */
  UINT32    RCVR;          /* 0x10: reset FSM and FIFO */
  UINT32    EXT_AX;        /* 0x14: enhanced mode pattern */
  union {
    UINT32  AX0;           /* 0x18: CS0 enhanced mode value */
    UINT32  ABIT0;         /* 0x18: CS0 address bit number */
  };
  UINT32    ISR;           /* 0x1c: interrupt status */
  UINT32    FSR;           /* 0x20: FIFO status */
  UINT32    SR;            /* 0x24: FSM status */
  UINT32    RISR;          /* 0x28: raw interrupt status */
  UINT32    VER;           /* 0x2c: version */
  UINT32    RES0x30;       /* 0x30 */
  UINT32    EXT_CTRL;      /* 0x34: extension control */
  UINT32    RES0x38;       /* 0x38 */
  UINT32    DLL_CTRL0;     /* 0x3c: CS0 delay line control */
  UINT32    RES0x40[16];   /* 0x40 - 0x7c */
  UINT32    DMA_TRIGGER;   /* 0x80: master DMA trigger */
  UINT32    DMA_ADDR;      /* 0x84: master DMA address */
  UINT32    LEN_CTRL;      /* 0x88: length control extension */
  UINT32    LEN_EXT;       /* 0x8c: transfer length extension */
  UINT32    RES0x90[4];    /* 0x90 - 0x9c */
  UINT32    RES0xA0[24];   /* 0xa0 - 0xfc */
  UINT32    CMD;           /* 0x100: command */
  UINT32    ADDR;          /* 0x104: address */
  UINT32    DATA;          /* 0x108: data FIFO */
  UINT32    RES0x10C[61];  /* 0x10c - 0x1fc */
  UINT32    CTRL1;         /* 0x200: CS1 control */
  UINT32    RES0x204[5];   /* 0x204 - 0x214 */
  union {
    UINT32  AX1;           /* 0x218: CS1 enhanced mode value */
    UINT32  ABIT1;         /* 0x218: CS1 address bit number */
  };
  UINT32    RES0x21C[8];   /* 0x21c - 0x238 */
  UINT32    DLL_CTRL1;     /* 0x23c: CS1 delay line control */
} FSPI_REG;

#endif /* RK356XFSPI_H__ */
