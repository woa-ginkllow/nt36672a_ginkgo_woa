/*
 * nt36672_fw.h - Etapa 4 (SIN PROBAR EN HARDWARE): descarga de firmware,
 * info del FW y lectura de eventos de touch del NT36672A (hw_crc = 1).
 * Basado en nt36xxx_fw_update.c / nt36xxx.c (LineageOS sm6125, C3J).
 *
 * El firmware se lee de  C:\novatek_ts_tianma_fw.bin  (copialo ahi).
 *
 * Incluir en nt36672_win.c DESPUES de NvtReadAt/VizSpiXfer/VzDelayMs
 * y ANTES de VizReadTouch.
 */
#pragma once

/* ----------------------------- constantes ------------------------------ */
#define NVT_EVENT_BUF      0x21C00
#define NVT_BOOT_RDY       0x3F10D
#define NVT_ILM_DES        0x3F128
#define NVT_ILM_LEN        0x3F118
#define NVT_G_ILM_CRC      0x3F100
#define NVT_DLM_DES        0x3F12C
#define NVT_DLM_LEN        0x3F130
#define NVT_G_DLM_CRC      0x3F104
#define NVT_BLD_CRC_EN     0x3F30E
#define NVT_SWRST_N8       0x03F0FE
#define NVT_SPI_RD_FAST    0x03F310
#define NVT_EM_HOST_CMD    0x50
#define NVT_EM_RESET_CPL   0x60
#define NVT_EM_FWINFO      0x78
#define NVT_RST_INIT       0xA0
#define NVT_RST_MAX        0xAF
#define NVT_CHUNK          4096          /* Linux usa 63 KB; da igual */
#define NVT_MAX_PART       32
#define NVT_POINT_LEN      65

typedef struct _NVT_PART {
    ULONG Bin, Sram, Size, Crc;
} NVT_PART;

static NVT_PART g_Part[NVT_MAX_PART];
static ULONG    g_PartCnt = 0;
