/*
 * nt36672_win.c - Minidriver HID (KMDF + mshidkmdf) para el touch NT36672A
 * del Redmi Note 8 (ginkgo, SM6125).
 *
 * Versión con diagnóstico de reset (GPIO 87) y más logs de protocolo.
 *
 * VIZ_TEST_FAKE_TOUCH = 0  → modo real
 * VIZ_RELEASE_RESET   = 1  → intenta sacar el chip de reset (prueba)
 */

#include <ntddk.h>
#include <wdf.h>
#include <hidport.h>
#include <ntstrsafe.h>
#include "nt36672_win_hid.h"
#include "nt36672_spi.h"
#include "nt36672_fw.h"

static CHAR  g_Buf[32768];
static ULONG g_BufLen = 0;

static VOID VizMarker(PCWSTR tag, ULONG a, ULONG b)
{
    WCHAR name[96];
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;

    RtlStringCbPrintfW(name, sizeof(name),
        L"\\??\\C:\\vz_%ws_%08lX_%08lX.txt", tag, a, b);
    RtlInitUnicodeString(&us, name);
    InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&h, FILE_GENERIC_WRITE, &oa, &iosb, NULL,
        FILE_ATTRIBUTE_NORMAL, 0, FILE_OVERWRITE_IF,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0)))
        ZwClose(h);
}

static VOID VizFlushLog(VOID)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\??\\C:\\viZTouch.log");
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    NTSTATUS stC, stW = (NTSTATUS)0xFFFFFFFF;

    InitializeObjectAttributes(&oa, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    stC = ZwCreateFile(&h, FILE_GENERIC_WRITE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, 0,
        FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
        NULL, 0);
    if (NT_SUCCESS(stC)) {
        stW = ZwWriteFile(h, NULL, NULL, NULL, &iosb, g_Buf, g_BufLen, NULL, NULL);
        ZwClose(h);
    }
    VizMarker(L"flush", (ULONG)stC, (ULONG)stW);
}

#define VIZ_TEST_FAKE_TOUCH  0
#define VIZ_RELEASE_RESET    0      // NO tocar GPIO 87
#define VIZ_HW_DIAG          1
#define VIZ_ENABLE_SE_PROBE  0
#define VIZ_POLL_MS          8

#define VIZ_VENDOR_ID   0x1209
#define VIZ_PRODUCT_ID  0x7E57
#define VIZ_VERSION     0x0001

#define VIZ_LOG_PHYS_BASE   0x0ULL
#define VIZ_LOG_SIZE        0x10000

typedef struct _VIZ_CLK { ULONG Reg; ULONG Mask; } VIZ_CLK;
static const VIZ_CLK g_Clks[] = {
    { 0x7900C, 0x13C0 },   /* bits 6,7,8,9,12 → m_ahb + s_ahb + core + core_2x + s2 */
};

#define GENI_INIT_CFG_REVISION  0x000
#define SE_GENI_FW_REVISION_RO  0x068

typedef struct _DEVICE_CONTEXT {
    PVOID Se;    SIZE_T SeLen;
    PVOID Tlmm;  SIZE_T TlmmLen;
    PVOID Gcc;   SIZE_T GccLen;
    PVOID Log;
    WDFQUEUE ReadQueue;
    WDFTIMER PollTimer;
    ULONG    Tick;
    volatile LONG Busy;
    BOOLEAN       Ready;
    BOOLEAN       HavePending;
    ULONG         PrevMask;
    VIZ_TOUCH_REPORT Pending;
} DEVICE_CONTEXT, * PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetCtx)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY EvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT EvtD0Exit;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL EvtIoInternalDeviceControl;
EVT_WDF_TIMER EvtPollTimer;

/* ------------------------------ log ------------------------------------ */

static PVOID g_Log = NULL;
typedef struct _VIZ_LOG_HDR { ULONG Magic; ULONG Head; } VIZ_LOG_HDR;

static VOID VizLog(_In_ PCSTR Fmt, ...)
{
    CHAR buf[200];
    size_t n = 0;
    va_list args;
    va_start(args, Fmt);
    RtlStringCbVPrintfA(buf, sizeof(buf), Fmt, args);
    va_end(args);

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "viztouch: %s\n", buf);

    RtlStringCbLengthA(buf, sizeof(buf), &n);
    if (g_BufLen + n + 1 < sizeof(g_Buf)) {
        RtlCopyMemory(g_Buf + g_BufLen, buf, n);
        g_Buf[g_BufLen + n] = '\n';
        g_BufLen += (ULONG)n + 1;
    }

    if (g_Log) {
        VIZ_LOG_HDR* h = (VIZ_LOG_HDR*)g_Log;
        PUCHAR data = (PUCHAR)g_Log + sizeof(VIZ_LOG_HDR);
        ULONG cap = VIZ_LOG_SIZE - sizeof(VIZ_LOG_HDR);
        for (size_t i = 0; i < n; i++) { data[h->Head % cap] = (UCHAR)buf[i]; h->Head++; }
        data[h->Head % cap] = '\n'; h->Head++;
    }
}

/* ---------------------------- hardware --------------------------------- */

static VOID VizEnableClocks(_In_ PDEVICE_CONTEXT c)
{
    if (!c->Gcc) return;
    for (ULONG i = 0; i < ARRAYSIZE(g_Clks); i++) {
        if (g_Clks[i].Mask == 0) continue;
        PULONG r = (PULONG)((PUCHAR)c->Gcc + g_Clks[i].Reg);
        WRITE_REGISTER_ULONG(r, READ_REGISTER_ULONG(r) | g_Clks[i].Mask);
        VizLog("clk[%lu] reg=0x%lx -> 0x%lx", i, g_Clks[i].Reg, READ_REGISTER_ULONG(r));
    }
}

static ULONG GccRd(PDEVICE_CONTEXT c, ULONG off)
{
    return READ_REGISTER_ULONG((PULONG)((PUCHAR)c->Gcc + off));
}

static VOID VizClkDump(PDEVICE_CONTEXT c)
{
    VizLog("vote=%08lx", GccRd(c, 0x7900C));
    VizLog("rcg cmd=%08lx cfg=%08lx m=%08lx n=%08lx d=%08lx",
        GccRd(c, 0x1F3A8), GccRd(c, 0x1F3AC), GccRd(c, 0x1F3B0),
        GccRd(c, 0x1F3B4), GccRd(c, 0x1F3B8));
}

static VOID VizClkCheck(PDEVICE_CONTEXT c)
{
    ULONG h[] = { 0x1F004, 0x1F008, 0x1F00C, 0x1F014, 0x1F3A4 };
    for (ULONG i = 0; i < ARRAYSIZE(h); i++)
        VizLog("halt[0x%lx]=%08lx (bit31=%lu)", h[i], GccRd(c, h[i]), GccRd(c, h[i]) >> 31);
}

