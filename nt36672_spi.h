/*
 * nt36672_spi.h - Etapas 2 y 3 (SIN COMPILAR NI PROBAR)
 *
 * - Pinmux del SPI (GPIO 6-9, funcion qup02 = func_sel 1)
 * - GENI SE2 como maestro SPI: FIFO + sondeo (sin interrupciones)
 * - Lectura del ID del NT36672A siguiendo nt36xxx.c (LineageOS sm6125)
 *
 * Incluir DESPUES de DEVICE_CONTEXT, VizLog, VizFlushLog, VizMarker y GccRd,
 * y ANTES de EvtPrepareHardware.
 *
 * NO pulses el GPIO 87: el driver Linux del C3J tiene NVT_TOUCH_SUPPORT_HW_RST = 0
 * (el reset del chip va ligado al LCD y por comandos SPI).
 *
 * Offsets tomados de qcom-geni-se.h y spi-geni-qcom.c del kernel de LineageOS.
 * Parametros SPI: modo 0, 8 bits, CS0 activo en alto, 6.4 MHz (19.2 MHz / 3).
 */
#pragma once

#ifndef TLMM_WEST
#define TLMM_WEST   0x100000
#define TLMM_CTL(n) (TLMM_WEST + 0x1000UL * (n))
#define TLMM_IO(n)  (TLMM_CTL(n) + 4)
#endif

 /* ---- registros GENI / SPI ---- */
#define VZ_FORCE_DEFAULT      0x020
#define VZ_OUTPUT_CTRL        0x024
#define VZ_CGC_CTRL           0x028
#define VZ_SER_M_CLK_CFG      0x048
#define VZ_CLK_SEL            0x07C
#define VZ_SPI_CPHA           0x224
#define VZ_SPI_LOOPBACK       0x22C
#define VZ_SPI_CPOL           0x230
#define VZ_SPI_DEMUX_OUT_INV  0x24C
#define VZ_SPI_DEMUX_SEL      0x250
#define VZ_DMA_MODE_EN        0x258
#define VZ_TX_PACK_CFG0       0x260
#define VZ_TX_PACK_CFG1       0x264
#define VZ_SPI_TRANS_CFG      0x25C
#define VZ_SPI_WORD_LEN       0x268
#define VZ_SPI_TX_TRANS_LEN   0x26C
#define VZ_SPI_RX_TRANS_LEN   0x270
#define VZ_SPI_DELAY_CNT      0x278
#define VZ_RX_PACK_CFG0       0x284
#define VZ_RX_PACK_CFG1       0x288
#define VZ_M_CMD0             0x600
#define VZ_M_CMD_CTRL         0x604
#define VZ_M_IRQ_STATUS       0x610
#define VZ_M_IRQ_EN           0x614
#define VZ_M_IRQ_CLEAR        0x618
#define VZ_TX_FIFOn           0x700
#define VZ_RX_FIFOn           0x780
#define VZ_TX_FIFO_STATUS     0x800
#define VZ_RX_FIFO_STATUS     0x804
#define VZ_RX_WM              0x810
#define VZ_RX_RFR_WM          0x814
#define VZ_SE_IRQ_EN          0xE1C
#define VZ_HW_PARAM_0         0xE24
#define VZ_DMA_GENERAL_CFG    0xE30

#define VZ_SPI_FULL_DUPLEX    3
#define VZ_M_CMD_DONE         0x00000001
#define VZ_M_ERR_BITS         0x33000006   /* overrun, illegal cmd, fifo rd/wr errors */
#define VZ_M_IRQ_ALL_EN       0x7FC0007F
