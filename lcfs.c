#include "lcfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>

// CRC32C (Castagnoli)
static uint32_t crc32c_table[256];
static int crc32c_initialized = 0;

static void init_crc32c_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0x82F63B78 : 0);
        }
        crc32c_table[i] = crc;
    }
    crc32c_initialized = 1;
}

uint32_t lcfs_crc32c(uint32_t crc, const void *buf, size_t len) {
    if (!crc32c_initialized) init_crc32c_table();
    const uint8_t *p = buf;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc = crc32c_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

// Calcular CRC de un encabezado
static uint32_t header_crc(const lcfs_obj_header *hdr) {
    lcfs_obj_header tmp = *hdr;
    tmp.header_crc = 0;
    return lcfs_crc32c(0, &tmp, sizeof(tmp));
}

// Lectura/escritura de bloques
int lcfs_read_block(int fd, uint64_t block_num, void *buf) {
    off_t off = (off_t)block_num * LCFS_BLOCK_SIZE;
    if (lseek(fd, off, SEEK_SET) < 0) return -1;
    if (read(fd, buf, LCFS_BLOCK_SIZE) != LCFS_BLOCK_SIZE) return -1;
    return 0;
}

int lcfs_write_block(int fd, uint64_t block_num, const void *buf) {
    off_t off = (off_t)block_num * LCFS_BLOCK_SIZE;
    if (lseek(fd, off, SEEK_SET) < 0) return -1;
    if (write(fd, buf, LCFS_BLOCK_SIZE) != LCFS_BLOCK_SIZE) return -1;
    return 0;
}

int lcfs_read_header(int fd, uint64_t block_num, lcfs_obj_header *hdr) {
    uint8_t block[LCFS_BLOCK_SIZE];
    if (lcfs_read_block(fd, block_num, block) < 0) return -1;
    memcpy(hdr, block, sizeof(*hdr));
    if (memcmp(hdr->magic, LCFS_MAGIC, LCFS_MAGIC_LEN) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (hdr->header_crc != header_crc(hdr)) {
        errno = EBADMSG;
        return -1;
    }
    return 0;
}

int lcfs_write_header(int fd, uint64_t block_num, const lcfs_obj_header *hdr) {
    lcfs_obj_header tmp = *hdr;
    tmp.header_crc = 0;
    tmp.header_crc = header_crc(&tmp);
    uint8_t block[LCFS_BLOCK_SIZE] = {0};
    memcpy(block, &tmp, sizeof(tmp));
    if (lcfs_write_block(fd, block_num, block) < 0) return -1;
    return 0;
}

// Obtener mapa de bits de bloques libres
int lcfs_get_free_map(int fd, uint8_t **bitmap, uint64_t *bitmap_blocks) {
    // El mapa se almacena como objeto especial con OID=1
    lcfs_obj_header hdr;
    if (lcfs_read_header(fd, LCFS_FREE_MAP_OID, &hdr) < 0) return -1;
    if (hdr.type != OBJ_TYPE_FREE_MAP) return -1;
    uint64_t bm_blocks = (hdr.size + LCFS_BLOCK_SIZE - 1) / LCFS_BLOCK_SIZE;
    uint8_t *bm = malloc(bm_blocks * LCFS_BLOCK_SIZE);
    if (!bm) return -1;
    // Leer todos los bloques del objeto
    for (uint64_t i = 0; i < bm_blocks; i++) {
        if (lcfs_read_block(fd, hdr.num_extents + i, bm + i*LCFS_BLOCK_SIZE) < 0) {
            free(bm);
            return -1;
        }
    }
    *bitmap = bm;
    *bitmap_blocks = bm_blocks;
    return 0;
}

int lcfs_set_free_map(int fd, const uint8_t *bitmap, uint64_t bitmap_blocks) {
    // Asumimos que el objeto ya existe, solo actualizamos su contenido
    lcfs_obj_header hdr;
    if (lcfs_read_header(fd, LCFS_FREE_MAP_OID, &hdr) < 0) return -1;
    hdr.size = bitmap_blocks * LCFS_BLOCK_SIZE;
    hdr.num_extents = bitmap_blocks; // Bloques de datos
    // Escribir header
    if (lcfs_write_header(fd, LCFS_FREE_MAP_OID, &hdr) < 0) return -1;
    // Escribir bitmap
    for (uint64_t i = 0; i < bitmap_blocks; i++) {
        if (lcfs_write_block(fd, hdr.num_extents + i, bitmap + i*LCFS_BLOCK_SIZE) < 0) return -1;
    }
    return 0;
}

// Asignar un bloque libre (simple búsqueda en bitmap)
int lcfs_alloc_block(int fd, uint64_t *block_num) {
    uint8_t *bitmap;
    uint64_t bm_blocks;
    if (lcfs_get_free_map(fd, &bitmap, &bm_blocks) < 0) return -1;
    uint64_t total_bits = bm_blocks * LCFS_BLOCK_SIZE * 8;
    for (uint64_t i = 0; i < total_bits; i++) {
        uint64_t byte_idx = i / 8;
        uint8_t bit = 1 << (i % 8);
        if (!(bitmap[byte_idx] & bit)) {
            bitmap[byte_idx] |= bit;
            if (lcfs_set_free_map(fd, bitmap, bm_blocks) < 0) {
                free(bitmap);
                return -1;
            }
            free(bitmap);
            *block_num = i;
            return 0;
        }
    }
    free(bitmap);
    errno = ENOSPC;
    return -1;
}

int lcfs_free_block(int fd, uint64_t block_num) {
    uint8_t *bitmap;
    uint64_t bm_blocks;
    if (lcfs_get_free_map(fd, &bitmap, &bm_blocks) < 0) return -1;
    uint64_t byte_idx = block_num / 8;
    uint8_t bit = 1 << (block_num % 8);
    bitmap[byte_idx] &= ~bit;
    if (lcfs_set_free_map(fd, bitmap, bm_blocks) < 0) {
        free(bitmap);
        return -1;
    }
    free(bitmap);
    return 0;
}

// Crear un nuevo objeto
int lcfs_create_object(int fd, uint16_t type, lcfs_oid_t parent_oid,
                       const char *name, lcfs_oid_t *new_oid, uint64_t *block_num) {
    uint64_t blk;
    if (lcfs_alloc_block(fd, &blk) < 0) return -1;
    lcfs_obj_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, LCFS_MAGIC, LCFS_MAGIC_LEN);
    hdr.type = type;
    hdr.version = LCFS_VERSION;
    hdr.parent_oid = parent_oid;
    // Generar OID simple: usar el bloque como OID (temporal)
    hdr.oid = blk; // Para simplificar, OID = bloque físico
    // Nombre
    uint8_t block[LCFS_BLOCK_SIZE] = {0};
    size_t name_len = strlen(name);
    if (name_len > LCFS_MAX_NAME_LEN) name_len = LCFS_MAX_NAME_LEN;
    memcpy(block + LCFS_HEADER_SIZE, &name_len, 2);
    memcpy(block + LCFS_HEADER_SIZE + 2, name, name_len);
    // Escribir header
    hdr.header_crc = 0;
    hdr.header_crc = header_crc(&hdr);
    memcpy(block, &hdr, sizeof(hdr));
    if (lcfs_write_block(fd, blk, block) < 0) {
        lcfs_free_block(fd, blk);
        return -1;
    }
    if (new_oid) *new_oid = hdr.oid;
    if (block_num) *block_num = blk;
    return 0;
                       }

                       // Eliminar un objeto (marcar como libre y borrar entrada padre)
                       int lcfs_delete_object(int fd, lcfs_oid_t oid) {
                           // En esta versión solo liberamos el bloque del objeto y sus extents
                           lcfs_obj_header hdr;
                           if (lcfs_read_header(fd, oid, &hdr) < 0) return -1;
                           // Liberar bloques de extents si los hay
                           // (omitido por brevedad)
                           lcfs_free_block(fd, oid);
                           // TODO: eliminar entrada del directorio padre
                           return 0;
                       }

                       // Buscar un nombre en un directorio
                       int lcfs_lookup_name(int fd, lcfs_oid_t dir_oid, const char *name,
                                            lcfs_oid_t *child_oid, uint16_t *child_type) {
                           lcfs_obj_header hdr;
                           if (lcfs_read_header(fd, dir_oid, &hdr) < 0) return -1;
                           if (hdr.type != OBJ_TYPE_DIR) return -1;

                           uint8_t block[LCFS_BLOCK_SIZE];
                           if (lcfs_read_block(fd, dir_oid, block) < 0) return -1;

                           // Leer entradas a partir de LCFS_HEADER_SIZE + 2 + name_len (nombre del directorio)
                           uint16_t dname_len;
                           memcpy(&dname_len, block + LCFS_HEADER_SIZE, 2);
                           size_t pos = LCFS_HEADER_SIZE + 2 + dname_len;

                           while (pos + sizeof(lcfs_dir_entry) <= LCFS_BLOCK_SIZE) {
                               lcfs_dir_entry entry;
                               memcpy(&entry, block + pos, sizeof(entry));
                               if (entry.child_oid == 0) break; // fin
                               pos += sizeof(entry);
                               char entry_name[LCFS_MAX_NAME_LEN+1];
                               memcpy(entry_name, block + pos, entry.name_len);
                               entry_name[entry.name_len] = '\0';
                               pos += entry.name_len;
                               if (strcmp(entry_name, name) == 0) {
                                   if (child_oid) *child_oid = entry.child_oid;
                                   if (child_type) *child_type = entry.child_type;
                                   return 0;
                               }
                           }
                           errno = ENOENT;
                           return -1;
                                            }

                                            // Añadir entrada a directorio
                                            int lcfs_add_dir_entry(int fd, lcfs_oid_t dir_oid, lcfs_oid_t child_oid,
                                                                   uint16_t child_type, const char *name) {
                                                lcfs_obj_header hdr;
                                                if (lcfs_read_header(fd, dir_oid, &hdr) < 0) return -1;
                                                if (hdr.type != OBJ_TYPE_DIR) return -1;

                                                uint8_t block[LCFS_BLOCK_SIZE];
                                                if (lcfs_read_block(fd, dir_oid, block) < 0) return -1;

                                                uint16_t dname_len;
                                                memcpy(&dname_len, block + LCFS_HEADER_SIZE, 2);
                                                size_t pos = LCFS_HEADER_SIZE + 2 + dname_len;

                                                // Buscar espacio libre en el bloque
                                                while (pos + sizeof(lcfs_dir_entry) <= LCFS_BLOCK_SIZE) {
                                                    lcfs_dir_entry entry;
                                                    memcpy(&entry, block + pos, sizeof(entry));
                                                    if (entry.child_oid == 0) {
                                                        // Escribir entrada aquí
                                                        entry.child_oid = child_oid;
                                                        entry.child_type = child_type;
                                                        entry.name_len = strlen(name);
                                                        if (entry.name_len > LCFS_MAX_NAME_LEN) entry.name_len = LCFS_MAX_NAME_LEN;
                                                        memcpy(block + pos, &entry, sizeof(entry));
                                                        memcpy(block + pos + sizeof(entry), name, entry.name_len);
                                                        // Actualizar header: size = número de entradas
                                                        hdr.size++;
                                                        hdr.header_crc = 0;
                                                        hdr.header_crc = header_crc(&hdr);
                                                        memcpy(block, &hdr, sizeof(hdr));
                                                        if (lcfs_write_block(fd, dir_oid, block) < 0) return -1;
                                                        return 0;
                                                    }
                                                    pos += sizeof(entry) + entry.name_len;
                                                }
                                                // Si no cabe en el bloque, necesitaríamos extent de metadatos (no implementado)
                                                errno = ENOSPC;
                                                return -1;
                                                                   }

                                                                   // Inicializar superblock y root
                                                                   int lcfs_init_superblock(int fd, uint64_t total_blocks) {
                                                                       // Crear superbloque en bloque 0
                                                                       lcfs_obj_header sb;
                                                                       memset(&sb, 0, sizeof(sb));
                                                                       memcpy(sb.magic, LCFS_MAGIC, LCFS_MAGIC_LEN);
                                                                       sb.type = OBJ_TYPE_SUPERBLOCK;
                                                                       sb.version = LCFS_VERSION;
                                                                       sb.oid = LCFS_SUPERBLOCK_OID;
                                                                       sb.size = sizeof(sb); // no usado realmente
                                                                       sb.num_extents = 0;
                                                                       sb.flags = 0;
                                                                       sb.parent_oid = 0;
                                                                       sb.header_crc = 0;
                                                                       sb.header_crc = header_crc(&sb);
                                                                       if (lcfs_write_block(fd, 0, &sb) < 0) return -1;

                                                                       // Crear free map en bloque 1
                                                                       uint64_t bm_blocks = (total_blocks + LCFS_BLOCK_SIZE*8 - 1) / (LCFS_BLOCK_SIZE*8);
                                                                       uint8_t *bitmap = calloc(bm_blocks, LCFS_BLOCK_SIZE);
                                                                       if (!bitmap) return -1;
                                                                       // Marcar bloque 0 y 1 como ocupados
                                                                       bitmap[0] |= 0x03; // bits 0 y 1
                                                                       // Crear objeto free map
                                                                       lcfs_obj_header fm;
                                                                       memset(&fm, 0, sizeof(fm));
                                                                       memcpy(fm.magic, LCFS_MAGIC, LCFS_MAGIC_LEN);
                                                                       fm.type = OBJ_TYPE_FREE_MAP;
                                                                       fm.version = LCFS_VERSION;
                                                                       fm.oid = LCFS_FREE_MAP_OID;
                                                                       fm.size = bm_blocks * LCFS_BLOCK_SIZE;
                                                                       fm.num_extents = bm_blocks;
                                                                       fm.header_crc = 0;
                                                                       fm.header_crc = header_crc(&fm);
                                                                       if (lcfs_write_block(fd, 1, &fm) < 0) { free(bitmap); return -1; }
                                                                       // Escribir bitmap en bloques siguientes (a partir del 2)
                                                                       for (uint64_t i = 0; i < bm_blocks; i++) {
                                                                           if (lcfs_write_block(fd, 2 + i, bitmap + i*LCFS_BLOCK_SIZE) < 0) {
                                                                               free(bitmap);
                                                                               return -1;
                                                                           }
                                                                       }
                                                                       free(bitmap);

                                                                       // Crear root en bloque 3 (asumiendo que total_blocks > 4)
                                                                       uint64_t root_blk;
                                                                       lcfs_create_object(fd, OBJ_TYPE_DIR, 0, "/", NULL, &root_blk);
                                                                       // Ajustar OID del root a 3 (por convención)
                                                                       lcfs_obj_header hdr;
                                                                       lcfs_read_header(fd, root_blk, &hdr);
                                                                       hdr.oid = LCFS_ROOT_OID;
                                                                       hdr.header_crc = 0;
                                                                       hdr.header_crc = header_crc(&hdr);
                                                                       lcfs_write_header(fd, root_blk, &hdr);

                                                                       // Actualizar superblock con root oid
                                                                       lcfs_read_header(fd, 0, &sb);
                                                                       sb.size = LCFS_ROOT_OID; // almacenar root oid en size (temporal)
                                                                       sb.header_crc = 0;
                                                                       sb.header_crc = header_crc(&sb);
                                                                       lcfs_write_block(fd, 0, &sb);

                                                                       return 0;
                                                                   }