/* TLMM: en SM6125 el bloque empieza en 0x400000.
 * Probamos primero sin offset WEST. Si los GPIO salen a 0, se puede probar con 0x100000. */

static VOID VizTlmmDump(PDEVICE_CONTEXT c)
{
    ULONG pins[] = { 6, 7, 8, 9, 87, 88 };
    for (ULONG i = 0; i < ARRAYSIZE(pins); i++) {
        ULONG ctl = READ_REGISTER_ULONG((PULONG)((PUCHAR)c->Tlmm + TLMM_CTL(pins[i])));
        ULONG io = READ_REGISTER_ULONG((PULONG)((PUCHAR)c->Tlmm + TLMM_IO(pins[i])));
        VizLog("gpio%lu ctl=%08lx io=%08lx (func=%lu oe=%lu)",
            pins[i], ctl, io, (ctl >> 2) & 0xF, (ctl >> 9) & 1);
    }
}

static VOID VzDelayMs(LONG ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * ms;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static ULONG SeRd(PDEVICE_CONTEXT c, ULONG off)
{
    return READ_REGISTER_ULONG((PULONG)((PUCHAR)c->Se + off));
}
static VOID SeWr(PDEVICE_CONTEXT c, ULONG off, ULONG v)
{
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)c->Se + off), v);
}

/* ------------------------- pinmux SPI (GPIO 6..9) ----------------------- */

static VOID VizSpiPinmux(PDEVICE_CONTEXT c)
{
    /* func_sel=1 (qup02), drive 6 mA (campo=2), sin pull */
    for (ULONG p = 6; p <= 9; p++) {
        WRITE_REGISTER_ULONG((PULONG)((PUCHAR)c->Tlmm + TLMM_CTL(p)),
            (1u << 2) | (2u << 6));
        VizLog("gpio%lu ctl=%08lx", p,
            READ_REGISTER_ULONG((PULONG)((PUCHAR)c->Tlmm + TLMM_CTL(p))));
    }
}

/* ------------------------- init GENI como maestro SPI ------------------- */

static VOID VizGeniInit(PDEVICE_CONTEXT c)
{
    ULONG depth = (SeRd(c, VZ_HW_PARAM_0) >> 16) & 0x3F;

    SeWr(c, VZ_CGC_CTRL, SeRd(c, VZ_CGC_CTRL) | 0x7F);
    SeWr(c, VZ_DMA_GENERAL_CFG, SeRd(c, VZ_DMA_GENERAL_CFG) | 0xF);
    SeWr(c, VZ_OUTPUT_CTRL, 0x7F);
    SeWr(c, VZ_FORCE_DEFAULT, 1);
    SeWr(c, VZ_RX_WM, 0);
    SeWr(c, VZ_RX_RFR_WM, depth > 2 ? depth - 2 : 0);
    SeWr(c, VZ_DMA_MODE_EN, 0);
    SeWr(c, VZ_M_IRQ_EN, VZ_M_IRQ_ALL_EN);
    SeWr(c, VZ_SE_IRQ_EN, 0xF);
}

static VOID VizSpiSetup(PDEVICE_CONTEXT c)
{
    ULONG wl;

    SeWr(c, VZ_SPI_LOOPBACK, 0);
    SeWr(c, VZ_SPI_DEMUX_SEL, 0);                /* CS0 */
    SeWr(c, VZ_SPI_CPHA, 0);                     /* modo 0 */
    SeWr(c, VZ_SPI_CPOL, 0);
    SeWr(c, VZ_SPI_DEMUX_OUT_INV, 0);   /* CS activo en bajo */
    SeWr(c, VZ_CLK_SEL, 0);
    SeWr(c, VZ_SER_M_CLK_CFG, (3u << 4) | 1u);   /* div 3 → ~6.4 MHz */
    SeWr(c, VZ_SPI_DELAY_CNT, 0);

    SeWr(c, VZ_TX_PACK_CFG0, 0x7F8FE);
    SeWr(c, VZ_TX_PACK_CFG1, 0xFFEFE);
    SeWr(c, VZ_RX_PACK_CFG0, 0x7F8FE);
    SeWr(c, VZ_RX_PACK_CFG1, 0xFFEFE);

    wl = SeRd(c, VZ_SPI_WORD_LEN);
    wl &= ~0x3FFu;
    wl |= (8 - 4);
    SeWr(c, VZ_SPI_WORD_LEN, wl);
}

/* ------------------------- transferencia SPI (sondeo) ------------------- */

static NTSTATUS VizSpiXfer(PDEVICE_CONTEXT c, const UCHAR* tx, UCHAR* rx, ULONG len)
{
    ULONG depth = (SeRd(c, VZ_HW_PARAM_0) >> 16) & 0x3F;
    ULONG txOff = 0, rxOff = 0, spins = 0, v, irq;

    if (len == 0 || len > 0xFFFFFF || depth == 0) return STATUS_INVALID_PARAMETER;

    SeWr(c, VZ_M_IRQ_CLEAR, 0xFFFFFFFF);
    SeWr(c, VZ_SPI_TRANS_CFG, SeRd(c, VZ_SPI_TRANS_CFG) & ~1u);
    SeWr(c, VZ_SPI_TX_TRANS_LEN, len);
    SeWr(c, VZ_SPI_RX_TRANS_LEN, len);
    SeWr(c, VZ_M_CMD0, (VZ_SPI_FULL_DUPLEX << 27));

    while (rxOff < len) {
        BOOLEAN progress = FALSE;

        while (txOff < len) {
            ULONG wc = SeRd(c, VZ_TX_FIFO_STATUS) & 0x0FFFFFFF;
            ULONG w = 0, n = (len - txOff) < 4 ? (len - txOff) : 4;
            if (wc >= depth) break;
            for (ULONG j = 0; j < n; j++) w |= (ULONG)tx[txOff + j] << (8 * j);
            SeWr(c, VZ_TX_FIFOn, w);
            txOff += n;
            progress = TRUE;
        }

        v = SeRd(c, VZ_RX_FIFO_STATUS) & 0x01FFFFFF;
        while (v-- && rxOff < len) {
            ULONG w = SeRd(c, VZ_RX_FIFOn);
            ULONG n = (len - rxOff) < 4 ? (len - rxOff) : 4;
            for (ULONG j = 0; j < n; j++) rx[rxOff + j] = (UCHAR)(w >> (8 * j));
            rxOff += n;
            progress = TRUE;
        }

        if (!progress) {
            KeStallExecutionProcessor(2);
            if (++spins > 100000) {
                SeWr(c, VZ_M_CMD_CTRL, 1u << 2);
                VizLog("spi timeout tx=%lu rx=%lu/%lu", txOff, rxOff, len);
                return STATUS_IO_TIMEOUT;
            }
        }
    }

    for (spins = 0; spins < 100000; spins++) {
        irq = SeRd(c, VZ_M_IRQ_STATUS);
        if (irq & VZ_M_ERR_BITS) {
            VizLog("spi err irq=%08lx", irq);
            SeWr(c, VZ_M_IRQ_CLEAR, irq);
            return STATUS_DEVICE_PROTOCOL_ERROR;
        }
        if ((irq & VZ_M_CMD_DONE) || !(SeRd(c, 0x040) & 1)) {
            SeWr(c, VZ_M_IRQ_CLEAR, irq);
            return STATUS_SUCCESS;
        }
        KeStallExecutionProcessor(2);
    }
    VizLog("done timeout irq=%08lx geni=%08lx", SeRd(c, VZ_M_IRQ_STATUS), SeRd(c, 0x040));
    return STATUS_IO_TIMEOUT;
}

