/* SPDX-License-Identifier: GPL-2.0-only
 *
 * test_core.c — retrain_core.h 离线模拟测试。
 * 用平面内存模拟两端 ECAM 配置空间与 GPU BAR0；LNKSTA 由脚本驱动。
 * 编译：cl /W4 /WX /O2 tests\test_core.c（直接包含生产头文件）。
 */
#include <stdio.h>
#include <string.h>
#include "../src/retrain_core.h"

#define CFG_SIZE 4096
#define BAR_SIZE 0x100000
#define MAX_SCRIPT 64

typedef struct {
    unsigned char gpu_cfg[CFG_SIZE];
    unsigned char root_cfg[CFG_SIZE];
    unsigned char bar0[BAR_SIZE];
    cmprt_u32 gpu_bdf, root_bdf;
    cmprt_u16 gpu_cap, root_cap;      /* 与 cfg 内容一致，用于脚本拦截 */
    unsigned short gpu_script[MAX_SCRIPT]; int gpu_len, gpu_idx;
    unsigned short root_script[MAX_SCRIPT]; int root_len, root_idx;
    struct { cmprt_u32 bdf, off, val; cmprt_u8 size; } wlog[256]; int wn;
    struct { cmprt_u32 off, val; } mlog[256]; int mn;
    unsigned sleeps;
    cmprt_u32 stuck_off;               /* 该 BAR0 偏移写入不生效 */
} MOCK;

static unsigned char *cfg_ptr(MOCK *m, cmprt_u32 bdf, cmprt_u32 off) {
    unsigned char *base = (bdf == m->gpu_bdf) ? m->gpu_cfg : m->root_cfg;
    return base + off;
}

static cmprt_u32 mock_cfg_read(void *v, cmprt_u32 bdf, cmprt_u32 off, cmprt_u8 size) {
    MOCK *m = (MOCK*)v;
    unsigned char *p;
    cmprt_u32 val;
    cmprt_u16 cap = (bdf == m->gpu_bdf) ? m->gpu_cap : m->root_cap;
    if (cap && off == (cmprt_u32)cap + CMPRT_LNKSTA && size == 2) {
        if (bdf == m->gpu_bdf) {
            int i = m->gpu_idx < m->gpu_len ? m->gpu_idx : m->gpu_len - 1;
            if (m->gpu_idx < m->gpu_len) m->gpu_idx++;
            return m->gpu_script[i];
        } else {
            int i = m->root_idx < m->root_len ? m->root_idx : m->root_len - 1;
            if (m->root_idx < m->root_len) m->root_idx++;
            return m->root_script[i];
        }
    }
    p = cfg_ptr(m, bdf, off);
    if (size == 1) val = p[0];
    else if (size == 2) val = (cmprt_u32)(p[0] | (p[1] << 8));
    else val = (cmprt_u32)(p[0] | (p[1] << 8) | ((cmprt_u32)p[2] << 16) | ((cmprt_u32)p[3] << 24));
    return val;
}

static void mock_cfg_write(void *v, cmprt_u32 bdf, cmprt_u32 off, cmprt_u32 val, cmprt_u8 size) {
    MOCK *m = (MOCK*)v;
    unsigned char *p = cfg_ptr(m, bdf, off);
    if (m->wn < 256) {
        m->wlog[m->wn].bdf = bdf; m->wlog[m->wn].off = off;
        m->wlog[m->wn].val = val; m->wlog[m->wn].size = size; m->wn++;
    }
    p[0] = (unsigned char)(val & 0xFF);
    if (size > 1) p[1] = (unsigned char)((val >> 8) & 0xFF);
    if (size > 2) { p[2] = (unsigned char)((val >> 16) & 0xFF); p[3] = (unsigned char)((val >> 24) & 0xFF); }
}

static cmprt_u32 mock_mmio_read(void *v, cmprt_u32 off) {
    MOCK *m = (MOCK*)v;
    unsigned char *p = m->bar0 + off;
    return (cmprt_u32)(p[0] | (p[1] << 8) | ((cmprt_u32)p[2] << 16) | ((cmprt_u32)p[3] << 24));
}

