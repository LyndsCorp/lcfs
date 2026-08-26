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

static uint32_t header_crc(const lcfs_obj_header *hdr) {
    lcfs_obj_header tmp = *hdr;
    tmp.header_crc = 0;
    return lcfs_crc32c(0, &tmp, sizeof(tmp));
}

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

int lcfs_read_object_name(int fd, uint64_t block_num, char *name, size_t max_len) {
    uint8_t block[LCFS_BLOCK_SIZE];
    if (lcfs_read_block(fd, block_num, block) < 0) return -1;
    uint16_t name_len;
    memcpy(&name_len, block + LCFS_HEADER_SIZE, 2);
    if (name_len >= max_len) name_len = max_len - 1;
    memcpy(name, block + LCFS_HEADER_SIZE + 2, name_len);
    name[name_len] = '\0';
    return 0;
}

int lcfs_init_superblock(int fd, uint64_t total_blocks) {
    // Superbloque en bloque 0
    lcfs_obj_header sb;
    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, LCFS_MAGIC, LCFS_MAGIC_LEN);
    sb.type = OBJ_TYPE_SUPERBLOCK;
    sb.version = LCFS_VERSION;
    sb.oid = LCFS_SUPERBLOCK_OID;
    sb.size = total_blocks;
    sb.num_extents = 0;
    sb.flags = 0;
    sb.parent_oid = 0;
    sb.generation = 1;
    sb.header_crc = 0;
    sb.header_crc = header_crc(&sb);
    if (lcfs_write_block(fd, 0, &sb) < 0) return -1;

    // Crear free map en bloque 1
    uint64_t bm_blocks = (total_blocks + LCFS_BLOCK_SIZE*8 - 1) / (LCFS_BLOCK_SIZE*8);
    uint8_t *bitmap = calloc(bm_blocks, LCFS_BLOCK_SIZE);
    if (!bitmap) return -1;
    // Marcar bloques 0 y 1 ocupados
    bitmap[0] |= 0x03;
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
    if (lcfs_write_block(fd, 1, &fm) < 0) {
        free(bitmap);
        return -1;
    }
    for (uint64_t i = 0; i < bm_blocks; i++) {
        if (lcfs_write_block(fd, 2 + i, bitmap + i*LCFS_BLOCK_SIZE) < 0) {
            free(bitmap);
            return -1;
        }
    }
    free(bitmap);

    // Crear OID map (vacío por ahora) en bloque 2+bm_blocks
    uint64_t oid_map_block = 2 + bm_blocks;
    lcfs_obj_header om;
    memset(&om, 0, sizeof(om));
    memcpy(om.magic, LCFS_MAGIC, LCFS_MAGIC_LEN);
    om.type = OBJ_TYPE_OID_MAP;
    om.version = LCFS_VERSION;
    om.oid = LCFS_OID_MAP_OID;
    om.size = 0;
    om.num_extents = 0;
    om.header_crc = 0;
    om.header_crc = header_crc(&om);
    if (lcfs_write_block(fd, oid_map_block, &om) < 0) return -1;
    // Marcar oid_map_block ocupado en bitmap
    uint64_t byte_idx = oid_map_block / 8;
    uint8_t bit = 1 << (oid_map_block % 8);
    bitmap[byte_idx] |= bit;
    if (lcfs_set_free_map(fd, bitmap, bm_blocks) < 0) return -1;

    // Crear root en el siguiente bloque libre
    uint64_t root_blk;
    if (lcfs_alloc_block(fd, &root_blk) < 0) return -1;
    lcfs_oid_t root_oid;
    if (lcfs_create_object(fd, OBJ_TYPE_DIR, 0, "/", &root_oid, &root_blk) < 0) return -1;
    // Ajustar OID del root a 3
    lcfs_obj_header hdr;
    lcfs_read_header(fd, root_blk, &hdr);
    hdr.oid = LCFS_ROOT_OID;
    hdr.header_crc = 0;
    hdr.header_crc = header_crc(&hdr);
    lcfs_write_header(fd, root_blk, &hdr);

    // Actualizar superblock con root oid y next_oid
    lcfs_read_header(fd, 0, &sb);
    sb.size = total_blocks;
    sb.first_child_oid = LCFS_ROOT_OID;
    sb.generation = 4; // next_oid
    sb.header_crc = 0;
    sb.header_crc = header_crc(&sb);
    lcfs_write_block(fd, 0, &sb);

    return 0;
}