static VOID VizSpiLoopback(PDEVICE_CONTEXT c)
{
    UCHAR t[4] = { 0xA5, 0x5A, 0x12, 0x34 }, r[4] = { 0 };
    NTSTATUS st;
    SeWr(c, VZ_SPI_LOOPBACK, 1);
    st = VizSpiXfer(c, t, r, 4);
    SeWr(c, VZ_SPI_LOOPBACK, 0);
    VizLog("loopback st=%08lx rx=%02lx %02lx %02lx %02lx", (ULONG)st,
        (ULONG)r[0], (ULONG)r[1], (ULONG)r[2], (ULONG)r[3]);
}

/* ------------------------- protocolo NT36xxx ---------------------------- */

static NTSTATUS NvtSetPage(PDEVICE_CONTEXT c, ULONG addr)
{
    UCHAR t[3], r[3];
    NTSTATUS st;
    t[0] = 0xFF;
    t[1] = (UCHAR)((addr >> 15) & 0xFF);
    t[2] = (UCHAR)((addr >> 7) & 0xFF);
    st = VizSpiXfer(c, t, r, 3);
    VizLog("set_page addr=0x%06lx st=%08lx rx=%02lx %02lx %02lx",
        addr, (ULONG)st, (ULONG)r[0], (ULONG)r[1], (ULONG)r[2]);
    return st;
}

static NTSTATUS NvtWriteAddr(PDEVICE_CONTEXT c, ULONG addr, UCHAR data)
{
    UCHAR t[2], r[2];
    NTSTATUS st = NvtSetPage(c, addr);
    if (!NT_SUCCESS(st)) return st;
    t[0] = (UCHAR)((addr & 0x7F) | 0x80);
    t[1] = data;
    st = VizSpiXfer(c, t, r, 2);
    VizLog("write_addr 0x%06lx=0x%02lx st=%08lx", addr, (ULONG)data, (ULONG)st);
    return st;
}

static NTSTATUS NvtReadAt(PDEVICE_CONTEXT c, ULONG pageAddr, UCHAR offset, UCHAR* out, ULONG n)
{
    UCHAR t[96], r[96];
    NTSTATUS st;
    if (n == 0 || n > 94) return STATUS_INVALID_PARAMETER;

    st = NvtSetPage(c, pageAddr);
    if (!NT_SUCCESS(st)) return st;

    RtlZeroMemory(t, sizeof(t));
    t[0] = offset & 0x7F;
    st = VizSpiXfer(c, t, r, n + 2);
    if (NT_SUCCESS(st)) {
        RtlCopyMemory(out, r + 2, n);
        VizLog("read_at page=0x%06lx off=0x%02lx n=%lu st=%08lx data=%02lx %02lx %02lx %02lx %02lx %02lx",
            pageAddr, (ULONG)offset, n, (ULONG)st,
            (ULONG)out[0], (ULONG)out[1], (ULONG)out[2],
            (ULONG)out[3], (ULONG)out[4], (ULONG)out[5]);
    }
    else {
        VizLog("read_at FAILED page=0x%06lx off=0x%02lx st=%08lx", pageAddr, (ULONG)offset, (ULONG)st);
    }
    return st;
}

/* ------------------------- diagnóstico + probe ------------------------- */

static VOID VizGpio87Dump(PDEVICE_CONTEXT c, PCSTR tag)
{
    ULONG ctl = READ_REGISTER_ULONG((PULONG)((PUCHAR)c->Tlmm + TLMM_CTL(87)));
    ULONG io = READ_REGISTER_ULONG((PULONG)((PUCHAR)c->Tlmm + TLMM_IO(87)));
    VizLog("%s gpio87 ctl=%08lx io=%08lx (func=%lu oe=%lu level=%lu)",
        tag, ctl, io, (ctl >> 2) & 0xF, (ctl >> 9) & 1, io & 1);
}

static VOID VizReleaseReset(PDEVICE_CONTEXT c)
{
    /* Configurar como GPIO output y poner nivel ALTO (sacar de reset activo-bajo) */
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)c->Tlmm + TLMM_CTL(87)),
        (0u << 2) | (2u << 6) | (1u << 9));   /* func=gpio, drive, OE=1 */
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)c->Tlmm + TLMM_IO(87)), 1);
    VzDelayMs(20);
    VizGpio87Dump(c, "despues de liberar reset");
}