static void mock_mmio_write(void *v, cmprt_u32 off, cmprt_u32 val) {
    MOCK *m = (MOCK*)v;
    unsigned char *p = m->bar0 + off;
    if (m->mn < 256) { m->mlog[m->mn].off = off; m->mlog[m->mn].val = val; m->mn++; }
    if (off == m->stuck_off) return;   /* 模拟写入无效 */
    p[0] = (unsigned char)(val & 0xFF);
    p[1] = (unsigned char)((val >> 8) & 0xFF);
    p[2] = (unsigned char)((val >> 16) & 0xFF);
    p[3] = (unsigned char)((val >> 24) & 0xFF);
}

static void mock_sleep(void *v, cmprt_u32 ms) {
    MOCK *m = (MOCK*)v;
    m->sleeps += ms;
}

static void put32(unsigned char *buf, cmprt_u32 off, cmprt_u32 v) {
    buf[off] = (unsigned char)(v & 0xFF);
    buf[off+1] = (unsigned char)((v >> 8) & 0xFF);
    buf[off+2] = (unsigned char)((v >> 16) & 0xFF);
    buf[off+3] = (unsigned char)((v >> 24) & 0xFF);
}
static void put16(unsigned char *buf, cmprt_u32 off, cmprt_u16 v) {
    buf[off] = (unsigned char)(v & 0xFF);
    buf[off+1] = (unsigned char)((v >> 8) & 0xFF);
}

#define GPU_BUS  0xB3
#define ROOT_BUS 0xB2
#define GPU_CAP  0x78
#define ROOT_CAP 0x40
#define GPU_LNKSTA_OFF  (GPU_CAP + CMPRT_LNKSTA)   /* 0x8A */
#define ROOT_LNKSTA_OFF (ROOT_CAP + CMPRT_LNKSTA)  /* 0x52 */
#define GPU_LNKCTL_OFF  (GPU_CAP + CMPRT_LNKCTL)   /* 0x88 */
#define ROOT_LNKCTL_OFF (ROOT_CAP + CMPRT_LNKCTL)  /* 0x50 */
#define GPU_LNKCTL2_OFF (GPU_CAP + CMPRT_LNKCTL2)  /* 0xA8 */
#define ROOT_LNKCTL2_OFF (ROOT_CAP + CMPRT_LNKCTL2)/* 0x70 */

#define LNK_G1_X4 0x0041
#define LNK_G2_X4 0x0042
#define LNK_TRAIN 0x0800

/* 构造合法拓扑：GPU 10DE:20C2 @ B3:00.0，Intel 根端口 @ B2:00.0(sec=0xB3) */
static void mock_init(MOCK *m) {
    memset(m, 0, sizeof(*m));
    m->gpu_bdf = cmprt_bdf(GPU_BUS, 0, 0);
    m->root_bdf = cmprt_bdf(ROOT_BUS, 0, 0);
    m->gpu_cap = GPU_CAP;
    m->root_cap = ROOT_CAP;

    put32(m->gpu_cfg, 0x00, 0x20C210DEu);
    put32(m->gpu_cfg, 0x08, 0x03000000u);          /* class: display */
    m->gpu_cfg[0x0E] = 0x00;
    m->gpu_cfg[0x34] = 0x60;                        /* cap ptr */
    m->gpu_cfg[0x60] = 0x01; m->gpu_cfg[0x61] = 0x68;   /* PM -> 0x68 */
    m->gpu_cfg[0x68] = 0x05; m->gpu_cfg[0x69] = 0x78;   /* MSI -> 0x78 */
    m->gpu_cfg[0x78] = 0x10; m->gpu_cfg[0x79] = 0x00;   /* PCIe, end */
    put32(m->gpu_cfg, 0x10, 0xF2000004u);           /* BAR0 64-bit mem */
    put32(m->gpu_cfg, 0x14, 0x00000000u);
    put16(m->gpu_cfg, GPU_LNKCTL_OFF, 0x0000);
    put16(m->gpu_cfg, GPU_LNKCTL2_OFF, 0x0001);     /* TLS=1 */

    put32(m->root_cfg, 0x00, 0x20308086u);          /* Intel 2030 */
    put32(m->root_cfg, 0x08, 0x06040000u);          /* class: PCI-PCI bridge */
    m->root_cfg[0x0E] = 0x01;                        /* header type 1 */
    m->root_cfg[0x18] = 0x00;                        /* primary */
    m->root_cfg[0x19] = GPU_BUS;                     /* secondary = GPU bus */
    m->root_cfg[0x1A] = GPU_BUS;                     /* subordinate */
    m->root_cfg[0x34] = ROOT_CAP;
    m->root_cfg[ROOT_CAP] = 0x10; m->root_cfg[ROOT_CAP + 1] = 0x00;
    put16(m->root_cfg, ROOT_LNKCTL_OFF, 0x0000);
    put16(m->root_cfg, ROOT_LNKCTL2_OFF, 0x0001);

    put32(m->bar0, CMPRT_R_BOOT0, 0x0EA000A1u);
    put32(m->bar0, CMPRT_R_CYA_0, 0x068731B7u);      /* bit2 置位 */
    put32(m->bar0, CMPRT_R_LINK_CONFIG_0, 0x80044C00u);/* MAX_RATE=1 */
    put32(m->bar0, CMPRT_R_XVE_OVR, 0u);
    put32(m->bar0, CMPRT_R_PRIV_MISC_1, 0xE0B40D00u);
    put32(m->bar0, CMPRT_R_VSEC_HIER, 0x00001000u);
    put32(m->bar0, CMPRT_R_LNKCAP_MIR, 0x00456101u);
    put32(m->bar0, CMPRT_R_LC2_MIR, 0x00000001u);
    put32(m->bar0, CMPRT_R_VSEC_DEVICE, 0x00000800u);
    put32(m->bar0, CMPRT_R_XP3G_OVR0, 1u);
    put32(m->bar0, CMPRT_R_XP3G_VAL0, 0u);
    put32(m->bar0, CMPRT_R_XP3G_OVR3, 4u);
    put32(m->bar0, CMPRT_R_XP3G_VAL3, 0x00200000u);
}

