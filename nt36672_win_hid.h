/*
 * nt36672_win_hid.h - Descriptor HID de pantalla tactil multitouch (10 contactos)
 * para el NT36672A del Redmi Note 8 (1080x2340).
 *
 * SIN PROBAR. Dimensiones fisicas aproximadas (panel de 6.3"):
 * 67.1 x 145.3 mm, en unidades de 0.1 mm. Ajustalas si Windows escala mal.
 */
#pragma once

#define VIZ_MAX_CONTACTS   10
#define VIZ_REPORT_ID_TOUCH 1
#define VIZ_REPORT_ID_MAXCNT 2

#define VIZ_X_MAX 1080
#define VIZ_Y_MAX 2340

/* Un dedo: 8 bits de flags + ID + X + Y = 6 bytes */
#define VIZ_FINGER_DESC                                                     \
    0x09, 0x22, 0xA1, 0x02,              /* Usage Finger, Collection Logical */ \
    0x09, 0x42, 0x15, 0x00, 0x25, 0x01,  /* Tip Switch, 0..1 */                \
    0x75, 0x01, 0x95, 0x01, 0x81, 0x02,  /* 1 bit, Input */                    \
    0x09, 0x47, 0x81, 0x02,              /* Confidence */                      \
    0x95, 0x06, 0x81, 0x03,              /* 6 bits de relleno */               \
    0x09, 0x51, 0x75, 0x08, 0x95, 0x01,  /* Contact ID, 8 bits */              \
    0x25, 0x7F, 0x81, 0x02,                                                    \
    0x05, 0x01, 0x09, 0x30,              /* Generic Desktop: X */              \
    0x75, 0x10, 0x95, 0x01,                                                    \
    0x35, 0x00, 0x46, 0x9F, 0x02,        /* Physical 0..671 (0.1 mm) */        \
    0x65, 0x11, 0x55, 0x0E,              /* Unit cm, exponente -2 */           \
    0x15, 0x00, 0x26, 0x37, 0x04,        /* Logical 0..1079 */                 \
    0x81, 0x02,                                                                \
    0x09, 0x31,                          /* Y */                               \
    0x46, 0xAD, 0x05,                    /* Physical 0..1453 */                \
    0x26, 0x23, 0x09,                    /* Logical 0..2339 */                 \
    0x81, 0x02,                                                                \
    0x05, 0x0D,                          /* volver a Digitizers */             \
    0xC0

static const UCHAR g_VizReportDesc[] = {
    0x05, 0x0D,                          /* Usage Page Digitizers */
    0x09, 0x04,                          /* Usage Touch Screen */
    0xA1, 0x01,                          /* Collection Application */
    0x85, VIZ_REPORT_ID_TOUCH,

    VIZ_FINGER_DESC, VIZ_FINGER_DESC, VIZ_FINGER_DESC, VIZ_FINGER_DESC,
    VIZ_FINGER_DESC, VIZ_FINGER_DESC, VIZ_FINGER_DESC, VIZ_FINGER_DESC,
    VIZ_FINGER_DESC, VIZ_FINGER_DESC,

    0x35, 0x00, 0x45, 0x00, 0x65, 0x00, 0x55, 0x00,   /* limpiar fisico/unidad */
    0x05, 0x0D, 0x09, 0x54,              /* Contact Count */
    0x15, 0x00, 0x25, VIZ_MAX_CONTACTS,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x02,

    0x85, VIZ_REPORT_ID_MAXCNT,          /* Feature: Contact Count Maximum */
    0x09, 0x55, 0x25, VIZ_MAX_CONTACTS,
    0xB1, 0x02,

    0xC0
};

#pragma pack(push, 1)
typedef struct _VIZ_FINGER {
    UCHAR  Flags;       /* bit0 = tip, bit1 = confidence */
    UCHAR  ContactId;
    USHORT X;
    USHORT Y;
} VIZ_FINGER;

typedef struct _VIZ_TOUCH_REPORT {
    UCHAR      ReportId;                       /* = VIZ_REPORT_ID_TOUCH */
    VIZ_FINGER Finger[VIZ_MAX_CONTACTS];
    UCHAR      ContactCount;
} VIZ_TOUCH_REPORT;
#pragma pack(pop)