static VOID VizSpiProbe(PDEVICE_CONTEXT c)
{
    UCHAR id[16];
    NTSTATUS st;
    ULONG retry, i;

    VizMarker(L"spi0", 0, 0);
    VizGpio87Dump(c, "estado reset (no se toca)");

    VizSpiPinmux(c);
    VizGeniInit(c);
    VizSpiSetup(c);

    VizLog("fifo depth=%lu width=%lu",
        (SeRd(c, VZ_HW_PARAM_0) >> 16) & 0x3F,
        (SeRd(c, VZ_HW_PARAM_0) >> 24) & 0x3F);

    VizSpiLoopback(c);
    VizFlushLog();

    /* ---------- Secuencia agresiva de wake-up ---------- */

    // 1. Engineering reset
    st = NvtWriteAddr(c, 0x7FFF80, 0x5A);
    VizLog("eng_reset st=%08lx", (ULONG)st);
    VzDelayMs(50);

    // 2. Varios bootloader reset + esperas largas
    for (i = 0; i < 3; i++) {
        st = NvtWriteAddr(c, 0x03F0FE, 0x69);   // swrst
        VizLog("swrst #%lu st=%08lx", i, (ULONG)st);
        VzDelayMs(30);
    }

    // 3. Desactivar SPI_RD_FAST
    st = NvtWriteAddr(c, 0x03F310, 0x00);
    VizLog("spi_rd_fast off st=%08lx", (ULONG)st);
    VzDelayMs(20);

    /* ---------- Pruebas de lectura de ID en varias direcciones ---------- */

    // Dirección clásica del driver Linux
    for (retry = 0; retry < 5; retry++) {
        RtlZeroMemory(id, sizeof(id));
        st = NvtReadAt(c, 0x1F600, 0x4E, id, 6);
        VizLog("ID classic try%lu: %02lx %02lx %02lx %02lx %02lx %02lx  st=%08lx",
            retry,
            (ULONG)id[0], (ULONG)id[1], (ULONG)id[2],
            (ULONG)id[3], (ULONG)id[4], (ULONG)id[5], (ULONG)st);
        VzDelayMs(15);
    }

    // Otras direcciones que a veces usan los drivers Novatek
    RtlZeroMemory(id, sizeof(id));
    st = NvtReadAt(c, 0x1F200, 0x00, id, 8);
    VizLog("ID alt1 0x1F200: %02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx st=%08lx",
        (ULONG)id[0], (ULONG)id[1], (ULONG)id[2], (ULONG)id[3],
        (ULONG)id[4], (ULONG)id[5], (ULONG)id[6], (ULONG)id[7], (ULONG)st);

    RtlZeroMemory(id, sizeof(id));
    st = NvtReadAt(c, 0x3F000, 0x00, id, 8);
    VizLog("ID alt2 0x3F000: %02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx st=%08lx",
        (ULONG)id[0], (ULONG)id[1], (ULONG)id[2], (ULONG)id[3],
        (ULONG)id[4], (ULONG)id[5], (ULONG)id[6], (ULONG)id[7], (ULONG)st);

    // Lectura cruda de la página 0 (a veces responde el bootloader)
    RtlZeroMemory(id, sizeof(id));
    st = NvtReadAt(c, 0x000000, 0x00, id, 8);
    VizLog("ID page0: %02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx st=%08lx",
        (ULONG)id[0], (ULONG)id[1], (ULONG)id[2], (ULONG)id[3],
        (ULONG)id[4], (ULONG)id[5], (ULONG)id[6], (ULONG)id[7], (ULONG)st);

    VizFlushLog();
    VizMarker(L"spi2", 0, 0);
}

/* ---------- bit-bang diagnostico (GPIO6=MISO 7=MOSI 8=CLK 9=CS) ---------- */
static ULONG g_bbCsHigh = 1, g_bbMode = 0;

static ULONG GpIn(PDEVICE_CONTEXT c, ULONG p)
{
    return READ_REGISTER_ULONG((PULONG)((PUCHAR)c->Tlmm + TLMM_IO(p))) & 1;
}
static VOID GpOut(PDEVICE_CONTEXT c, ULONG p, ULONG v)
{
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)c->Tlmm + TLMM_IO(p)), v ? 2 : 0);
}
static VOID GpCfg(PDEVICE_CONTEXT c, ULONG p, ULONG pull, ULONG oe)
{
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)c->Tlmm + TLMM_CTL(p)),
        pull | (2u << 6) | (oe ? (1u << 9) : 0));
}   /* func=0 (GPIO) */

static VOID BbXfer(PDEVICE_CONTEXT c, const UCHAR* tx, UCHAR* rx, ULONG len)
{
    ULONG i, b;
    GpOut(c, 8, g_bbMode == 3);                 /* CLK en reposo */
    GpOut(c, 9, g_bbCsHigh);                    /* CS activo */
    KeStallExecutionProcessor(5);
    for (i = 0; i < len; i++) {
        UCHAR r = 0;
        for (b = 0; b < 8; b++) {
            if (g_bbMode == 3) GpOut(c, 8, 0);
            GpOut(c, 7, (tx[i] >> (7 - b)) & 1);
            KeStallExecutionProcessor(2);
            GpOut(c, 8, 1);                      /* flanco de subida: muestreo */
            KeStallExecutionProcessor(2);
            r = (UCHAR)((r << 1) | GpIn(c, 6));
            if (g_bbMode == 0) GpOut(c, 8, 0);
        }
        rx[i] = r;
    }
    GpOut(c, 8, g_bbMode == 3);
    KeStallExecutionProcessor(5);
    GpOut(c, 9, !g_bbCsHigh);                   /* CS inactivo */
    KeStallExecutionProcessor(10);
}

static VOID BbSetPage(PDEVICE_CONTEXT c, ULONG a)
{
    UCHAR t[3] = { 0xFF, (UCHAR)(a >> 15), (UCHAR)(a >> 7) }, r[3]; BbXfer(c, t, r, 3);
}
static VOID BbWr(PDEVICE_CONTEXT c, ULONG a, UCHAR d)
{
    UCHAR t[2] = { (UCHAR)((a & 0x7F) | 0x80), d }, r[2]; BbSetPage(c, a); BbXfer(c, t, r, 2);
}

static ULONG NvtU32(const UCHAR* d, ULONG o)
{
    return (ULONG)d[o] | ((ULONG)d[o + 1] << 8) | ((ULONG)d[o + 2] << 16) | ((ULONG)d[o + 3] << 24);
}

/* Devuelve 0 si ok. Misma logica que nvt_bin_header_parser. */
static int NvtParseBin(const UCHAR* d, ULONG len)
{
    ULONG i, end, pos, info = 0, ovly = 0, need = 0;

    if (len < 0x1000) return -1;
    for (i = len / 4096; i > 0; i--) {
        const UCHAR* t = d + i * 4096 - 3;
        if ((t[0] == 'N' && t[1] == 'V' && t[2] == 'T') ||
            (t[0] == 'M' && t[1] == 'O' && t[2] == 'D')) {
            need = i * 4096; break;
        }
    }
    if (!need) return -2;
    if ((UCHAR)(d[need - 4096] + d[need - 4096 + 1]) != 0xFF) return -3;

    end = NvtU32(d, 0);
    for (pos = 0x30; pos < end; pos += 0x10) info++;
    if (d[0x28] & 0x10) ovly = d[0x28] & 0x0F;

    g_PartCnt = 2 + ovly + info;
    if (g_PartCnt > NVT_MAX_PART) return -4;

    for (i = 0; i < g_PartCnt; i++) {
        if (i < 2) {
            g_Part[i].Bin = NvtU32(d, i * 12);
            g_Part[i].Sram = NvtU32(d, i * 12 + 4);
            g_Part[i].Size = NvtU32(d, i * 12 + 8);
            g_Part[i].Crc = NvtU32(d, 0x18 + i * 4);
        }
        else if (i < 2 + info) {
            ULONG p = 0x30 + 0x10 * (i - 2);
            g_Part[i].Sram = NvtU32(d, p);
            g_Part[i].Size = NvtU32(d, p + 4);
            g_Part[i].Bin = NvtU32(d, p + 8);
            g_Part[i].Crc = NvtU32(d, p + 12);
        }
        else {
            ULONG p = g_Part[1].Bin + 0x10 * (i - 2 - info);
            g_Part[i].Sram = NvtU32(d, p);
            g_Part[i].Size = NvtU32(d, p + 4);
            g_Part[i].Bin = NvtU32(d, p + 8);
            g_Part[i].Crc = NvtU32(d, p + 12);
        }
    }
    return 0;
}