static void set_script(MOCK *m, const unsigned short *g, int gl,
                       const unsigned short *r, int rl) {
    memcpy(m->gpu_script, g, (size_t)gl * 2); m->gpu_len = gl;
    memcpy(m->root_script, r, (size_t)rl * 2); m->root_len = rl;
}

static cmprt_io make_io(MOCK *m) {
    cmprt_io io;
    io.ctx = m;
    io.cfg_read = mock_cfg_read;
    io.cfg_write = mock_cfg_write;
    io.mmio_read = mock_mmio_read;
    io.mmio_write = mock_mmio_write;
    io.sleep_ms = mock_sleep;
    return io;
}

static int g_pass, g_fail;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; printf("  PASS %s\n", msg); } \
    else { g_fail++; printf("  FAIL %s\n", msg); } \
} while (0)

static int last_root_write(MOCK *m, cmprt_u32 off, cmprt_u32 *val) {
    int i;
    for (i = m->wn - 1; i >= 0; i--)
        if (m->wlog[i].bdf == m->root_bdf && m->wlog[i].off == off) {
            *val = m->wlog[i].val; return 1;
        }
    return 0;
}

int main(void) {
    static MOCK m;   /* 含 1 MiB BAR0 模拟区，不能放栈上 */
    cmprt_io io; cmprt_result r; cmprt_u32 code; cmprt_u32 v;

    /* 1. 已进入 Gen2：零写入直接通过 */
    {
        const unsigned short g1[] = { LNK_G2_X4, LNK_G2_X4, LNK_G2_X4 };
        mock_init(&m); io = make_io(&m);
        set_script(&m, g1, 3, g1, 3);
        code = cmprt_run(&io, m.gpu_bdf, m.root_bdf, 1, &r);
        printf("[already-gen2] code=%u\n", code);
        CHECK(code == CMPRT_OK_ALREADY_GEN2, "code OK_ALREADY_GEN2");
        CHECK(m.wn == 0 && m.mn == 0 && m.sleeps == 0, "no writes at all");
    }

    /* 2. 正常路径：Gen1 -> 训练 -> 稳定 Gen2 */
    {
        const unsigned short gs[] = { LNK_G1_X4, LNK_G1_X4|LNK_TRAIN, LNK_G2_X4|LNK_TRAIN,
                                      LNK_G2_X4, LNK_G2_X4, LNK_G2_X4, LNK_G2_X4 };
        const unsigned short rs[] = { LNK_G1_X4, LNK_G1_X4|LNK_TRAIN, LNK_G2_X4|LNK_TRAIN,
                                      LNK_G2_X4, LNK_G2_X4, LNK_G2_X4, LNK_G2_X4 };
        mock_init(&m); io = make_io(&m);
        set_script(&m, gs, 7, rs, 7);
        code = cmprt_run(&io, m.gpu_bdf, m.root_bdf, 1, &r);
        printf("[happy] code=%u polls=%u\n", code, r.poll_count);
        CHECK(code == CMPRT_OK_RETRAINED, "code OK_RETRAINED");
        CHECK(m.mn == 6, "six BAR0 policy writes");
        CHECK((mock_mmio_read(&m, CMPRT_R_CYA_0) & 4u) == 0, "CYA_0 DIS_G2 cleared");
        CHECK((mock_mmio_read(&m, CMPRT_R_LINK_CONFIG_0) & 0xC0000u) == 0x80000u,
              "LINK_CONFIG_0 MAX_RATE=2");
        CHECK(mock_mmio_read(&m, CMPRT_R_XVE_OVR) == 6u, "XVE_OVR=6");
        CHECK((mock_mmio_read(&m, CMPRT_R_PRIV_MISC_1) & 0x7800u) == 0x2800u,
              "PRIV_MISC_1 bits 11/13 set, 12/14 clear");
        CHECK((mock_mmio_read(&m, CMPRT_R_VSEC_HIER) & 0x1001u) == 1u, "VSEC_HIER fixed");
        CHECK((mock_mmio_read(&m, CMPRT_R_LC2_MIR) & 0xFu) == 2u, "LC2 mirror TLS=2");
        CHECK(last_root_write(&m, ROOT_LNKCTL2_OFF, &v) && (v & 0xFu) == 2u,
              "root TLS=2 written");
        {
            int rl = 0, i;
            for (i = 0; i < m.wn; i++)
                if (m.wlog[i].bdf == m.root_bdf && m.wlog[i].off == ROOT_LNKCTL_OFF &&
                    (m.wlog[i].val & CMPRT_LNKCTL_RL)) rl = 1;
            CHECK(rl, "root Retrain-Link SET observed");
        }
        {
            int aspm_cleared = 0, i;
            for (i = 0; i < m.wn; i++)
                if (m.wlog[i].off == GPU_LNKCTL_OFF && !(m.wlog[i].val & 3u)) aspm_cleared = 1;
            CHECK(aspm_cleared, "GPU ASPM cleared");
        }
        CHECK((r.gpu_lnksta_after & 0xFu) == 2u && (r.root_lnksta_after & 0xFu) == 2u,
              "final LNKSTA Gen2 both ends");
        CHECK(m.sleeps >= 50, "50ms settle sleep done");
    }

    /* 3. 只读查询：不写任何东西 */
    {
        const unsigned short g1[] = { LNK_G1_X4 };
        mock_init(&m); io = make_io(&m);
        set_script(&m, g1, 1, g1, 1);
        code = cmprt_run(&io, m.gpu_bdf, m.root_bdf, 0, &r);
        printf("[query] code=%u\n", code);
        CHECK(code == CMPRT_OK_QUERY, "code OK_QUERY");
        CHECK(m.wn == 0 && m.mn == 0, "query performs no writes");
        CHECK(r.bar0_before[7] == 0x068731B7u, "snapshot captured CYA_0");
    }

    /* 4. 轮询超时：始终 Gen1 -> 恢复根端策略 */
    {
        const unsigned short g1[] = { LNK_G1_X4 };
        mock_init(&m); io = make_io(&m);
        set_script(&m, g1, 1, g1, 1);
        code = cmprt_run(&io, m.gpu_bdf, m.root_bdf, 1, &r);
        printf("[timeout] code=%u polls=%u\n", code, r.poll_count);
        CHECK(code == CMPRT_ERR_POLL_TIMEOUT, "code ERR_POLL_TIMEOUT");
        CHECK(r.poll_count == CMPRT_MAX_POLL, "polled to limit");
        CHECK(last_root_write(&m, ROOT_LNKCTL2_OFF, &v) && (v & 0xFu) == 1u,
              "root TLS restored to 1");
        {
            int rl = 0, i;
            for (i = 0; i < m.wn; i++)
                if (m.wlog[i].bdf == m.root_bdf && m.wlog[i].off == ROOT_LNKCTL_OFF &&
                    (m.wlog[i].val & CMPRT_LNKCTL_RL)) rl++;
            CHECK(rl >= 2, "second Retrain-Link SET issued at 1.5s");
        }
    }

    /* 5. 拓扑不符：次级总线不等于 GPU 总线 */
    {
        const unsigned short g1[] = { LNK_G1_X4 };
        mock_init(&m); io = make_io(&m);
        m.root_cfg[0x19] = 0x55;
        set_script(&m, g1, 1, g1, 1);
        code = cmprt_run(&io, m.gpu_bdf, m.root_bdf, 1, &r);
        printf("[topology] code=%u\n", code);
        CHECK(code == CMPRT_ERR_TOPOLOGY, "code ERR_TOPOLOGY");
        CHECK(m.wn == 0 && m.mn == 0, "no writes on topology failure");
    }

    /* 6. BAR0 死亡 */
    {
        const unsigned short g1[] = { LNK_G1_X4 };
        mock_init(&m); io = make_io(&m);
        put32(m.bar0, CMPRT_R_BOOT0, 0xFFFFFFFFu);
        set_script(&m, g1, 1, g1, 1);
        code = cmprt_run(&io, m.gpu_bdf, m.root_bdf, 1, &r);
        printf("[bar0-dead] code=%u\n", code);
        CHECK(code == CMPRT_ERR_BAR0_DEAD, "code ERR_BAR0_DEAD");
        CHECK(m.mn == 0, "no BAR0 writes when dead");
    }

    /* 7. 寄存器卡死：XVE_OVR 写不进 */
    {
        const unsigned short g1[] = { LNK_G1_X4 };
        mock_init(&m); io = make_io(&m);
        m.stuck_off = CMPRT_R_XVE_OVR;
        set_script(&m, g1, 1, g1, 1);
        code = cmprt_run(&io, m.gpu_bdf, m.root_bdf, 1, &r);
        printf("[reg-stuck] code=%u stuck=0x%X\n", code, r.stuck_reg);
        CHECK(code == CMPRT_ERR_REG_STUCK, "code ERR_REG_STUCK");
        CHECK(r.stuck_reg == CMPRT_R_XVE_OVR, "stuck_reg recorded");
    }

    /* 8. 宽度变化：训练后宽度下降 */
    {
        const unsigned short gs[] = { LNK_G1_X4, 0x0012, 0x0012, 0x0012, 0x0012 };
        const unsigned short rs[] = { LNK_G1_X4, 0x0012, 0x0012, 0x0012, 0x0012 };
        mock_init(&m); io = make_io(&m);
        set_script(&m, gs, 5, rs, 5);
        code = cmprt_run(&io, m.gpu_bdf, m.root_bdf, 1, &r);
        printf("[width] code=%u\n", code);
        CHECK(code == CMPRT_ERR_WIDTH_CHANGED, "code ERR_WIDTH_CHANGED");
    }

    /* 9. 链路死亡：全一读取 */
    {
        const unsigned short gd[] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
                                      0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
        mock_init(&m); io = make_io(&m);
        set_script(&m, gd, 11, gd, 11);
        code = cmprt_run(&io, m.gpu_bdf, m.root_bdf, 1, &r);
        printf("[link-down] code=%u\n", code);
        CHECK(code == CMPRT_ERR_LINK_DOWN, "code ERR_LINK_DOWN");
    }

    /* 10. 无 PCIe capability */
    {
        const unsigned short g1[] = { LNK_G1_X4 };
        mock_init(&m); io = make_io(&m);
        m.gpu_cfg[0x34] = 0;
        set_script(&m, g1, 1, g1, 1);
        code = cmprt_run(&io, m.gpu_bdf, m.root_bdf, 1, &r);
        printf("[no-cap] code=%u\n", code);
        CHECK(code == CMPRT_ERR_NO_PCIE_CAP, "code ERR_NO_PCIE_CAP");
    }

    /* 11. GPU ID 不符 */
    {
        const unsigned short g1[] = { LNK_G1_X4 };
        mock_init(&m); io = make_io(&m);
        put32(m.gpu_cfg, 0, 0x12345678u);
        set_script(&m, g1, 1, g1, 1);
        code = cmprt_run(&io, m.gpu_bdf, m.root_bdf, 1, &r);
        printf("[bad-id] code=%u\n", code);
        CHECK(code == CMPRT_ERR_GPU_ID, "code ERR_GPU_ID");
    }

    printf("== %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
