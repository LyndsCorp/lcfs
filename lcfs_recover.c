#include "lcfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

typedef struct {
    lcfs_oid_t oid;
    uint16_t type;
    uint64_t block;
    uint32_t flags;
    uint32_t generation;
    lcfs_oid_t parent_oid;
    char name[LCFS_MAX_NAME_LEN+1];
    int valid;
} object_info;

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
    off_t dev_size = lseek(fd, 0, SEEK_END);
    uint64_t total_blocks = dev_size / LCFS_BLOCK_SIZE;
    printf("LCFS Recovery\n\n");
    printf("Scanning blocks...\n");
    object_info *objects = calloc(total_blocks, sizeof(object_info));
    if (!objects) {
        close(fd);
        return 1;
    }
    uint64_t total_objects = 0, valid_objects = 0, corrupted_objects = 0;
    for (uint64_t b = 0; b < total_blocks; b++) {
        uint8_t block[LCFS_BLOCK_SIZE];
        if (lcfs_read_block(fd, b, block) < 0) continue;
        lcfs_obj_header hdr;
        memcpy(&hdr, block, sizeof(hdr));
        if (memcmp(hdr.magic, LCFS_MAGIC, LCFS_MAGIC_LEN) != 0) continue;
        total_objects++;
        if (hdr.header_crc == header_crc(&hdr)) {
            object_info *obj = &objects[valid_objects];
            obj->oid = hdr.oid;
            obj->type = hdr.type;
            obj->block = b;
            obj->flags = hdr.flags;
            obj->generation = hdr.generation;
            obj->parent_oid = hdr.parent_oid;
            obj->valid = 1;
            // Leer nombre
            uint16_t name_len;
            memcpy(&name_len, block + LCFS_HEADER_SIZE, 2);
            if (name_len > LCFS_MAX_NAME_LEN) name_len = LCFS_MAX_NAME_LEN;
            memcpy(obj->name, block + LCFS_HEADER_SIZE + 2, name_len);
            obj->name[name_len] = '\0';
            valid_objects++;
        } else {
            corrupted_objects++;
        }
    }
    printf("Objects found: %llu\n", (unsigned long long)total_objects);
    printf("Valid objects: %llu\n", (unsigned long long)valid_objects);
    printf("Corrupted objects: %llu\n", (unsigned long long)corrupted_objects);

    // Encontrar root: objeto DIR con parent_oid=0
    lcfs_oid_t root_oid = 0;
    for (uint64_t i = 0; i < valid_objects; i++) {
        if (objects[i].valid && objects[i].type == OBJ_TYPE_DIR && objects[i].parent_oid == 0) {
            root_oid = objects[i].oid;
            break;
        }
    }
    if (root_oid == 0) {
        printf("Root: NOT FOUND\n");
        free(objects);
        close(fd);
        return 1;
    }
    printf("Root: FOUND (OID %llu)\n", (unsigned long long)root_oid);

    // Detectar huérfanos (parent_oid no existe o no es directorio)
    uint64_t orphan_count = 0;
    for (uint64_t i = 0; i < valid_objects; i++) {
        if (!objects[i].valid || objects[i].parent_oid == 0) continue;
        int parent_found = 0;
        for (uint64_t j = 0; j < valid_objects; j++) {
            if (objects[j].valid && objects[j].oid == objects[i].parent_oid &&
                objects[j].type == OBJ_TYPE_DIR) {
                parent_found = 1;
            break;
                }
        }
        if (!parent_found) {
            orphan_count++;
            // Podríamos mover a lost+found (no implementado aquí)
            printf("  Orphan: %s (OID %llu, parent %llu)\n", objects[i].name,
                   (unsigned long long)objects[i].oid,
                   (unsigned long long)objects[i].parent_oid);
        }
    }
    printf("Orphaned objects: %llu\n", (unsigned long long)orphan_count);
    printf("\nReconstructing filesystem graph...\n");
    // Aquí se implementaría la reconstrucción real y escritura de free map/oid map
    printf("Recovery completed.\n");
    free(objects);
    close(fd);
    return 0;
}
