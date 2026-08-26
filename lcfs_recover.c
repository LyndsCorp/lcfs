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

// Genera un nombre único en .lost+found basado en el OID y nombre original
static void make_unique_name(lcfs_oid_t oid, const char *orig_name,
                             char *out, size_t out_size) {
    if (orig_name && orig_name[0] != '\0' && strlen(orig_name) <= LCFS_MAX_NAME_LEN - 4) {
        snprintf(out, out_size, "id_%s", orig_name);
    } else {
        snprintf(out, out_size, "id_%llu", (unsigned long long)oid);
    }
                             }

                             // Elimina todas las entradas de un directorio (usado para limpiar .lost+found)
                             static int clear_directory_entries(int fd, lcfs_oid_t dir_oid) {
                                 uint64_t dir_block;
                                 if (lcfs_object_location(fd, dir_oid, &dir_block) < 0)
                                     return -1;
                                 uint8_t block[LCFS_BLOCK_SIZE];
                                 if (lcfs_read_block(fd, dir_block, block) < 0)
                                     return -1;

                                 // Obtener el offset donde empiezan las entradas
                                 uint16_t dname_len;
                                 memcpy(&dname_len, block + LCFS_HEADER_SIZE, 2);
                                 size_t data_start = LCFS_HEADER_SIZE + 2 + dname_len;

                                 // Poner a cero toda la zona de entradas
                                 memset(block + data_start, 0, LCFS_BLOCK_SIZE - data_start);

                                 // Actualizar el encabezado: size = 0
                                 lcfs_obj_header hdr;
                                 memcpy(&hdr, block, sizeof(hdr));
                                 hdr.size = 0;
                                 if (lcfs_write_header(fd, dir_block, &hdr) < 0)
                                     return -1;
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

                                 // Buscar o crear .lost+found
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
                                     printf("Creando directorio %s...\n", LOST_FOUND_NAME);
                                     if (lcfs_create_dir(fd, root_oid, LOST_FOUND_NAME, &lost_found_oid) < 0) {
                                         perror("lcfs_create_dir");
                                         free(objects);
                                         close(fd);
                                         return 1;
                                     }
                                     // Añadir a la lista
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
                                     // Limpiar .lost+found para evitar duplicados de ejecuciones anteriores
                                     printf("Limpiando entradas de %s...\n", LOST_FOUND_NAME);
                                     if (clear_directory_entries(fd, lost_found_oid) < 0) {
                                         printf("Error al limpiar %s\n", LOST_FOUND_NAME);
                                     }
                                 }

                                 printf("\nReconstructing filesystem graph...\n");
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

                                     // Si el padre existe, comprobar que el hijo esté listado
                                     if (parent_found) {
                                         lcfs_oid_t dummy_oid;
                                         uint16_t dummy_type;
                                         if (lcfs_lookup_name(fd, objects[i].parent_oid, objects[i].name,
                                             &dummy_oid, &dummy_type) != 0) {
                                             parent_found = 0;
                                             }
                                     }

                                     if (!parent_found) {
                                         orphan_count++;
                                         char new_name[LCFS_MAX_NAME_LEN + 1];
                                         make_unique_name(objects[i].oid, objects[i].name, new_name, sizeof(new_name));

                                         printf("  Orphan: OID %llu (original: '%s') -> %s\n",
                                                (unsigned long long)objects[i].oid,
                                                objects[i].name[0] ? objects[i].name : "(vacío)",
                                                new_name);

                                         // Añadir entrada en .lost+found
                                         if (lcfs_add_dir_entry(fd, lost_found_oid, objects[i].oid,
                                             objects[i].type, new_name) == 0) {
                                             // Actualizar parent_oid
                                             uint64_t obj_block;
                                         if (lcfs_object_location(fd, objects[i].oid, &obj_block) == 0) {
                                             lcfs_obj_header hdr;
                                             if (lcfs_read_header(fd, obj_block, &hdr) == 0) {
                                                 hdr.parent_oid = lost_found_oid;
                                                 if (lcfs_write_header(fd, obj_block, &hdr) == 0) {
                                                     printf("    Movido correctamente.\n");
                                                 } else {
                                                     printf("    Error al actualizar parent_oid.\n");
                                                 }
                                             }
                                         }
                                             } else {
                                                 printf("    Error al añadir entrada a .lost+found.\n");
                                             }
                                     }
                                 }

                                 printf("Orphaned objects: %llu\n", (unsigned long long)orphan_count);
                                 printf("\nRecovery completed.\n");

                                 free(objects);
                                 close(fd);
                                 return 0;
                             }
