/* 一次性诊断：用户态 GetSystemFirmwareTable('ACPI','MCFG') 是否被系统拒绝 */
#include <windows.h>
#include <stdio.h>

int main(void)
{
    UINT need, got;
    BYTE buf[4096];
    DWORD i, count;
    ULONGLONG base;
    USHORT seg;
    UCHAR startBus, endBus;

    SetLastError(0);
    need = GetSystemFirmwareTable(0x49504341u /*'ACPI'*/, 0x4746434Du /*'MCFG'*/,
                                  NULL, 0);
    printf("size-query: need=%u gle=%lu\n", need, GetLastError());
    if (need == 0 || need > sizeof(buf)) return 1;

    SetLastError(0);
    got = GetSystemFirmwareTable(0x49504341u, 0x4746434Du, buf, sizeof(buf));
    printf("fetch: got=%u gle=%lu sig=%02X %02X %02X %02X\n",
           got, GetLastError(), buf[0], buf[1], buf[2], buf[3]);
    if (!got) return 2;

    count = (got - 44) / 16;
    printf("entries=%lu\n", count);
    for (i = 0; i < count; i++) {
        BYTE *e = buf + 44 + i * 16;
        memcpy(&base, e + 0, 8);
        memcpy(&seg, e + 8, 2);
        startBus = e[10]; endBus = e[11];
        printf("  [%lu] base=0x%llX seg=%u bus %u-%u\n",
               i, base, seg, startBus, endBus);
    }
    return 0;
}
