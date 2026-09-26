/* resource.h - control identifiers for xbconfig.rc. */

#ifndef XB_RESOURCE_H
#define XB_RESOURCE_H

#define IDD_MAIN                100
#define IDD_BIND                101
#define IDD_CAPTURE             102
#define IDD_NEWNAME             103
#define IDI_APP                 104
#define IDA_MAIN                110

/* Main dialog. */
#define IDC_PROFILES            1000
#define IDC_ADD                 1001
#define IDC_APPLY               1002
#define IDC_LAYER               1003
#define IDC_STATUS              1004
#define IDC_PADGROUP            1005
#define IDC_PROFILEGROUP        1006
#define IDC_LAYERLABEL          1007
#define IDC_STICKS              1008
#define IDC_REFRESH             1009
#define IDC_TOGGLELAYER         1010
#define IDC_RESETPAD            1011
#define IDC_KEYHELP             1012

/* Binding dialog. */
#define IDC_CONTROL             1100
#define IDC_ACTION              1101
#define IDC_CODE                1102
#define IDC_CAPTURE             1103
#define IDC_REPEAT              1104
#define IDC_TOGGLE              1105
#define IDC_ANALOG              1106
#define IDC_PASSTHROUGH         1107
#define IDC_HZ                  1108
#define IDC_DELAY               1109
#define IDC_HARD                1110
#define IDC_PREVIEW             1111
#define IDC_CODELABEL           1112
#define IDC_HZLABEL             1113
#define IDC_DELAYLABEL          1114
#define IDC_HARDLABEL           1115
#define IDC_CLEAR               1116

/* Capture and new-name dialogs. */
#define IDC_CAPTURED            1200
#define IDC_NEWNAME             1201

/* Stick dialog. */
#define IDD_STICK               105
#define IDC_ST_WHICH            1300
#define IDC_ST_MODE             1301
#define IDC_ST_DEADZONE         1302
#define IDC_ST_OUTER            1303
#define IDC_ST_MAXSPEED         1304
#define IDC_ST_SPEEDUNIT        1305
#define IDC_ST_CURVE            1306
#define IDC_ST_GAINX            1307
#define IDC_ST_GAINY            1308
#define IDC_ST_INVX             1309
#define IDC_ST_INVY             1310
#define IDC_ST_SMOOTH           1311
#define IDC_ST_ATHRESH          1312
#define IDC_ST_ARATE            1313
#define IDC_ST_AMAX             1314
#define IDC_ST_ADECAY           1315
#define IDC_ST_HINT             1316
#define IDC_ST_GRAPH            1317
#define IDC_ST_DEFAULT          1318
#define IDC_ST_OFF              1319

/* Layer dialog. */
#define IDD_LAYER               106
#define IDD_CHORD               107
#define IDC_LY_WHICH            1400
#define IDC_LY_BTN1             1401
#define IDC_LY_BTN2             1402
#define IDC_LY_PREVIEW          1403
#define IDC_LY_HINT             1404
#define IDC_LY_CLEAR            1405

/* Chord dialog. */
#define IDC_CH_WHICH            1420
#define IDC_CH_M1               1421
#define IDC_CH_M2               1422
#define IDC_CH_M3               1423
#define IDC_CH_ACTION           1424
#define IDC_CH_CODE             1425
#define IDC_CH_CAPTURE          1426
#define IDC_CH_TOGGLE           1427
#define IDC_CH_REPEAT           1428
#define IDC_CH_HZ               1429
#define IDC_CH_DELAY            1430
#define IDC_CH_PREVIEW          1431
#define IDC_CH_CLEAR            1432

/*
 * The pad control buttons are created at RUNTIME from a table keyed by
 * the CORE_SA_* constants rather than listed here, so a control cannot
 * exist on the dialog without existing in core.h. These are the range
 * they are allocated from.
 */
#define IDC_PAD_BASE            1500
#define IDC_PAD_LAST            1599

#endif  /* XB_RESOURCE_H */
