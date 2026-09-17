/* SPDX-License-Identifier: GPL-2.0-only
 *
 * retrain_core.h — CMP 170HX (GA100, 10DE:20C2/2082) PCIe Gen2 重训练状态机。
 *
 * 平台无关核心：所有硬件访问通过 cmprt_io 回调完成，
 * 内核驱动(retrain.c)提供 ECAM/BAR0 实现，离线测试(test_core.c)提供内存模拟。
 *
 * 时序依据(已验证路径)：
 *   cmpunlocker pcie-gen2-probe-retrain.patch —— NVIDIA 驱动 probe 阶段(GSP 已起)：
 *     BAR0: CYA_0 清 bit2 / LINK_CONFIG_0 MAX_RATE=2 / XVE_OVR=6, 50ms,
 *     两端 LNKCTL2.TLS=2, 上游 LNKCTL.RL 置位, 轮询 Gen2。
 *   cmpunlocker pcie-gen2.patch 晚写 —— GSP boot 后重写 PRIV_MISC_1/CYA/LINK_CONFIG/XVE。
 *   CMP40HX-Unlock-OnlyEFI Windows 实卡 —— 恢复 LINK_CONFIG_0+PRIV_MISC_1 后
 *     根端口 Retrain-Link SET-only(最多两次)即可物理 Gen2；不用 FLR/D3/SBR/PnP。
 *
 * 前提：显存/PCIe 受保护配置(XP3G 覆盖、VSEC_DEVICE、算力选择器等)必须已经由
 *   cmp170hx-windows-driver 0.11 的 MemoryGen2 链在 NVIDIA 接管前写入。
 *   本核心只补「GSP 初始化后会被清掉/未生效的策略寄存器 + 实际链路重训练」。
 *
 * 安全语义：
 *   - 只写目标 GPU 与其直接上游桥的指定寄存器，逐次回读；
 *   - 16 位配置写，避免触碰相邻 W1C 状态(LNKSTA 与 LNKCTL 同 dword)；
 *   - 失败恢复根端口原 TLS/ASPM 策略并报告，不执行 FLR、不改宽度、不碰其他设备。
 */
#ifndef CMPRT_CORE_H
#define CMPRT_CORE_H

typedef unsigned char  cmprt_u8;
typedef unsigned short cmprt_u16;
typedef unsigned int   cmprt_u32;

/* ---- BAR0 寄存器偏移(GA100) ---- */
#define CMPRT_R_BOOT0          0x00000u
#define CMPRT_R_XVE_OVR        0x8872cu
#define CMPRT_R_PRIV_MISC_1    0x8841cu
#define CMPRT_R_VSEC_DEVICE    0x8860cu
#define CMPRT_R_VSEC_HIER      0x88610u
#define CMPRT_R_LNKCAP_MIR     0x88084u
#define CMPRT_R_LC2_MIR        0x880a8u
#define CMPRT_R_CYA_0          0x8c2c0u
#define CMPRT_R_LINK_CONFIG_0  0x8c040u
#define CMPRT_R_XP3G_OVR0      0x8e110u
#define CMPRT_R_XP3G_VAL0      0x8e120u
#define CMPRT_R_XP3G_OVR3      0x8e11cu
#define CMPRT_R_XP3G_VAL3      0x8e12cu

#define CMPRT_BAR0_SNAP_COUNT  13u

/* ---- PCI 配置空间固定偏移 ---- */
#define CMPRT_CFG_ID           0x00u
#define CMPRT_CFG_CLASS        0x08u
#define CMPRT_CFG_HDRTYPE      0x0Eu
#define CMPRT_CFG_SECBUS       0x19u
#define CMPRT_CFG_BAR0_LO      0x10u
#define CMPRT_CFG_BAR0_HI      0x14u
#define CMPRT_CFG_CAPPTR       0x34u
#define CMPRT_PCIE_CAP_ID      0x10u
#define CMPRT_LNKCAP           0x0Cu  /* cap 相对偏移 */
#define CMPRT_LNKCTL           0x10u
#define CMPRT_LNKSTA           0x12u
#define CMPRT_LNKCTL2          0x30u

#define CMPRT_LNKCTL_RL        0x0020u
#define CMPRT_LNKCTL_ASPM_MASK 0x0003u
#define CMPRT_LNKSTA_TRAINING  0x0800u

