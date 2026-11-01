// SPDX-License-Identifier: BSD-2-Clause-Patent
// Runs the driver's own GPT locator (SdStore.h odin_store::locate) over a disk image file, so the tool that creates the
// partition (scripts/h1/sd_data_partition.py) is checked against the code that must accept it at boot.
//   usage: locate_image IMAGE GUID   -> "OK first=<lba> sectors=<n>" (exit 0) or "FAIL <why>" (exit 1)
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include "SdStore.h"
struct FileIO {
    int fd;
    bool read(uint64_t lba, uint32_t count, void *dst) {
        return pread(fd, dst, size_t(count) * 512, off_t(lba * 512)) == ssize_t(size_t(count) * 512);
    }
};
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) return 2;
    uint64_t sectors = uint64_t(lseek(fd, 0, SEEK_END)) / 512;
    uint8_t expected[16];
    if (!odin_store::guid(argv[2], expected)) { puts("FAIL bad guid"); return 1; }
    static uint8_t tables[2][odin_store::kTableBytes];
    odin_store::Region region;
    const char *why = "";
    FileIO io{fd};
    if (!odin_store::locate(io, sectors, sectors - 8192, expected, region, tables[0], tables[1], &why)) {
        printf("FAIL %s\n", why);
        return 1;
    }
    printf("OK first=%llu sectors=%llu\n", (unsigned long long) region.first, (unsigned long long) region.sectors);
    return 0;
}
