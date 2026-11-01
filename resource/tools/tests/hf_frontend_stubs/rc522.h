#ifndef RC522_H
#define RC522_H
#define PCD_IDLE 0x00
#define PCD_RECEIVE 0x08
#define PCD_RESET 0x0f
#define CommandReg 0x01
#define ComIrqReg 0x04
#define FIFOLevelReg 0x0a
#define ModeReg 0x11
#define TxModeReg 0x12
#define RxModeReg 0x13
#define TxControlReg 0x14
#define TxAutoReg 0x15
#define TxSelReg 0x16
#define RxSelReg 0x17
#define RxThresholdReg 0x18
#define DemodReg 0x19
#define RFCfgReg 0x26
#define GsNReg 0x27
#define CWGsCfgReg 0x28
#define ModGsCfgReg 0x29
#define TModeReg 0x2a
#define TestADCReg 0x3b
void pcd_14a_reader_antenna_off(void);
void pcd_14a_reader_uninit(void);
void pcd_14a_reader_init(void);
#endif