#ifndef VIZ_HOST_TEST

/* ------------------------- buffers y utilidades ------------------------ */
static PUCHAR g_Tx = NULL, g_Rx = NULL;     /* NVT_CHUNK + 8 */

static BOOLEAN NvtAllocBufs(VOID)
{
    if (!g_Tx) g_Tx = (PUCHAR)ExAllocatePoolWithTag(NonPagedPoolNx, NVT_CHUNK + 8, 'xTzV');
    if (!g_Rx) g_Rx = (PUCHAR)ExAllocatePoolWithTag(NonPagedPoolNx, NVT_CHUNK + 8, 'xRzV');
    return g_Tx && g_Rx;
}

/* version silenciosa de set_page */
static NTSTATUS NqPage(PDEVICE_CONTEXT c, ULONG addr)
{
    UCHAR t[3], r[3];
    t[0] = 0xFF; t[1] = (UCHAR)(addr >> 15); t[2] = (UCHAR)(addr >> 7);
    return VizSpiXfer(c, t, r, 3);
}

/* escribe n bytes (buf[0] = offset, buf[1..] = datos) */
static NTSTATUS NqWriteBuf(PDEVICE_CONTEXT c, UCHAR* buf, ULONG len)
{
    buf[0] = (buf[0] & 0x7F) | 0x80;
    return VizSpiXfer(c, buf, g_Rx, len);
}

static NTSTATUS NqWrite(PDEVICE_CONTEXT c, ULONG addr, UCHAR data)
{
    UCHAR b[2];
    NTSTATUS st = NqPage(c, addr);
    if (!NT_SUCCESS(st)) return st;
    b[0] = (UCHAR)(addr & 0x7F); b[1] = data;
    return NqWriteBuf(c, b, 2);
}

/* lee n datos desde 'off' dentro de la pagina ya fijada */
static NTSTATUS NqRead(PDEVICE_CONTEXT c, UCHAR off, UCHAR* out, ULONG n)
{
    NTSTATUS st;
    if (n == 0 || n > NVT_CHUNK) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(g_Tx, n + 2);
    g_Tx[0] = off & 0x7F;
    st = VizSpiXfer(c, g_Tx, g_Rx, n + 2);
    if (NT_SUCCESS(st)) RtlCopyMemory(out, g_Rx + 2, n);
    return st;
}

/* ------------------------- carga del .bin ------------------------------ */
static NTSTATUS NvtLoadFile(PUCHAR* out, ULONG* outLen)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\??\\C:\\Windows\\System32\\Drivers\\novatek_ts_tianma_fw.bin");
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    FILE_STANDARD_INFORMATION fi;
    HANDLE h;
    PUCHAR buf;
    ULONG size;
    NTSTATUS st;

    InitializeObjectAttributes(&oa, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    st = ZwCreateFile(&h, FILE_GENERIC_READ, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(st)) { VizLog("fw: no se pudo abrir el .bin st=%08lx", (ULONG)st); return st; }

    st = ZwQueryInformationFile(h, &iosb, &fi, sizeof(fi), FileStandardInformation);
    size = NT_SUCCESS(st) ? (ULONG)fi.EndOfFile.QuadPart : 0;
    if (size < 0x1000 || size > 0x100000) { ZwClose(h); return STATUS_INVALID_IMAGE_FORMAT; }

    buf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPoolNx, size, 'wFzV');
    if (!buf) { ZwClose(h); return STATUS_INSUFFICIENT_RESOURCES; }

    st = ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, size, NULL, NULL);
    ZwClose(h);
    if (!NT_SUCCESS(st) || iosb.Information != size) {
        ExFreePoolWithTag(buf, 'wFzV');
        return NT_SUCCESS(st) ? STATUS_UNSUCCESSFUL : st;
    }
    *out = buf; *outLen = size;
    return STATUS_SUCCESS;
}

