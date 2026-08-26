#define _POSIX_C_SOURCE 200809L

#include "lcfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <dispositivo|imagen> <tamaño_en_megabytes>\n", argv[0]);
        return 1;
    }
    const char *path = argv[1];
    uint64_t size_mb = strtoull(argv[2], NULL, 10);
    uint64_t total_bytes = size_mb * 1024ULL * 1024ULL;
    uint64_t total_blocks = total_bytes / LCFS_BLOCK_SIZE;
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    if (ftruncate(fd, total_bytes) < 0) {
        perror("ftruncate");
        close(fd);
        return 1;
    }
    if (lcfs_init_superblock(fd, total_blocks) < 0) {
        fprintf(stderr, "Error al inicializar LCFS\n");
        close(fd);
        return 1;
    }
    close(fd);
    printf("LCFS filesystem creado en %s (%llu bloques)\n", path, (unsigned long long)total_blocks);
    return 0;
}
