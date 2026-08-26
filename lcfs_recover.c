#define _POSIX_C_SOURCE 200809L

#include "lcfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

#define LOST_FOUND_NAME ".lost+found"

typedef struct {
    lcfs_oid_t oid;
    uint16_t type;
    uint64_t block;
    uint32_t flags;
    uint32_t generation;
    lcfs_oid_t parent_oid;
    char name[LCFS_MAX_NAME_LEN + 1];
    int valid;
} object_info;

static void make_unique_name(lcfs_oid_t oid, const char *orig_name,
                             char *out, size_t out_size) {
    if (orig_name && orig_name[0] != '\0' && strlen(orig_name) <= LCFS_MAX_NAME_LEN - 4) {
        snprintf(out, out_size, "id_%s", orig_name);
    } else {
        snprintf(out, out_size, "id_%llu", (unsigned long long)oid);
    }
                             }

                             static int clear_directory_entries(int fd, lcfs_oid_t dir_oid) {
                                 uint64_t dir_block;
                                 if (lcfs_object_location(fd, dir_oid, &dir_block) < 0)
                                     return -1;
                                 uint8_t block[LCFS_BLOCK_SIZE];
                                 if (lcfs_read_block(fd, dir_block, block) < 0)
                                     return -1;

                                 uint16_t dname_len;
                                 memcpy(&dname_len, block + LCFS_HEADER_SIZE, 2);
                                 size_t data_start = LCFS_HEADER_SIZE + 2 + dname_len;

                                 memset(block + data_start, 0, LCFS_BLOCK_SIZE - data_start);

                                 lcfs_obj_header hdr;
                                 memcpy(&hdr, block, sizeof(hdr));
                                 hdr.size = 0;
                                 if (lcfs_write_header(fd, dir_block, &hdr) < 0)
                                     return -1;
                                 return 0;
                             }

                             static int is_oid_in_dir(int fd, lcfs_oid_t dir_oid, lcfs_oid_t target_oid) {
                                 uint64_t dir_block;
                                 if (lcfs_object_location(fd, dir_oid, &dir_block) < 0)
                                     return 0;
                                 uint8_t block[LCFS_BLOCK_SIZE];
                                 if (lcfs_read_block(fd, dir_block, block) < 0)
                                     return 0;
                                 uint16_t dname_len;
                                 memcpy(&dname_len, block + LCFS_HEADER_SIZE, 2);
                                 size_t pos = LCFS_HEADER_SIZE + 2 + dname_len;
                                 while (pos + sizeof(lcfs_dir_entry) <= LCFS_BLOCK_SIZE) {
                                     lcfs_dir_entry entry;
                                     memcpy(&entry, block + pos, sizeof(entry));
                                     if (entry.child_oid == 0)
                                         break;
                                     if (entry.child_oid == target_oid)
                                         return 1;
                                     if (entry.name_len == 0 || entry.name_len > LCFS_MAX_NAME_LEN)
                                         break;
                                     pos += sizeof(entry) + entry.name_len;
                                 }
                                 return 0;
                             }

                             int main(int argc, char *argv[]) {
                                 if (argc != 2) {
                                     fprintf(stderr, "Uso: %s <dispositivo|imagen>\n", argv[0]);
                                     return 1;
                                 }

                                 int fd = open(argv[1], O_RDWR);
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
                                     if (lcfs_validate_header(&hdr) == 0) {
                                         object_info *obj = &objects[valid_objects];
                                         obj->oid = hdr.oid;
                                         obj->type = hdr.type;
                                         obj->block = b;
                                         obj->flags = hdr.flags;
                                         obj->generation = hdr.generation;
                                         obj->parent_oid = hdr.parent_oid;
                                         obj->valid = 1;

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

                                 printf("\nLista de objetos válidos:\n");
                                 for (uint64_t i = 0; i < valid_objects; i++) {
                                     if (objects[i].valid) {
                                         printf("  OID %llu | Tipo %u | Padre %llu | Nombre '%s'\n",
                                                (unsigned long long)objects[i].oid,
                                                objects[i].type,
                                                (unsigned long long)objects[i].parent_oid,
                                                objects[i].name);
                                     }
                                 }

                                 lcfs_oid_t root_oid = 0;
                                 for (uint64_t i = 0; i < valid_objects; i++) {
                                     if (objects[i].valid && objects[i].type == OBJ_TYPE_DIR && objects[i].parent_oid == 0) {
                                         root_oid = objects[i].oid;
                                         break;
                                     }
                                 }
                                 if (root_oid == 0) {
                                     printf("\nRoot: NOT FOUND\n");
                                     free(objects);
                                     close(fd);
                                     return 1;
                                 }
                                 printf("\nRoot: FOUND (OID %llu)\n", (unsigned long long)root_oid);

                                 // Reconstruir free map y OID map
                                 printf("\nReconstruyendo free map y OID map...\n");
                                 uint8_t *bitmap = NULL;
                                 uint64_t bm_blocks = 0;
                                 if (lcfs_rebuild_free_map(fd, &bitmap, &bm_blocks) == 0) {
                                     if (lcfs_set_free_map(fd, bitmap, bm_blocks) == 0)
                                         printf("Free map reconstruido.\n");
                                     free(bitmap);
                                 }
                                 if (lcfs_rebuild_oid_map(fd) == 0)
                                     printf("OID map reconstruido.\n");

                                 // Reconstruir entradas de directorios
                                 printf("\nReconstruyendo entradas de directorios...\n");
                                 for (uint64_t i = 0; i < valid_objects; i++) {
                                     if (!objects[i].valid || objects[i].type != OBJ_TYPE_DIR)
                                         continue;

                                     if (clear_directory_entries(fd, objects[i].oid) < 0) {
                                         printf("  Error al limpiar directorio OID %llu\n", (unsigned long long)objects[i].oid);
                                         continue;
                                     }

                                     uint32_t added = 0;
                                     for (uint64_t j = 0; j < valid_objects; j++) {
                                         if (objects[j].valid && objects[j].parent_oid == objects[i].oid) {
                                             if (lcfs_add_dir_entry(fd, objects[i].oid, objects[j].oid,
                                                 objects[j].type, objects[j].name) == 0) {
                                                 added++;
                                                 } else {
                                                     printf("  Error al añadir '%s' a directorio OID %llu\n",
                                                            objects[j].name, (unsigned long long)objects[i].oid);
                                                 }
                                         }
                                     }
                                     printf("  Directorio OID %llu: %u entradas reconstruidas\n",
                                            (unsigned long long)objects[i].oid, added);
                                 }

                                 // Crear o limpiar .lost+found
                                 lcfs_oid_t lost_found_oid = 0;
                                 for (uint64_t i = 0; i < valid_objects; i++) {
                                     if (objects[i].valid && objects[i].type == OBJ_TYPE_DIR &&
                                         objects[i].parent_oid == root_oid &&
                                         strcmp(objects[i].name, LOST_FOUND_NAME) == 0) {
                                         lost_found_oid = objects[i].oid;
                                     break;
                                         }
                                 }

                                 if (lost_found_oid == 0) {
                                     printf("\nCreando directorio %s...\n", LOST_FOUND_NAME);
                                     if (lcfs_create_dir(fd, root_oid, LOST_FOUND_NAME, &lost_found_oid) < 0) {
                                         perror("lcfs_create_dir");
                                         free(objects);
                                         close(fd);
                                         return 1;
                                     }
                                     object_info *tmp = realloc(objects, (valid_objects + 1) * sizeof(object_info));
                                     if (!tmp) {
                                         free(objects);
                                         close(fd);
                                         return 1;
                                     }
                                     objects = tmp;
                                     objects[valid_objects].oid = lost_found_oid;
                                     objects[valid_objects].type = OBJ_TYPE_DIR;
                                     objects[valid_objects].block = 0;
                                     objects[valid_objects].parent_oid = root_oid;
                                     strcpy(objects[valid_objects].name, LOST_FOUND_NAME);
                                     objects[valid_objects].valid = 1;
                                     valid_objects++;
                                 } else {
                                     clear_directory_entries(fd, lost_found_oid);
                                 }

                                 printf("\nDetectando huérfanos...\n");
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
                                         if (is_oid_in_dir(fd, lost_found_oid, objects[i].oid)) {
                                             printf("  OID %llu ya está en .lost+found, se omite.\n",
                                                    (unsigned long long)objects[i].oid);
                                             continue;
                                         }

                                         orphan_count++;
                                         char new_name[LCFS_MAX_NAME_LEN + 1];
                                         make_unique_name(objects[i].oid, objects[i].name, new_name, sizeof(new_name));
                                         printf("  Huérfano OID %llu -> %s\n", (unsigned long long)objects[i].oid, new_name);

                                         if (lcfs_add_dir_entry(fd, lost_found_oid, objects[i].oid,
                                             objects[i].type, new_name) == 0) {
                                             uint64_t obj_block;
                                         if (lcfs_object_location(fd, objects[i].oid, &obj_block) == 0) {
                                             lcfs_obj_header hdr;
                                             if (lcfs_read_header(fd, obj_block, &hdr) == 0) {
                                                 hdr.parent_oid = lost_found_oid;
                                                 lcfs_write_header(fd, obj_block, &hdr);
                                             }
                                         }
                                             }
                                     }
                                 }
                                 printf("Huérfanos movidos: %llu\n", (unsigned long long)orphan_count);

                                 printf("\nRecovery completed.\n");
                                 free(objects);
                                 close(fd);
                                 return 0;
                             }