/* ------------------------- pasos de la descarga ------------------------ */
static NTSTATUS NvtWriteSram(PDEVICE_CONTEXT c, const UCHAR* fw, ULONG sram, ULONG size, ULONG bin)
{
    while (size) {
        ULONG n = size < NVT_CHUNK ? size : NVT_CHUNK;
        NTSTATUS st = NqPage(c, sram);
        if (!NT_SUCCESS(st)) return st;
        g_Tx[0] = (UCHAR)(sram & 0x7F);
        RtlCopyMemory(g_Tx + 1, fw + bin, n);
        st = NqWriteBuf(c, g_Tx, n + 1);
        if (!NT_SUCCESS(st)) return st;
        sram += n; bin += n; size -= n;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NvtWriteFirmware(PDEVICE_CONTEXT c, const UCHAR* fw, ULONG fwLen)
{
    ULONG i;
    for (i = 0; i < g_PartCnt; i++) {
        NTSTATUS st;
        if (g_Part[i].Bin + g_Part[i].Size > fwLen) return STATUS_INVALID_IMAGE_FORMAT;
        if (!g_Part[i].Size) continue;               /* particion reservada */
        st = NvtWriteSram(c, fw, g_Part[i].Sram, g_Part[i].Size + 1, g_Part[i].Bin);
        if (!NT_SUCCESS(st)) { VizLog("fw: part %lu fallo st=%08lx", i, (ULONG)st); return st; }
    }
    return STATUS_SUCCESS;
}

static VOID NvtSetBldBank(PDEVICE_CONTEXT c, ULONG des, ULONG sram, ULONG lenAddr, ULONG size,
    ULONG crcAddr, ULONG crc)
{
    UCHAR b[5];
    NqPage(c, des);
    b[0] = (UCHAR)(des & 0x7F); b[1] = (UCHAR)sram; b[2] = (UCHAR)(sram >> 8); b[3] = (UCHAR)(sram >> 16);
    NqWriteBuf(c, b, 4);
    b[0] = (UCHAR)(lenAddr & 0x7F); b[1] = (UCHAR)size; b[2] = (UCHAR)(size >> 8);   /* hw_crc == 1: 2 bytes */
    NqWriteBuf(c, b, 3);
    b[0] = (UCHAR)(crcAddr & 0x7F);
    b[1] = (UCHAR)crc; b[2] = (UCHAR)(crc >> 8); b[3] = (UCHAR)(crc >> 16); b[4] = (UCHAR)(crc >> 24);
    NqWriteBuf(c, b, 5);
}

static NTSTATUS NvtCheckResetState(PDEVICE_CONTEXT c, ULONG want, ULONG retryMax)
{
    UCHAR b[6];
    ULONG retry = 0;
    NqPage(c, NVT_EVENT_BUF | NVT_EM_RESET_CPL);
    for (;;) {
        RtlZeroMemory(b, sizeof(b));
        NqRead(c, NVT_EM_RESET_CPL, b, 5);          /* b[0] = estado (Linux: buf[1]) */
        if (b[0] >= want && b[0] <= NVT_RST_MAX) return STATUS_SUCCESS;
        if (++retry > retryMax) {
            VizLog("fw: reset state timeout: %02lx %02lx %02lx %02lx %02lx",
                (ULONG)b[0], (ULONG)b[1], (ULONG)b[2], (ULONG)b[3], (ULONG)b[4]);
            return STATUS_IO_TIMEOUT;
        }
        VzDelayMs(10);
    }
}

static NTSTATUS NvtDownloadFw(PDEVICE_CONTEXT c, const UCHAR* fw, ULONG fwLen)
{
    ULONG attempt;
    NTSTATUS st = STATUS_UNSUCCESSFUL;

    for (attempt = 0; attempt < 3; attempt++) {
        UCHAR b[2];

        /* bootloader reset */
        NqWrite(c, NVT_SWRST_N8, 0x69);
        VzDelayMs(5);
        NqWrite(c, NVT_SPI_RD_FAST, 0x00);

        st = NvtWriteFirmware(c, fw, fwLen);
        if (!NT_SUCCESS(st)) continue;

        /* CRC por hardware: ILM y DLM */
        NvtSetBldBank(c, NVT_ILM_DES, g_Part[0].Sram, NVT_ILM_LEN, g_Part[0].Size, NVT_G_ILM_CRC, g_Part[0].Crc);
        NvtSetBldBank(c, NVT_DLM_DES, g_Part[1].Sram, NVT_DLM_LEN, g_Part[1].Size, NVT_G_DLM_CRC, g_Part[1].Crc);

        /* nvt_bld_crc_enable: bit7 de BLD_CRC_EN */
        NqPage(c, NVT_BLD_CRC_EN);
        b[1] = 0xFF;
        NqRead(c, (UCHAR)(NVT_BLD_CRC_EN & 0x7F), &b[1], 1);
        b[0] = (UCHAR)(NVT_BLD_CRC_EN & 0x7F);
        b[1] |= 0x80;
        NqWriteBuf(c, b, 2);

        /* nvt_fw_crc_enable: limpia estado de reset y manda 0xAE */
        NqPage(c, NVT_EVENT_BUF);
        b[0] = NVT_EM_RESET_CPL; b[1] = 0x00; NqWriteBuf(c, b, 2);
        b[0] = NVT_EM_HOST_CMD;  b[1] = 0xAE; NqWriteBuf(c, b, 2);

        /* nvt_boot_ready (hw_crc != 0: solo BOOT_RDY = 1) */
        NqWrite(c, NVT_BOOT_RDY, 1);
        VzDelayMs(5);

        st = NvtCheckResetState(c, NVT_RST_INIT, 10);
        VizLog("fw: intento %lu st=%08lx", attempt, (ULONG)st);
        if (NT_SUCCESS(st)) break;
    }
    return st;
}

static VOID NvtGetFwInfo(PDEVICE_CONTEXT c)
{
    UCHAR b[16];
    RtlZeroMemory(b, sizeof(b));
    NqPage(c, NVT_EVENT_BUF | NVT_EM_FWINFO);
    NqRead(c, NVT_EM_FWINFO, b, 16);            /* b[0] = buf[1] de Linux */
    VizLog("fwinfo: ver=%02lx ~ver=%02lx x_num=%lu y_num=%lu max=%lux%lu btn=%lu tipo=%02lx",
        (ULONG)b[0], (ULONG)b[1], (ULONG)b[2], (ULONG)b[3],
        (ULONG)((b[4] << 8) | b[5]), (ULONG)((b[6] << 8) | b[7]),
        (ULONG)b[10], (ULONG)b[13]);
    NqPage(c, NVT_EVENT_BUF);                   /* dejar la pagina lista para los eventos */
}

static PUCHAR g_Fw = NULL;
static ULONG  g_FwLen = 0;

static NTSTATUS NvtBringUp(PDEVICE_CONTEXT c)
{
    NTSTATUS st;
    if (!NvtAllocBufs()) return STATUS_INSUFFICIENT_RESOURCES;

    if (!g_Fw) {                                  /* se carga y parsea una sola vez */
        st = NvtLoadFile(&g_Fw, &g_FwLen);
        if (!NT_SUCCESS(st)) { g_Fw = NULL; return st; }
        if (NvtParseBin(g_Fw, g_FwLen)) {
            ExFreePoolWithTag(g_Fw, 'wFzV'); g_Fw = NULL;
            return STATUS_INVALID_IMAGE_FORMAT;
        }
        VizLog("fw: len=%lu particiones=%lu", g_FwLen, g_PartCnt);
    }

    st = NvtDownloadFw(c, g_Fw, g_FwLen);
    if (!NT_SUCCESS(st)) return st;
    NvtGetFwInfo(c);
    return STATUS_SUCCESS;
}

static NTSTATUS VizTouchStart(PDEVICE_CONTEXT c)
{
    VizEnableClocks(c);
    VizSpiPinmux(c);
    VizGeniInit(c);
    VizSpiSetup(c);
    return NvtBringUp(c);
}

/* ------------------------- lectura de eventos -------------------------- */
/* Devuelve TRUE si hay que enviar un reporte. Llamar a DISPATCH_LEVEL OK. */
static BOOLEAN NvtReadTouch(PDEVICE_CONTEXT c, ULONG* prevMask, VIZ_TOUCH_REPORT* R)
{
    UCHAR p[NVT_POINT_LEN];
    ULONG cur = 0, i, n = 0;

    RtlZeroMemory(R, sizeof(*R));
    R->ReportId = VIZ_REPORT_ID_TOUCH;

    if (!NT_SUCCESS(NqRead(c, 0x00, p, NVT_POINT_LEN))) return FALSE;

    for (i = 0; i < VIZ_MAX_CONTACTS; i++) {
        ULONG pos = i * 6;
        ULONG id = p[pos] >> 3;
        ULONG status = p[pos] & 0x07;
        ULONG x, y;

        if (id < 1 || id > VIZ_MAX_CONTACTS) continue;
        if (status != 1 && status != 2) continue;           /* 1 = entra, 2 = mueve */

        x = ((ULONG)p[pos + 1] << 4) + (p[pos + 3] >> 4);
        y = ((ULONG)p[pos + 2] << 4) + (p[pos + 3] & 0x0F);
        if (x > VIZ_X_MAX) x = VIZ_X_MAX;
        if (y > VIZ_Y_MAX) y = VIZ_Y_MAX;

        if (n < VIZ_MAX_CONTACTS) {
            R->Finger[n].Flags = 0x03;                       /* tip + confidence */
            R->Finger[n].ContactId = (UCHAR)(id - 1);
            R->Finger[n].X = (USHORT)x;
            R->Finger[n].Y = (USHORT)y;
            n++;
        }
        cur |= 1u << (id - 1);
    }

    /* dedos que se levantaron desde el reporte anterior */
    for (i = 0; i < VIZ_MAX_CONTACTS; i++) {
        if ((*prevMask & (1u << i)) && !(cur & (1u << i)) && n < VIZ_MAX_CONTACTS) {
            R->Finger[n].Flags = 0x02;                       /* confidence, sin tip */
            R->Finger[n].ContactId = (UCHAR)i;
            n++;
        }
    }

    R->ContactCount = (UCHAR)n;
    *prevMask = cur;
    return n != 0;
}

#endif /* VIZ_HOST_TEST */

/* Devuelve TRUE si hay un reporte nuevo en *R. */
static BOOLEAN VizReadTouch(_In_ PDEVICE_CONTEXT c, _Out_ VIZ_TOUCH_REPORT* R)
{
    RtlZeroMemory(R, sizeof(*R));
    R->ReportId = VIZ_REPORT_ID_TOUCH;

#if VIZ_TEST_FAKE_TOUCH
    {
        ULONG t = c->Tick++ % 200;
        if (t < 150) {
            R->Finger[0].Flags = 0x03;
            R->Finger[0].ContactId = 0;
            R->Finger[0].X = (USHORT)((t * VIZ_X_MAX) / 150);
            R->Finger[0].Y = (USHORT)((t * VIZ_Y_MAX) / 150);
            R->ContactCount = 1;
            return TRUE;
        }
        if (t == 150) {
            R->Finger[0].Flags = 0x02;
            R->Finger[0].ContactId = 0;
            R->ContactCount = 1;
            return TRUE;
        }
        return FALSE;
    }
#else
    if (!c->Ready) return FALSE;
    return NvtReadTouch(c, &c->PrevMask, R);
#endif
}

/* ------------------------- manejo de IOCTL HID ------------------------- */

static NTSTATUS HidGetDeviceDescriptor(WDFREQUEST Request, size_t* Info)
{
    PVOID buf; NTSTATUS st;
    st = WdfRequestRetrieveOutputBuffer(Request, sizeof(HID_DESCRIPTOR), &buf, NULL);
    if (!NT_SUCCESS(st)) return st;

    HID_DESCRIPTOR d;
    RtlZeroMemory(&d, sizeof(d));
    d.bLength = sizeof(HID_DESCRIPTOR);
    d.bDescriptorType = HID_HID_DESCRIPTOR_TYPE;
    d.bcdHID = HID_REVISION;
    d.bCountry = 0;
    d.bNumDescriptors = 1;
    d.DescriptorList[0].bReportType = HID_REPORT_DESCRIPTOR_TYPE;
    d.DescriptorList[0].wReportLength = (USHORT)sizeof(g_VizReportDesc);

    RtlCopyMemory(buf, &d, sizeof(d));
    *Info = sizeof(d);
    return STATUS_SUCCESS;
}

static NTSTATUS HidGetReportDescriptor(WDFREQUEST Request, size_t* Info)
{
    PVOID buf; NTSTATUS st;
    st = WdfRequestRetrieveOutputBuffer(Request, sizeof(g_VizReportDesc), &buf, NULL);
    if (!NT_SUCCESS(st)) return st;
    RtlCopyMemory(buf, g_VizReportDesc, sizeof(g_VizReportDesc));
    *Info = sizeof(g_VizReportDesc);
    return STATUS_SUCCESS;
}

static NTSTATUS HidGetAttributes(WDFREQUEST Request, size_t* Info)
{
    PVOID buf; NTSTATUS st;
    st = WdfRequestRetrieveOutputBuffer(Request, sizeof(HID_DEVICE_ATTRIBUTES), &buf, NULL);
    if (!NT_SUCCESS(st)) return st;

    HID_DEVICE_ATTRIBUTES a;
    RtlZeroMemory(&a, sizeof(a));
    a.Size = sizeof(a);
    a.VendorID = VIZ_VENDOR_ID;
    a.ProductID = VIZ_PRODUCT_ID;
    a.VersionNumber = VIZ_VERSION;
    RtlCopyMemory(buf, &a, sizeof(a));
    *Info = sizeof(a);
    return STATUS_SUCCESS;
}

static NTSTATUS HidGetFeature(WDFREQUEST Request, size_t* Info)
{
    PIRP irp = WdfRequestWdmGetIrp(Request);
    PHID_XFER_PACKET p = (PHID_XFER_PACKET)irp->UserBuffer;

    if (p == NULL || p->reportBuffer == NULL || p->reportBufferLen < 2)
        return STATUS_INVALID_PARAMETER;

    if (p->reportId == VIZ_REPORT_ID_MAXCNT) {
        p->reportBuffer[0] = VIZ_REPORT_ID_MAXCNT;
        p->reportBuffer[1] = VIZ_MAX_CONTACTS;
        *Info = 2;
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}

VOID EvtIoInternalDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request,
    _In_ size_t OutLen, _In_ size_t InLen, _In_ ULONG Code)
{
    UNREFERENCED_PARAMETER(OutLen);
    UNREFERENCED_PARAMETER(InLen);

    PDEVICE_CONTEXT c = GetCtx(WdfIoQueueGetDevice(Queue));
    NTSTATUS st = STATUS_NOT_SUPPORTED;
    size_t info = 0;

    switch (Code) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:  st = HidGetDeviceDescriptor(Request, &info); break;
    case IOCTL_HID_GET_REPORT_DESCRIPTOR:  st = HidGetReportDescriptor(Request, &info); break;
    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:  st = HidGetAttributes(Request, &info);       break;
    case IOCTL_HID_GET_FEATURE:            st = HidGetFeature(Request, &info);          break;

    case IOCTL_HID_READ_REPORT:
        st = WdfRequestForwardToIoQueue(Request, c->ReadQueue);
        if (NT_SUCCESS(st)) return;
        break;

    case IOCTL_HID_ACTIVATE_DEVICE:
    case IOCTL_HID_DEACTIVATE_DEVICE:
        st = STATUS_SUCCESS;
        break;

    default:
        break;
    }

    WdfRequestCompleteWithInformation(Request, st, info);
}

VOID EvtPollTimer(_In_ WDFTIMER Timer)
{
    PDEVICE_CONTEXT c = GetCtx(WdfTimerGetParentObject(Timer));
    WDFREQUEST req;
    PVOID out;
    NTSTATUS st;

    if (!c->Ready) return;
    if (InterlockedCompareExchange(&c->Busy, 1, 0)) return;

    if (!c->HavePending && VizReadTouch(c, &c->Pending))
        c->HavePending = TRUE;

    if (c->HavePending) {
        st = WdfIoQueueRetrieveNextRequest(c->ReadQueue, &req);
        if (NT_SUCCESS(st)) {
            st = WdfRequestRetrieveOutputBuffer(req, sizeof(c->Pending), &out, NULL);
            if (NT_SUCCESS(st)) {
                RtlCopyMemory(out, &c->Pending, sizeof(c->Pending));
                WdfRequestCompleteWithInformation(req, STATUS_SUCCESS, sizeof(c->Pending));
            }
            else {
                WdfRequestComplete(req, st);
            }
            c->HavePending = FALSE;
        }
    }
    InterlockedExchange(&c->Busy, 0);
}

/* ------------------------------ PnP ------------------------------------ */

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}