int lcfs_get_free_map(int fd, uint8_t **bitmap, uint64_t *bitmap_blocks) {
    lcfs_obj_header hdr;
    if (lcfs_read_header(fd, LCFS_FREE_MAP_OID, &hdr) < 0) return -1;
    if (hdr.type != OBJ_TYPE_FREE_MAP) return -1;
    uint64_t bm_blocks = hdr.num_extents;
    uint8_t *bm = malloc(bm_blocks * LCFS_BLOCK_SIZE);
    if (!bm) return -1;
    for (uint64_t i = 0; i < bm_blocks; i++) {
        if (lcfs_read_block(fd, 1 + i, bm + i*LCFS_BLOCK_SIZE) < 0) {
            free(bm);
            return -1;
        }
    }
    *bitmap = bm;
    *bitmap_blocks = bm_blocks;
    return 0;
}

int lcfs_set_free_map(int fd, const uint8_t *bitmap, uint64_t bitmap_blocks) {
    lcfs_obj_header hdr;
    if (lcfs_read_header(fd, LCFS_FREE_MAP_OID, &hdr) < 0) return -1;
    hdr.size = bitmap_blocks * LCFS_BLOCK_SIZE;
    hdr.num_extents = bitmap_blocks;
    hdr.header_crc = 0;
    hdr.header_crc = header_crc(&hdr);
    if (lcfs_write_header(fd, LCFS_FREE_MAP_OID, &hdr) < 0) return -1;
    for (uint64_t i = 0; i < bitmap_blocks; i++) {
        if (lcfs_write_block(fd, 1 + i, bitmap + i*LCFS_BLOCK_SIZE) < 0) return -1;
    }
    return 0;
}

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

