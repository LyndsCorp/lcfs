#include "lcfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <dispositivo|imagen>\n", argv[0]);
        return 1;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    lcfs_obj_header sb;
    if (lcfs_read_header(fd, LCFS_SUPERBLOCK_OID, &sb) == 0) {
        printf("LCFS Superblock:\n");
        printf("  Version: 0x%04x\n", sb.version);
        printf("  Total blocks: %u\n", sb.size);
        printf("  Root OID: %llu\n", (unsigned long long)sb.first_child_oid);
        printf("  Next OID: %u\n", sb.generation);
    } else {
        printf("Superblock no encontrado (posiblemente corrupto)\n");
    }
    close(fd);
    return 0;
}