NTSTATUS EvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_IO_QUEUE_CONFIG qc;
    WDF_TIMER_CONFIG tc;
    WDFDEVICE dev;
    NTSTATUS st;

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = EvtReleaseHardware;
    pnp.EvtDeviceD0Entry = EvtD0Entry;
    pnp.EvtDeviceD0Exit = EvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WdfFdoInitSetFilter(DeviceInit);     /* no somos power policy owner: lo es HIDCLASS */

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEVICE_CONTEXT);
    st = WdfDeviceCreate(&DeviceInit, &attr, &dev);

    WDF_TIMER_CONFIG_INIT_PERIODIC(&tc, EvtPollTimer, VIZ_POLL_MS);
    tc.AutomaticSerialization = FALSE;
    if (!NT_SUCCESS(st)) return st;

    PDEVICE_CONTEXT c = GetCtx(dev);

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qc, WdfIoQueueDispatchParallel);
    qc.EvtIoInternalDeviceControl = EvtIoInternalDeviceControl;
    st = WdfIoQueueCreate(dev, &qc, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(st)) return st;

    WDF_IO_QUEUE_CONFIG_INIT(&qc, WdfIoQueueDispatchManual);
    st = WdfIoQueueCreate(dev, &qc, WDF_NO_OBJECT_ATTRIBUTES, &c->ReadQueue);
    if (!NT_SUCCESS(st)) return st;

    WDF_TIMER_CONFIG_INIT_PERIODIC(&tc, EvtPollTimer, VIZ_POLL_MS);
    tc.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = dev;
    return WdfTimerCreate(&tc, &attr, &c->PollTimer);
}