int lcfs_object_location(int fd, lcfs_oid_t oid, uint64_t *block_num) {
    // Buscar en el OID map (objeto con OID=2)
    lcfs_obj_header hdr;
    if (lcfs_read_header(fd, LCFS_OID_MAP_OID, &hdr) == 0 && hdr.type == OBJ_TYPE_OID_MAP) {
        // El OID map almacena pares (oid, block) en su espacio de datos
        uint8_t block[LCFS_BLOCK_SIZE];
        if (lcfs_read_block(fd, LCFS_OID_MAP_OID, block) == 0) {
            // Recorrer entradas (simplificado: asumimos array de pares)
            uint64_t count = hdr.size / sizeof(uint64_t) / 2;
            uint64_t *entries = (uint64_t*)(block + LCFS_HEADER_SIZE);
            for (uint64_t i = 0; i < count; i++) {
                if (entries[2*i] == oid) {
                    *block_num = entries[2*i+1];
                    return 0;
                }
            }
        }
    }
    // Fallback: escaneo completo
    // Obtener tamaño del dispositivo
    off_t dev_size = lseek(fd, 0, SEEK_END);
    uint64_t total_blocks = dev_size / LCFS_BLOCK_SIZE;
    for (uint64_t b = 0; b < total_blocks; b++) {
        lcfs_obj_header obj_hdr;
        if (lcfs_read_header(fd, b, &obj_hdr) == 0 && obj_hdr.oid == oid) {
            *block_num = b;
            return 0;
        }
    }
    errno = ENOENT;
    return -1;
}

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
    // OID: generarlo a partir del superbloque
    lcfs_obj_header sb;
    if (lcfs_read_header(fd, LCFS_SUPERBLOCK_OID, &sb) == 0) {
        hdr.oid = sb.generation++;
        sb.header_crc = 0;
        sb.header_crc = header_crc(&sb);
        lcfs_write_block(fd, LCFS_SUPERBLOCK_OID, &sb);
    } else {
        // Si no hay superbloque, usar timestamp+aleatorio simple
        hdr.oid = (uint64_t)time(NULL) << 32 | (uint64_t)rand();
    }
    // Nombre
    uint8_t block[LCFS_BLOCK_SIZE] = {0};
    size_t name_len = strlen(name);
    if (name_len > LCFS_MAX_NAME_LEN) name_len = LCFS_MAX_NAME_LEN;
    memcpy(block + LCFS_HEADER_SIZE, &name_len, 2);
    memcpy(block + LCFS_HEADER_SIZE + 2, name, name_len);
    hdr.header_crc = 0;
    hdr.header_crc = header_crc(&hdr);
    memcpy(block, &hdr, sizeof(hdr));
    if (lcfs_write_block(fd, blk, block) < 0) {
        lcfs_free_block(fd, blk);
        return -1;
    }
    // Actualizar OID map
    // (simplificado: no implementado aquí, se hará en lcfs_rebuild_oid_map)
    if (new_oid) *new_oid = hdr.oid;
    if (block_num) *block_num = blk;
    return 0;
                       }

                       int lcfs_delete_object(int fd, lcfs_oid_t oid) {
                           uint64_t block;
                           if (lcfs_object_location(fd, oid, &block) < 0) return -1;
                           lcfs_obj_header hdr;
                           if (lcfs_read_header(fd, block, &hdr) < 0) return -1;
                           // Liberar extents si es archivo o symlink con datos externos
                           if (hdr.type == OBJ_TYPE_FILE || hdr.type == OBJ_TYPE_SYMLINK) {
                               if (hdr.num_extents > 0) {
                                   // Leer extents almacenados en el bloque o en tabla aparte
                                   // Por simplicidad asumimos que los extents están en el mismo bloque
                                   if (hdr.num_extents * sizeof(lcfs_extent) <= LCFS_BLOCK_SIZE - LCFS_HEADER_SIZE - 2 - LCFS_MAX_NAME_LEN) {
                                       lcfs_extent *extents = (lcfs_extent*)((uint8_t*)&hdr + LCFS_HEADER_SIZE + 2 + strlen((char*)((uint8_t*)&hdr + LCFS_HEADER_SIZE + 2)));
                                       for (uint32_t i = 0; i < hdr.num_extents; i++) {
                                           lcfs_free_block(fd, extents[i].physical_block);
                                       }
                                   } else {
                                       // Tabla de extents separada (no implementado en esta versión simple)
                                   }
                               }
                           }
                           lcfs_free_block(fd, block);
                           // TODO: eliminar entrada del directorio padre
                           return 0;
                       }

                       int lcfs_lookup_name(int fd, lcfs_oid_t dir_oid, const char *name,
                                            lcfs_oid_t *child_oid, uint16_t *child_type) {
                           uint64_t dir_block;
                           if (lcfs_object_location(fd, dir_oid, &dir_block) < 0) return -1;
                           lcfs_obj_header hdr;
                           if (lcfs_read_header(fd, dir_block, &hdr) < 0) return -1;
                           if (hdr.type != OBJ_TYPE_DIR) return -1;
                           uint8_t block[LCFS_BLOCK_SIZE];
                           if (lcfs_read_block(fd, dir_block, block) < 0) return -1;
                           uint16_t dname_len;
                           memcpy(&dname_len, block + LCFS_HEADER_SIZE, 2);
                           size_t pos = LCFS_HEADER_SIZE + 2 + dname_len;
                           while (pos + sizeof(lcfs_dir_entry) <= LCFS_BLOCK_SIZE) {
                               lcfs_dir_entry entry;
                               memcpy(&entry, block + pos, sizeof(entry));
                               if (entry.child_oid == 0) break;
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

                                            int lcfs_add_dir_entry(int fd, lcfs_oid_t dir_oid, lcfs_oid_t child_oid,
                                                                   uint16_t child_type, const char *name) {
                                                uint64_t dir_block;
                                                if (lcfs_object_location(fd, dir_oid, &dir_block) < 0) return -1;
                                                lcfs_obj_header hdr;
                                                if (lcfs_read_header(fd, dir_block, &hdr) < 0) return -1;
                                                if (hdr.type != OBJ_TYPE_DIR) return -1;
                                                uint8_t block[LCFS_BLOCK_SIZE];
                                                if (lcfs_read_block(fd, dir_block, block) < 0) return -1;
                                                uint16_t dname_len;
                                                memcpy(&dname_len, block + LCFS_HEADER_SIZE, 2);
                                                size_t pos = LCFS_HEADER_SIZE + 2 + dname_len;
                                                while (pos + sizeof(lcfs_dir_entry) <= LCFS_BLOCK_SIZE) {
                                                    lcfs_dir_entry entry;
                                                    memcpy(&entry, block + pos, sizeof(entry));
                                                    if (entry.child_oid == 0) {
                                                        entry.child_oid = child_oid;
                                                        entry.child_type = child_type;
                                                        entry.name_len = strlen(name);
                                                        if (entry.name_len > LCFS_MAX_NAME_LEN) entry.name_len = LCFS_MAX_NAME_LEN;
                                                        memcpy(block + pos, &entry, sizeof(entry));
                                                        memcpy(block + pos + sizeof(entry), name, entry.name_len);
                                                        hdr.size++;
                                                        hdr.header_crc = 0;
                                                        hdr.header_crc = header_crc(&hdr);
                                                        memcpy(block, &hdr, sizeof(hdr));
                                                        if (lcfs_write_block(fd, dir_block, block) < 0) return -1;
                                                        return 0;
                                                    }
                                                    pos += sizeof(entry) + entry.name_len;
                                                }
                                                errno = ENOSPC;
                                                return -1;
                                                                   }

                                                                   int lcfs_read_file(int fd, lcfs_oid_t oid, char *buf, size_t size, off_t offset) {
                                                                       uint64_t block;
                                                                       if (lcfs_object_location(fd, oid, &block) < 0) return -1;
                                                                       lcfs_obj_header hdr;
                                                                       if (lcfs_read_header(fd, block, &hdr) < 0) return -1;
                                                                       if (hdr.type != OBJ_TYPE_FILE && hdr.type != OBJ_TYPE_SYMLINK) return -1;
                                                                       // Archivo inline
                                                                       if (hdr.num_extents == 0) {
                                                                           uint8_t obj_block[LCFS_BLOCK_SIZE];
                                                                           if (lcfs_read_block(fd, block, obj_block) < 0) return -1;
                                                                           uint16_t name_len;
                                                                           memcpy(&name_len, obj_block + LCFS_HEADER_SIZE, 2);
                                                                           size_t data_start = LCFS_HEADER_SIZE + 2 + name_len;
                                                                           if (offset >= hdr.size) return 0;
                                                                           size_t read_size = size;
                                                                           if (offset + read_size > hdr.size) read_size = hdr.size - offset;
                                                                           memcpy(buf, obj_block + data_start + offset, read_size);
                                                                           return read_size;
                                                                       }
                                                                       // Archivo con extents (simplificado: asumimos extents en el mismo bloque)
                                                                       uint8_t obj_block[LCFS_BLOCK_SIZE];
                                                                       if (lcfs_read_block(fd, block, obj_block) < 0) return -1;
                                                                       uint16_t name_len;
                                                                       memcpy(&name_len, obj_block + LCFS_HEADER_SIZE, 2);
                                                                       size_t data_start = LCFS_HEADER_SIZE + 2 + name_len;
                                                                       lcfs_extent *extents = (lcfs_extent*)(obj_block + data_start);
                                                                       size_t total_read = 0;
                                                                       off_t current_offset = 0;
                                                                       for (uint32_t i = 0; i < hdr.num_extents; i++) {
                                                                           uint64_t logical_start = extents[i].logical_block * LCFS_BLOCK_SIZE;
                                                                           uint64_t logical_end = logical_start + LCFS_BLOCK_SIZE;
                                                                           if (offset >= logical_end || offset + size <= logical_start) continue;
                                                                           uint64_t phys_block = extents[i].physical_block;
                                                                           uint8_t data[LCFS_BLOCK_SIZE];
                                                                           if (lcfs_read_block(fd, phys_block, data) < 0) return -1;
                                                                           off_t off_in_extent = offset - logical_start;
                                                                           size_t len = LCFS_BLOCK_SIZE - off_in_extent;
                                                                           if (offset + len > hdr.size) len = hdr.size - offset;
                                                                           if (len > size - total_read) len = size - total_read;
                                                                           memcpy(buf + total_read, data + off_in_extent, len);
                                                                           total_read += len;
                                                                           offset += len;
                                                                           if (total_read >= size) break;
                                                                       }
                                                                       return total_read;
                                                                   }

                                                                   int lcfs_readlink(int fd, lcfs_oid_t oid, char *buf, size_t bufsize) {
                                                                       uint64_t block;
                                                                       if (lcfs_object_location(fd, oid, &block) < 0) return -1;
                                                                       lcfs_obj_header hdr;
                                                                       if (lcfs_read_header(fd, block, &hdr) < 0) return -1;
                                                                       if (hdr.type != OBJ_TYPE_SYMLINK) return -1;
                                                                       // Los symlinks son como archivos inline
                                                                       if (hdr.num_extents == 0) {
                                                                           uint8_t obj_block[LCFS_BLOCK_SIZE];
                                                                           if (lcfs_read_block(fd, block, obj_block) < 0) return -1;
                                                                           uint16_t name_len;
                                                                           memcpy(&name_len, obj_block + LCFS_HEADER_SIZE, 2);
                                                                           size_t data_start = LCFS_HEADER_SIZE + 2 + name_len;
                                                                           size_t link_len = hdr.size;
                                                                           if (link_len >= bufsize) link_len = bufsize - 1;
                                                                           memcpy(buf, obj_block + data_start, link_len);
                                                                           buf[link_len] = '\0';
                                                                           return 0;
                                                                       } else {
                                                                           // Symlink con extents (raro, pero posible si es muy largo)
                                                                           char *tmp = malloc(hdr.size + 1);
                                                                           if (!tmp) return -1;
                                                                           lcfs_read_file(fd, oid, tmp, hdr.size, 0);
                                                                           tmp[hdr.size] = '\0';
                                                                           strncpy(buf, tmp, bufsize);
                                                                           free(tmp);
                                                                           return 0;
                                                                       }
                                                                   }

                                                                   int lcfs_rebuild_free_map(int fd, uint8_t **bitmap, uint64_t *bitmap_blocks) {
                                                                       off_t dev_size = lseek(fd, 0, SEEK_END);
                                                                       uint64_t total_blocks = dev_size / LCFS_BLOCK_SIZE;
                                                                       uint64_t bm_blocks = (total_blocks + LCFS_BLOCK_SIZE*8 - 1) / (LCFS_BLOCK_SIZE*8);
                                                                       uint8_t *bm = calloc(bm_blocks, LCFS_BLOCK_SIZE);
                                                                       if (!bm) return -1;
                                                                       // Marcar todos los bloques que contienen objetos válidos
                                                                       for (uint64_t b = 0; b < total_blocks; b++) {
                                                                           uint8_t block[LCFS_BLOCK_SIZE];
                                                                           if (lcfs_read_block(fd, b, block) < 0) continue;
                                                                           lcfs_obj_header hdr;
                                                                           memcpy(&hdr, block, sizeof(hdr));
                                                                           if (memcmp(hdr.magic, LCFS_MAGIC, LCFS_MAGIC_LEN) == 0 &&
                                                                               hdr.header_crc == header_crc(&hdr)) {
                                                                               // Es un objeto válido, marcar su bloque
                                                                               uint64_t byte_idx = b / 8;
                                                                           uint8_t bit = 1 << (b % 8);
                                                                           bm[byte_idx] |= bit;
                                                                           // Si es archivo con extents, marcar los bloques de datos
                                                                           if (hdr.type == OBJ_TYPE_FILE || hdr.type == OBJ_TYPE_SYMLINK) {
                                                                               if (hdr.num_extents > 0) {
                                                                                   // Asumimos extents en el mismo bloque por simplicidad
                                                                                   uint16_t name_len;
                                                                                   memcpy(&name_len, block + LCFS_HEADER_SIZE, 2);
                                                                                   size_t data_start = LCFS_HEADER_SIZE + 2 + name_len;
                                                                                   lcfs_extent *extents = (lcfs_extent*)(block + data_start);
                                                                                   for (uint32_t i = 0; i < hdr.num_extents; i++) {
                                                                                       uint64_t phys = extents[i].physical_block;
                                                                                       byte_idx = phys / 8;
                                                                                       bit = 1 << (phys % 8);
                                                                                       bm[byte_idx] |= bit;
                                                                                   }
                                                                               }
                                                                           }
                                                                               }
                                                                       }
                                                                       *bitmap = bm;
                                                                       *bitmap_blocks = bm_blocks;
                                                                       return 0;
                                                                   }

                                                                   int lcfs_rebuild_oid_map(int fd) {
                                                                       // Escanear todos los bloques y construir OID map
                                                                       off_t dev_size = lseek(fd, 0, SEEK_END);
                                                                       uint64_t total_blocks = dev_size / LCFS_BLOCK_SIZE;
                                                                       // Crear lista temporal de pares (oid, block)
                                                                       typedef struct { uint64_t oid, block; } oid_entry;
                                                                       oid_entry *entries = malloc(sizeof(oid_entry) * total_blocks);
                                                                       if (!entries) return -1;
                                                                       uint64_t count = 0;
                                                                       for (uint64_t b = 0; b < total_blocks; b++) {
                                                                           lcfs_obj_header hdr;
                                                                           if (lcfs_read_header(fd, b, &hdr) == 0) {
                                                                               entries[count].oid = hdr.oid;
                                                                               entries[count].block = b;
                                                                               count++;
                                                                           }
                                                                       }
                                                                       // Escribir en objeto OID_MAP (bloque 2)
                                                                       uint8_t block[LCFS_BLOCK_SIZE] = {0};
                                                                       lcfs_obj_header om;
                                                                       memcpy(om.magic, LCFS_MAGIC, LCFS_MAGIC_LEN);
                                                                       om.type = OBJ_TYPE_OID_MAP;
                                                                       om.version = LCFS_VERSION;
                                                                       om.oid = LCFS_OID_MAP_OID;
                                                                       om.size = count * 16;
                                                                       om.num_extents = 0;
                                                                       om.header_crc = 0;
                                                                       om.header_crc = header_crc(&om);
                                                                       memcpy(block, &om, sizeof(om));
                                                                       uint64_t *ptr = (uint64_t*)(block + LCFS_HEADER_SIZE);
                                                                       for (uint64_t i = 0; i < count; i++) {
                                                                           ptr[2*i] = entries[i].oid;
                                                                           ptr[2*i+1] = entries[i].block;
                                                                       }
                                                                       lcfs_write_block(fd, LCFS_OID_MAP_OID, block);
                                                                       free(entries);
                                                                       return 0;
                                                                   }