/* ---- 结果码 ---- */
#define CMPRT_OK_ALREADY_GEN2   0u   /* 进入即物理 Gen2，未做任何写 */
#define CMPRT_OK_RETRAINED      1u   /* 重训练后两端稳定 Gen2 */
#define CMPRT_ERR_BAD_INPUT     2u
#define CMPRT_ERR_GPU_ID        3u
#define CMPRT_ERR_ROOT_ID       4u
#define CMPRT_ERR_TOPOLOGY      5u   /* 上游桥次级总线 != GPU 总线 */
#define CMPRT_ERR_BAR0          6u
#define CMPRT_ERR_BAR0_DEAD     7u
#define CMPRT_ERR_REG_STUCK     8u   /* 策略寄存器写后回读不符(stuck_reg 记录偏移) */
#define CMPRT_ERR_LINK_DOWN     9u
#define CMPRT_ERR_POLL_TIMEOUT  10u  /* 3 秒内未到稳定 Gen2，已恢复根端策略 */
#define CMPRT_ERR_WIDTH_CHANGED 11u
#define CMPRT_ERR_NO_PCIE_CAP   12u
#define CMPRT_ERR_MCFG          13u  /* 内核层：ECAM 基址获取失败 */
#define CMPRT_ERR_MAP           14u  /* 内核层：MMIO 映射失败 */
#define CMPRT_OK_QUERY          15u  /* 只读模式完成 */

#define CMPRT_MAX_POLL          30u  /* 30 x 100ms = 3s */

typedef struct cmprt_io_s {
    void *ctx;
    /* bdf = (bus<<8)|(dev<<3)|fn；实现方自行区分 GPU/上游桥映射页 */
    cmprt_u32 (*cfg_read)(void *ctx, cmprt_u32 bdf, cmprt_u32 off, cmprt_u8 size);
    void      (*cfg_write)(void *ctx, cmprt_u32 bdf, cmprt_u32 off, cmprt_u32 val, cmprt_u8 size);
    cmprt_u32 (*mmio_read)(void *ctx, cmprt_u32 off);
    void      (*mmio_write)(void *ctx, cmprt_u32 off, cmprt_u32 val);
    void      (*sleep_ms)(void *ctx, cmprt_u32 ms);
} cmprt_io;

typedef struct cmprt_result_s {
    cmprt_u32 code;
    cmprt_u32 stage;
    cmprt_u32 gpu_id;          /* cfg[0]：期望 0x20C210DE 或 0x208210DE */
    cmprt_u32 root_id;
    cmprt_u16 gpu_pcie_cap;    /* PCIe capability 起始偏移(0=未找到) */
    cmprt_u16 root_pcie_cap;
    cmprt_u32 gpu_bar0_lo;
    cmprt_u32 gpu_bar0_hi;
    cmprt_u32 bar0_before[CMPRT_BAR0_SNAP_COUNT];
    cmprt_u32 bar0_after[CMPRT_BAR0_SNAP_COUNT];
    cmprt_u16 gpu_lnkctl_before,  root_lnkctl_before;
    cmprt_u16 gpu_lnkctl2_before, root_lnkctl2_before;
    cmprt_u16 gpu_lnksta_before,  root_lnksta_before;
    cmprt_u16 gpu_lnkctl_after,   root_lnkctl_after;
    cmprt_u16 gpu_lnkctl2_after,  root_lnkctl2_after;
    cmprt_u16 gpu_lnksta_after,   root_lnksta_after;
    cmprt_u32 poll_count;
    cmprt_u32 poll_trace[CMPRT_MAX_POLL]; /* 低16=根端 LNKSTA，高16=GPU LNKSTA */
    cmprt_u32 stuck_reg;
    char      message[96];
} cmprt_result;

