/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#define CMP_VERSION 7u
#define CMP_BUILD 0x000a0003u
#define CMP_MEMORY_HANDOVER CTL_CODE(FILE_DEVICE_UNKNOWN,0x803,METHOD_BUFFERED,FILE_READ_DATA|FILE_WRITE_DATA)
#define CMP_FINAL_RESET CTL_CODE(FILE_DEVICE_UNKNOWN,0x802,METHOD_BUFFERED,FILE_READ_DATA|FILE_WRITE_DATA)
#define CMP_DIAG CTL_CODE(FILE_DEVICE_UNKNOWN,0x800,METHOD_BUFFERED,FILE_READ_DATA)
#define CMP_RUN CTL_CODE(FILE_DEVICE_UNKNOWN,0x801,METHOD_BUFFERED,FILE_READ_DATA|FILE_WRITE_DATA)
#define CMP_COMPUTE 1u
#define CMP_MEMORY 2u
#define CMP_ACK 0x434d5031u
#define CMP_REG_COUNT 19u
#define CMP_FBPA_COUNT 24u
#define CMP_TARGET_MATCHED 1u
#define CMP_SEC2_STOPPED 2u
#define CMP_GSP_STOPPED 4u
#define CMP_CLEANUP_VERIFIED 8u
#define CMP_DMA_RELEASED 16u
#define CMP_DMA_RETAINED 32u
#define CMP_GEN2_REQUEST 1u
#define CMP_GEN2_RESUME 2u
typedef struct { unsigned long version,mode,ack,reserved; } CMP_INPUT;
typedef struct {
    unsigned long reg,wanted,before,after,cpu,mailbox0,mailbox1,wpr_lo,wpr_hi,outcome;
} CMP_PLM_OUTPUT;
typedef struct {
    unsigned long version,device,state,status,stage,mode;
    unsigned long regs[CMP_REG_COUNT];
    unsigned long before[CMP_REG_COUNT];
    unsigned long after[CMP_REG_COUNT];
    unsigned long build,checks,diagnostic_status;
    unsigned long cleanup_before[3],cleanup_after[3];
    unsigned long cleanup_attempted,cleanup_mismatch;
    unsigned long stop_final[2];
    unsigned long stop_cpu[2],stop_dma[2];
    unsigned long stop_gsp_riscv;
    unsigned long plm_count;
    CMP_PLM_OUTPUT plm[4]; /* outcome: 1=already matched, 2=written, 3=failed */
    unsigned long reset_query_status,reset_supported,reset_attempted,reset_status,reset_complete;
    unsigned long snapshot_cached; /* 1=pre-reset; 2=sealed no-FLR handover */
    unsigned long memory_handover;
    unsigned long pcie_requested,pcie_configured,pcie_link_status;
    unsigned long pcie_before[18],pcie_wanted[18],pcie_after[18];
    unsigned long fbpa_cfg1[CMP_FBPA_COUNT],fbpa_amount[CMP_FBPA_COUNT],fbpa_readable_mask;
} CMP_OUTPUT;
/* state: 0=no RUN completed, 1=targets AND cleanup verified, 2=failed/cold boot required.
 * checks/cleanup_* describe the RUN; after[] is the latest snapshot.
 * diagnostic_status describes the latest DIAG; it cannot erase a RUN failure.
 * No state means the NVIDIA driver has validated the expanded memory. */
#define CMP_GUID_VALUE {0xdf6f62f8,0x6382,0x4b1d,{0x93,0xc0,0x6a,0xc1,0xcc,0x17,0x08,0x01}}
