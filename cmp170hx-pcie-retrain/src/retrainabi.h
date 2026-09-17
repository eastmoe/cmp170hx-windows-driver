/* SPDX-License-Identifier: GPL-2.0-only
 *
 * retrainabi.h — cmpretrain 驱动与 retrainctl 用户态共享 ABI。
 * 双方用同一 GUID 文本与结构布局；IOCTL 为 METHOD_BUFFERED。
 */
#ifndef CMPRT_ABI_H
#define CMPRT_ABI_H

#include "retrain_core.h"

/* {3F9E4D27-8C1A-4B6D-9E2F-1A5C7D0B4E83} */
#define CMPRT_INTERFACE_GUID_INIT \
    { 0x3F9E4D27, 0x8C1A, 0x4B6D, { 0x9E, 0x2F, 0x1A, 0x5C, 0x7D, 0x0B, 0x4E, 0x83 } }

#define CMPRT_DEVICE_TYPE  0x8F20u
#define CMPRT_IOCTL_RUN    0x800u

#define IOCTL_CMPRT_RUN \
    (((CMPRT_DEVICE_TYPE) << 16) | ((FILE_ANY_ACCESS) << 14) | ((CMPRT_IOCTL_RUN) << 2) | (METHOD_BUFFERED))

#define CMPRT_FLAG_APPLY  0x00000001u  /* 0=只读查询；1=执行策略补写+重训练 */

#pragma pack(push, 8)
typedef struct {
    unsigned int Segment;        /* PCI segment，通常 0 */
    unsigned int GpuBus, GpuDevice, GpuFunction;
    unsigned int RootBus, RootDevice, RootFunction;  /* 直接上游桥 */
    unsigned int Flags;          /* CMPRT_FLAG_* */
    unsigned int Reserved[3];
} CMPRT_REQUEST;

/* 结果直接复用核心结构(布局固定、无指针) */
typedef cmprt_result CMPRT_RESULT;
#pragma pack(pop)

/* WDM 服务模式：设备名/符号链接/Win32 路径/服务名 */
#define CMPRT_NT_DEVICE_NAME  L"\\Device\\CmpRetrain"
#define CMPRT_SYMLINK_NAME    L"\\DosDevices\\CmpRetrain"
#define CMPRT_WIN32_PATH      L"\\\\.\\CmpRetrain"
#define CMPRT_SERVICE_NAME    L"cmpretrain"

#endif /* CMPRT_ABI_H */