/* ---- 内部小工具 ---- */
static void cmprt_strcpy(char *dst, const char *src, cmprt_u32 cap) {
    cmprt_u32 i = 0;
    if (cap == 0) return;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static cmprt_u16 cmprt_find_pcie_cap(const cmprt_io *io, cmprt_u32 bdf) {
    cmprt_u32 ptr, guard;
    ptr = io->cfg_read(io->ctx, bdf, CMPRT_CFG_CAPPTR, 1) & 0xFCu;
    for (guard = 0; guard < 48 && ptr >= 0x40 && ptr <= 0xFC; guard++) {
        cmprt_u8 id = (cmprt_u8)io->cfg_read(io->ctx, bdf, ptr, 1);
        if (id == CMPRT_PCIE_CAP_ID) return (cmprt_u16)ptr;
        ptr = io->cfg_read(io->ctx, bdf, ptr + 1, 1) & 0xFCu;
    }
    return 0;
}

static cmprt_u16 cmprt_lnksta(const cmprt_io *io, cmprt_u32 bdf, cmprt_u16 cap) {
    return (cmprt_u16)io->cfg_read(io->ctx, bdf, (cmprt_u32)cap + CMPRT_LNKSTA, 2);
}
static cmprt_u16 cmprt_lnkctl(const cmprt_io *io, cmprt_u32 bdf, cmprt_u16 cap) {
    return (cmprt_u16)io->cfg_read(io->ctx, bdf, (cmprt_u32)cap + CMPRT_LNKCTL, 2);
}
static cmprt_u16 cmprt_lnkctl2(const cmprt_io *io, cmprt_u32 bdf, cmprt_u16 cap) {
    return (cmprt_u16)io->cfg_read(io->ctx, bdf, (cmprt_u32)cap + CMPRT_LNKCTL2, 2);
}

static cmprt_u32 cmprt_bdf(cmprt_u32 bus, cmprt_u32 dev, cmprt_u32 fn) {
    return (bus << 8) | (dev << 3) | fn;
}

/* 快照 13 个 BAR0 寄存器：与 bar0_before/after 索引一一对应 */
static const cmprt_u32 cmprt_bar0_snap[CMPRT_BAR0_SNAP_COUNT] = {
    CMPRT_R_BOOT0, CMPRT_R_XVE_OVR, CMPRT_R_PRIV_MISC_1,
    CMPRT_R_VSEC_DEVICE, CMPRT_R_VSEC_HIER, CMPRT_R_LNKCAP_MIR,
    CMPRT_R_LC2_MIR, CMPRT_R_CYA_0, CMPRT_R_LINK_CONFIG_0,
    CMPRT_R_XP3G_OVR0, CMPRT_R_XP3G_VAL0, CMPRT_R_XP3G_OVR3, CMPRT_R_XP3G_VAL3
};

static void cmprt_snapshot_bar0(const cmprt_io *io, cmprt_u32 *out) {
    cmprt_u32 i;
    for (i = 0; i < CMPRT_BAR0_SNAP_COUNT; i++)
        out[i] = io->mmio_read(io->ctx, cmprt_bar0_snap[i]);
}

/* 单步 BAR0 写 + 掩码回读校验；ok=0 表示失败 */
static void cmprt_bar0_step(const cmprt_io *io, cmprt_result *r,
                            cmprt_u32 reg, cmprt_u32 value,
                            cmprt_u32 mask, cmprt_u32 expect) {
    io->mmio_write(io->ctx, reg, value);
    io->mmio_read(io->ctx, reg); /* 写后冲刷一次读 */
    r->stage = reg;
    if ((io->mmio_read(io->ctx, reg) & mask) != expect) {
        r->code = CMPRT_ERR_REG_STUCK;
        r->stuck_reg = reg;
    }
}

/*
 * 主流程。
 *   gpu_bdf/root_bdf：调用方已按总线号验证过(用户态 PnP 父子关系)，
 *                     本函数再做硬件级复核(ID/桥类型/次级总线)。
 *   apply：0=只读查询(任何时刻安全)，1=执行策略补写 + 重训练。
 */
static cmprt_u32 cmprt_run(const cmprt_io *io,
                           cmprt_u32 gpu_bdf, cmprt_u32 root_bdf,
                           int apply, cmprt_result *r) {
    cmprt_u16 gcap, rcap, gs, rs, gwidth, rwidth;
    cmprt_u32 i, stable, down, poll;
    cmprt_u16 v16;
    cmprt_u32 v;

    for (i = 0; i < sizeof(*r) / sizeof(cmprt_u32); i++) ((cmprt_u32*)r)[i] = 0;
    r->message[0] = 0;
    r->code = CMPRT_ERR_BAD_INPUT;
    r->stage = 0;
    r->stuck_reg = 0;

    if (!io || !io->cfg_read || !io->cfg_write || !io->mmio_read || !io->mmio_write || !io->sleep_ms) {
        cmprt_strcpy(r->message, "io callbacks missing", sizeof(r->message));
        return r->code;
    }

    /* ---- 1. 身份与拓扑硬件复核 ---- */
    r->stage = 1;
    r->gpu_id = io->cfg_read(io->ctx, gpu_bdf, CMPRT_CFG_ID, 4);
    if (r->gpu_id != 0x20C210DEu && r->gpu_id != 0x208210DEu) {
        r->code = CMPRT_ERR_GPU_ID;
        cmprt_strcpy(r->message, "GPU id mismatch (want 10DE:20C2/2082)", sizeof(r->message));
        return r->code;
    }
    r->root_id = io->cfg_read(io->ctx, root_bdf, CMPRT_CFG_ID, 4);
    if ((r->root_id & 0xFFFFu) == 0xFFFFu || r->root_id == 0 || r->root_id == 0xFFFFFFFFu) {
        r->code = CMPRT_ERR_ROOT_ID;
        cmprt_strcpy(r->message, "upstream bridge not present", sizeof(r->message));
        return r->code;
    }
    v = io->cfg_read(io->ctx, root_bdf, CMPRT_CFG_CLASS, 4);
    if (((v >> 16) & 0xFFFFu) != 0x0604u) {
        r->code = CMPRT_ERR_TOPOLOGY;
        cmprt_strcpy(r->message, "upstream device is not a PCI-PCI bridge", sizeof(r->message));
        return r->code;
    }
    v = io->cfg_read(io->ctx, root_bdf, CMPRT_CFG_HDRTYPE, 1);
    if ((v & 0x7Fu) != 1u) {
        r->code = CMPRT_ERR_TOPOLOGY;
        cmprt_strcpy(r->message, "upstream device header type is not bridge", sizeof(r->message));
        return r->code;
    }
    v = io->cfg_read(io->ctx, root_bdf, CMPRT_CFG_SECBUS, 1);
    if (v != ((gpu_bdf >> 8) & 0xFFu)) {
        r->code = CMPRT_ERR_TOPOLOGY;
        cmprt_strcpy(r->message, "bridge secondary bus != GPU bus", sizeof(r->message));
        return r->code;
    }

    /* ---- 2. PCIe capability 定位 ---- */
    r->stage = 2;
    gcap = cmprt_find_pcie_cap(io, gpu_bdf);
    rcap = cmprt_find_pcie_cap(io, root_bdf);
    r->gpu_pcie_cap = gcap;
    r->root_pcie_cap = rcap;
    if (!gcap || !rcap) {
        r->code = CMPRT_ERR_NO_PCIE_CAP;
        cmprt_strcpy(r->message, "PCIe capability not found", sizeof(r->message));
        return r->code;
    }

    /* ---- 3. 进入状态快照 ---- */
    r->stage = 3;
    r->gpu_lnkctl_before  = cmprt_lnkctl(io, gpu_bdf, gcap);
    r->root_lnkctl_before = cmprt_lnkctl(io, root_bdf, rcap);
    r->gpu_lnkctl2_before  = cmprt_lnkctl2(io, gpu_bdf, gcap);
    r->root_lnkctl2_before = cmprt_lnkctl2(io, root_bdf, rcap);
    r->gpu_lnksta_before  = cmprt_lnksta(io, gpu_bdf, gcap);
    r->root_lnksta_before = cmprt_lnksta(io, root_bdf, rcap);

    r->gpu_bar0_lo = io->cfg_read(io->ctx, gpu_bdf, CMPRT_CFG_BAR0_LO, 4);
    r->gpu_bar0_hi = io->cfg_read(io->ctx, gpu_bdf, CMPRT_CFG_BAR0_HI, 4);
    if ((r->gpu_bar0_lo & 1u) || (r->gpu_bar0_lo & 0xFFFFFFF0u) == 0) {
        r->code = CMPRT_ERR_BAR0;
        cmprt_strcpy(r->message, "GPU BAR0 not a valid memory BAR", sizeof(r->message));
        return r->code;
    }
    if (io->mmio_read(io->ctx, CMPRT_R_BOOT0) == 0xFFFFFFFFu) {
        r->code = CMPRT_ERR_BAR0_DEAD;
        cmprt_strcpy(r->message, "BAR0 reads all-ones (GPU link/config dead?)", sizeof(r->message));
        return r->code;
    }
    cmprt_snapshot_bar0(io, r->bar0_before);
    for (i = 0; i < CMPRT_BAR0_SNAP_COUNT; i++) r->bar0_after[i] = r->bar0_before[i];

    gs = r->gpu_lnksta_before;
    rs = r->root_lnksta_before;
    gwidth = (gs >> 4) & 0x3Fu;
    rwidth = (rs >> 4) & 0x3Fu;

    /* 全一/零速率 = 配置空间或链路不可读，任何后续写都没有意义 */
    if (gs == 0xFFFFu || rs == 0xFFFFu || (gs & 0xFu) == 0 || (rs & 0xFu) == 0) {
        r->code = CMPRT_ERR_LINK_DOWN;
        cmprt_strcpy(r->message, "link status unreadable at entry (all-ones/zero)",
                     sizeof(r->message));
        r->gpu_lnksta_after = gs; r->root_lnksta_after = rs;
        r->gpu_lnkctl_after = r->gpu_lnkctl_before; r->root_lnkctl_after = r->root_lnkctl_before;
        r->gpu_lnkctl2_after = r->gpu_lnkctl2_before; r->root_lnkctl2_after = r->root_lnkctl2_before;
        return r->code;
    }

    if ((gs & 0xFu) >= 2u && (rs & 0xFu) >= 2u && gwidth != 0 && rwidth == gwidth) {
        r->code = CMPRT_OK_ALREADY_GEN2;
        cmprt_strcpy(r->message, "already physical Gen2 on both ends; no writes", sizeof(r->message));
        r->gpu_lnksta_after = gs; r->root_lnksta_after = rs;
        r->gpu_lnkctl_after = r->gpu_lnkctl_before; r->root_lnkctl_after = r->root_lnkctl_before;
        r->gpu_lnkctl2_after = r->gpu_lnkctl2_before; r->root_lnkctl2_after = r->root_lnkctl2_before;
        return r->code;
    }

    if (!apply) {
        r->code = CMPRT_OK_QUERY;
        cmprt_strcpy(r->message, "query only; no writes performed", sizeof(r->message));
        return r->code;
    }

    /* ---- 4. GPU 策略寄存器补写(Linux late-write + probe-retrain 的并集) ---- */
    r->stage = 4;
    /* CYA_0：清 DIS_G2(bit2)。依据 pcie-gen2-probe-retrain.patch。 */
    v = io->mmio_read(io->ctx, CMPRT_R_CYA_0);
    cmprt_bar0_step(io, r, CMPRT_R_CYA_0, v & ~4u, 4u, 0u);
    if (r->code == CMPRT_ERR_REG_STUCK) { cmprt_strcpy(r->message, "CYA_0 bit2 clear failed", sizeof(r->message)); return r->code; }

    /* LINK_CONFIG_0：MAX_RATE(bits19:18)=2 */
    v = io->mmio_read(io->ctx, CMPRT_R_LINK_CONFIG_0);
    cmprt_bar0_step(io, r, CMPRT_R_LINK_CONFIG_0, (v & ~0x000C0000u) | (2u << 18), 0x000C0000u, 0x00080000u);
    if (r->code == CMPRT_ERR_REG_STUCK) { cmprt_strcpy(r->message, "LINK_CONFIG_0 MAX_RATE=2 failed", sizeof(r->message)); return r->code; }

    /* XVE override：0x8872C = 6(0.10.2 实卡验证写后 TLS 才被接受) */
    cmprt_bar0_step(io, r, CMPRT_R_XVE_OVR, 6u, 0xFFFFFFFFu, 6u);
    if (r->code == CMPRT_ERR_REG_STUCK) { cmprt_strcpy(r->message, "XVE override write failed", sizeof(r->message)); return r->code; }

    /* PRIV_MISC_1：置 bits 11/13、清 bits 12/14(late-write 关键项) */
    v = io->mmio_read(io->ctx, CMPRT_R_PRIV_MISC_1);
    cmprt_bar0_step(io, r, CMPRT_R_PRIV_MISC_1, (v | 0x2800u) & ~0x5000u, 0x7800u, 0x2800u);
    if (r->code == CMPRT_ERR_REG_STUCK) { cmprt_strcpy(r->message, "PRIV_MISC_1 update failed", sizeof(r->message)); return r->code; }

    /* VSEC_HIERARCHY：清 bit12、置 bit0(pcie-gen2.patch 中的普通写) */
    v = io->mmio_read(io->ctx, CMPRT_R_VSEC_HIER);
    cmprt_bar0_step(io, r, CMPRT_R_VSEC_HIER, (v & ~0x1000u) | 1u, 0x1001u, 0x0001u);
    if (r->code == CMPRT_ERR_REG_STUCK) { cmprt_strcpy(r->message, "VSEC_HIERARCHY update failed", sizeof(r->message)); return r->code; }

    /* LC2 镜像(BAR0 0x880A8)：功能要求只有 TLS=2(cmpunlocker probe-retrain)。
     * 0.4 实卡发现 bits19:16 写不回(读回 0x00000002)——该镜像高半字疑为
     * LNKSTA2 状态(RO)，0.10 链的 "+0xF0000" 多半误把状态位当可写位。
     * 写入保留 0xF0000(若 RO 则无害)，校验只查低 4 位 TLS。 */
    v = io->mmio_read(io->ctx, CMPRT_R_LC2_MIR);
    cmprt_bar0_step(io, r, CMPRT_R_LC2_MIR, (v & ~15u) | 2u | 0x000F0000u, 0x0000000Fu, 0x00000002u);
    if (r->code == CMPRT_ERR_REG_STUCK) { cmprt_strcpy(r->message, "LC2 mirror TLS write failed", sizeof(r->message)); return r->code; }

    cmprt_snapshot_bar0(io, r->bar0_after);

    /* 等待策略生效并确认 BAR 仍存活(probe-retrain 的 msleep(50)) */
    io->sleep_ms(io->ctx, 50);
    if (io->mmio_read(io->ctx, CMPRT_R_BOOT0) == 0xFFFFFFFFu) {
        r->code = CMPRT_ERR_BAR0_DEAD;
        cmprt_strcpy(r->message, "BAR0 died after policy writes", sizeof(r->message));
        return r->code;
    }

    /* ---- 5. 配置空间：两端 TLS=2、清 ASPM；16 位写保护相邻状态 ---- */
    r->stage = 5;
    v16 = cmprt_lnkctl2(io, gpu_bdf, gcap);
    io->cfg_write(io->ctx, gpu_bdf, (cmprt_u32)gcap + CMPRT_LNKCTL2, (cmprt_u32)((v16 & ~0xFu) | 2u), 2);
    v16 = cmprt_lnkctl2(io, root_bdf, rcap);
    io->cfg_write(io->ctx, root_bdf, (cmprt_u32)rcap + CMPRT_LNKCTL2, (cmprt_u32)((v16 & ~0xFu) | 2u), 2);
    /* 清 ASPM(B站方案与 40HX 均记录空载掉卡与 ASPM 相关) */
    v16 = cmprt_lnkctl(io, gpu_bdf, gcap);
    io->cfg_write(io->ctx, gpu_bdf, (cmprt_u32)gcap + CMPRT_LNKCTL, (cmprt_u32)(v16 & ~CMPRT_LNKCTL_ASPM_MASK), 2);
    v16 = cmprt_lnkctl(io, root_bdf, rcap);
    io->cfg_write(io->ctx, root_bdf, (cmprt_u32)rcap + CMPRT_LNKCTL, (cmprt_u32)(v16 & ~CMPRT_LNKCTL_ASPM_MASK), 2);

    /* ---- 6. 根端口 Retrain Link(SET-only；40HX 实卡语义) ---- */
    r->stage = 6;
    v16 = cmprt_lnkctl(io, root_bdf, rcap);
    io->cfg_write(io->ctx, root_bdf, (cmprt_u32)rcap + CMPRT_LNKCTL, (cmprt_u32)(v16 | CMPRT_LNKCTL_RL), 2);

    /* ---- 7. 轮询：两端 Gen2、宽度不变、Training 消失，连续 3 次 ---- */
    r->stage = 7;
    stable = 0; down = 0;
    r->poll_count = 0;
    for (poll = 0; poll < CMPRT_MAX_POLL; poll++) {
        io->sleep_ms(io->ctx, 100);
        /* 40HX 实卡：第一次 RL 可能不进入训练，1.5s 仍未 Gen2 时再 SET 一次 */
        if (poll == 15 && stable == 0) {
            v16 = cmprt_lnkctl(io, root_bdf, rcap);
            io->cfg_write(io->ctx, root_bdf, (cmprt_u32)rcap + CMPRT_LNKCTL,
                          (cmprt_u32)(v16 | CMPRT_LNKCTL_RL), 2);
        }
        gs = cmprt_lnksta(io, gpu_bdf, gcap);
        rs = cmprt_lnksta(io, root_bdf, rcap);
        r->poll_trace[poll] = ((cmprt_u32)gs << 16) | rs;
        r->poll_count = poll + 1;

        if (gs == 0xFFFFu || rs == 0xFFFFu || (gs & 0xFu) == 0 || (rs & 0xFu) == 0) {
            if (++down >= 10) {
                r->code = CMPRT_ERR_LINK_DOWN;
                cmprt_strcpy(r->message, "link read all-ones/zero for >1s during retrain", sizeof(r->message));
                goto restore;
            }
            stable = 0;
            continue;
        }
        down = 0;

        if ((gs & CMPRT_LNKSTA_TRAINING) == 0 && (rs & CMPRT_LNKSTA_TRAINING) == 0 &&
            (gs & 0xFu) >= 2u && (rs & 0xFu) >= 2u) {
            if ((((gs >> 4) & 0x3Fu) != gwidth) || (((rs >> 4) & 0x3Fu) != rwidth)) {
                r->code = CMPRT_ERR_WIDTH_CHANGED;
                r->gpu_lnksta_after = gs; r->root_lnksta_after = rs;
                cmprt_strcpy(r->message, "link width changed during retrain", sizeof(r->message));
                goto restore;
            }
            if (++stable >= 3) {
                r->code = CMPRT_OK_RETRAINED;
                cmprt_strcpy(r->message, "Gen2 stable on both ends", sizeof(r->message));
                goto done;
            }
        } else {
            stable = 0;
        }
    }
    r->code = CMPRT_ERR_POLL_TIMEOUT;
    cmprt_strcpy(r->message, "no stable Gen2 within 3s; root policy restored", sizeof(r->message));

restore:
    /* 失败恢复根端口原 TLS/ASPM；GPU 侧策略保留(与 stage-1 一致，本身无害) */
    v16 = (cmprt_u16)((r->root_lnkctl2_before & 0xFu) |
                      (cmprt_lnkctl2(io, root_bdf, rcap) & ~0xFu));
    io->cfg_write(io->ctx, root_bdf, (cmprt_u32)rcap + CMPRT_LNKCTL2, (cmprt_u32)v16, 2);
    v16 = (cmprt_u16)((r->root_lnkctl_before & CMPRT_LNKCTL_ASPM_MASK) |
                      (cmprt_lnkctl(io, root_bdf, rcap) & ~CMPRT_LNKCTL_ASPM_MASK));
    io->cfg_write(io->ctx, root_bdf, (cmprt_u32)rcap + CMPRT_LNKCTL, (cmprt_u32)v16, 2);

done:
    r->gpu_lnksta_after  = cmprt_lnksta(io, gpu_bdf, gcap);
    r->root_lnksta_after = cmprt_lnksta(io, root_bdf, rcap);
    r->gpu_lnkctl_after  = cmprt_lnkctl(io, gpu_bdf, gcap);
    r->root_lnkctl_after = cmprt_lnkctl(io, root_bdf, rcap);
    r->gpu_lnkctl2_after  = cmprt_lnkctl2(io, gpu_bdf, gcap);
    r->root_lnkctl2_after = cmprt_lnkctl2(io, root_bdf, rcap);
    cmprt_snapshot_bar0(io, r->bar0_after);
    return r->code;
}

#endif /* CMPRT_CORE_H */