static PVOID MapRes(PCM_PARTIAL_RESOURCE_DESCRIPTOR d, SIZE_T* len)
{
    *len = d->u.Memory.Length;
    return MmMapIoSpaceEx(d->u.Memory.Start, d->u.Memory.Length, PAGE_READWRITE | PAGE_NOCACHE);
}

NTSTATUS EvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw, _In_ WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Raw);
    PDEVICE_CONTEXT c = GetCtx(Device);
    ULONG memIdx = 0;

    VizMarker(L"prep", 0, 0);
    g_BufLen = 0;

    if (VIZ_LOG_PHYS_BASE) {
        PHYSICAL_ADDRESS pa; pa.QuadPart = (LONGLONG)VIZ_LOG_PHYS_BASE;
        c->Log = MmMapIoSpaceEx(pa, VIZ_LOG_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
        g_Log = c->Log;
        if (g_Log) { VIZ_LOG_HDR* h = g_Log; h->Magic = 'ZIVL'; h->Head = 0; }
    }

    for (ULONG i = 0; i < WdfCmResourceListGetCount(Translated); i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(Translated, i);
        if (d->Type != CmResourceTypeMemory) continue;
        switch (memIdx++) {
        case 0: c->Se = MapRes(d, &c->SeLen);   break;
        case 1: c->Tlmm = MapRes(d, &c->TlmmLen); break;
        case 2: c->Gcc = MapRes(d, &c->GccLen);  break;
        }
    }
    VizLog("mapeado SE2=%p TLMM=%p GCC=%p", c->Se, c->Tlmm, c->Gcc);

#if !VIZ_TEST_FAKE_TOUCH
    if (!c->Se || !c->Tlmm || !c->Gcc) return STATUS_DEVICE_CONFIGURATION_ERROR;
    {
        NTSTATUS fs = VizTouchStart(c);
        VizLog("VizTouchStart st=%08lx", (ULONG)fs);
        c->Ready = NT_SUCCESS(fs);
        VizFlushLog();
    }
#endif
    return STATUS_SUCCESS;
}

NTSTATUS EvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Translated);
    PDEVICE_CONTEXT c = GetCtx(Device);
    if (c->Se)   MmUnmapIoSpace(c->Se, c->SeLen);
    if (c->Tlmm) MmUnmapIoSpace(c->Tlmm, c->TlmmLen);
    if (c->Gcc)  MmUnmapIoSpace(c->Gcc, c->GccLen);
    if (c->Log) { g_Log = NULL; MmUnmapIoSpace(c->Log, VIZ_LOG_SIZE); }
    c->Se = c->Tlmm = c->Gcc = c->Log = NULL;
    return STATUS_SUCCESS;
}

NTSTATUS EvtD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE Prev)
{
    PDEVICE_CONTEXT c = GetCtx(Device);
    c->Tick = 0;
    c->PrevMask = 0;
    c->HavePending = FALSE;

    /* En el primer arranque PrepareHardware ya lo hizo; al volver de D3/D2 hay que repetirlo */
    if (Prev != WdfPowerDeviceD3Final && c->Se && c->Tlmm && c->Gcc) {
        NTSTATUS st = VizTouchStart(c);
        VizLog("resume st=%08lx", (ULONG)st);
        c->Ready = NT_SUCCESS(st);
        VizFlushLog();
    }
    WdfTimerStart(c->PollTimer, WDF_REL_TIMEOUT_IN_MS(VIZ_POLL_MS));
    return STATUS_SUCCESS;
}

NTSTATUS EvtD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE Target)
{
    PDEVICE_CONTEXT c = GetCtx(Device);
    UNREFERENCED_PARAMETER(Target);
    WdfTimerStop(c->PollTimer, TRUE);
    c->Ready = FALSE;
    return STATUS_SUCCESS;
}