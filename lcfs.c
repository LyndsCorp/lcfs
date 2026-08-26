#define _POSIX_C_SOURCE 200809L

#include "lcfs.h"
#include "debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <assert.h>
#include <inttypes.h>

/* CRC32C (Castagnoli) */
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

int lcfs_validate_header(const lcfs_obj_header *hdr) {
    if (memcmp(hdr->magic, LCFS_MAGIC, LCFS_MAGIC_LEN) != 0) return -1;
    lcfs_obj_header tmp = *hdr;
    tmp.header_crc = 0;
    uint32_t crc = lcfs_crc32c(0, &tmp, sizeof(tmp));
    return (hdr->header_crc == crc) ? 0 : -1;
}

int lcfs_read_block(int fd, uint64_t block_num, void *buf) {
    DEBUG_ENTER();
    off_t off = (off_t)block_num * LCFS_BLOCK_SIZE;
    if (lseek(fd, off, SEEK_SET) < 0) {
        DEBUG_ERROR("lseek falló para bloque %" PRIu64, block_num);
        return -1;
    }
    if (read(fd, buf, LCFS_BLOCK_SIZE) != LCFS_BLOCK_SIZE) {
        DEBUG_ERROR("read falló para bloque %" PRIu64, block_num);
        return -1;
    }
    DEBUG_EXIT(0);
    return 0;
}

int lcfs_write_block(int fd, uint64_t block_num, const void *buf) {
    DEBUG_ENTER();
    uint8_t block[LCFS_BLOCK_SIZE] = {0};
    memcpy(block, buf, LCFS_BLOCK_SIZE);
    off_t off = (off_t)block_num * LCFS_BLOCK_SIZE;
    if (lseek(fd, off, SEEK_SET) < 0) {
        DEBUG_ERROR("lseek falló para bloque %" PRIu64, block_num);
        return -1;
    }
    if (write(fd, block, LCFS_BLOCK_SIZE) != LCFS_BLOCK_SIZE) {
        DEBUG_ERROR("write falló para bloque %" PRIu64, block_num);
        return -1;
    }
    DEBUG_EXIT(0);
    return 0;
}

int lcfs_read_header(int fd, uint64_t block_num, lcfs_obj_header *hdr) {
    DEBUG_ENTER();
    uint8_t block[LCFS_BLOCK_SIZE];
    if (lcfs_read_block(fd, block_num, block) < 0) return -1;
    memcpy(hdr, block, sizeof(*hdr));
    if (memcmp(hdr->magic, LCFS_MAGIC, LCFS_MAGIC_LEN) != 0) {
        DEBUG_ERROR("Firma mágica incorrecta en bloque %" PRIu64, block_num);
        errno = EINVAL;
        return -1;
    }
    if (hdr->header_crc != header_crc(hdr)) {
        DEBUG_ERROR("CRC incorrecto en bloque %" PRIu64, block_num);
        errno = EBADMSG;
        return -1;
    }
    DEBUG_EXIT(0);
    return 0;
}

int lcfs_write_header(int fd, uint64_t block_num, const lcfs_obj_header *hdr) {
    DEBUG_ENTER();
    lcfs_obj_header tmp = *hdr;
    tmp.header_crc = 0;
    tmp.header_crc = header_crc(&tmp);
    uint8_t block[LCFS_BLOCK_SIZE] = {0};
    memcpy(block, &tmp, sizeof(tmp));
    if (lcfs_write_block(fd, block_num, block) < 0) return -1;
    DEBUG_EXIT(0);
    return 0;
}

int lcfs_read_object_name(int fd, uint64_t block_num, char *name, size_t max_len) {
    DEBUG_ENTER();
    uint8_t block[LCFS_BLOCK_SIZE];
    if (lcfs_read_block(fd, block_num, block) < 0) return -1;
    uint16_t name_len;
    memcpy(&name_len, block + LCFS_HEADER_SIZE, 2);
    if (name_len >= max_len) name_len = max_len - 1;
    memcpy(name, block + LCFS_HEADER_SIZE + 2, name_len);
    name[name_len] = '\0';
    DEBUG_EXIT(0);
    return 0;
}

int lcfs_init_superblock(int fd, uint64_t total_blocks) {
    DEBUG_ENTER();
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
    sb.generation = 4;
    sb.header_crc = 0;
    sb.header_crc = header_crc(&sb);
    if (lcfs_write_header(fd, 0, &sb) < 0) return -1;

    uint64_t bm_blocks = (total_blocks + LCFS_BLOCK_SIZE*8 - 1) / (LCFS_BLOCK_SIZE*8);
    uint8_t *bitmap = calloc(bm_blocks, LCFS_BLOCK_SIZE);
    if (!bitmap) return -1;
    bitmap[0] |= 0x03; // bloques 0 y 1 ocupados

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
    if (lcfs_write_header(fd, 1, &fm) < 0) { free(bitmap); return -1; }

    for (uint64_t i = 0; i < bm_blocks; i++) {
        uint64_t byte_idx = (2 + i) / 8;
        uint8_t bit = 1 << ((2 + i) % 8);
        bitmap[byte_idx] |= bit;
        if (lcfs_write_block(fd, 2 + i, bitmap + i*LCFS_BLOCK_SIZE) < 0) {
            free(bitmap); return -1;
        }
    }

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
    if (lcfs_write_header(fd, oid_map_block, &om) < 0) { free(bitmap); return -1; }

    uint64_t byte_idx = oid_map_block / 8;
    uint8_t bit = 1 << (oid_map_block % 8);
    bitmap[byte_idx] |= bit;

    if (lcfs_set_free_map(fd, bitmap, bm_blocks) < 0) { free(bitmap); return -1; }
    free(bitmap);

    uint64_t root_blk;
    lcfs_oid_t root_oid;
    if (lcfs_create_object(fd, OBJ_TYPE_DIR, 0, "/", &root_oid, &root_blk) < 0) return -1;

    lcfs_obj_header hdr;
    if (lcfs_read_header(fd, root_blk, &hdr) < 0) return -1;
    hdr.oid = LCFS_ROOT_OID;
    hdr.header_crc = 0;
    hdr.header_crc = header_crc(&hdr);
    if (lcfs_write_header(fd, root_blk, &hdr) < 0) return -1;

    if (lcfs_read_header(fd, 0, &sb) < 0) return -1;
    sb.first_child_oid = LCFS_ROOT_OID;
    sb.generation = 4;
    sb.header_crc = 0;
    sb.header_crc = header_crc(&sb);
    if (lcfs_write_header(fd, 0, &sb) < 0) return -1;

    DEBUG_EXIT(0);
    return 0;
}

int lcfs_object_location(int fd, lcfs_oid_t oid, uint64_t *block_num) {
    DEBUG_ENTER();
    off_t dev_size = lseek(fd, 0, SEEK_END);
    uint64_t total_blocks = dev_size / LCFS_BLOCK_SIZE;
    for (uint64_t b = 0; b < total_blocks; b++) {
        lcfs_obj_header obj_hdr;
        if (lcfs_read_header(fd, b, &obj_hdr) == 0 && obj_hdr.oid == oid) {
            *block_num = b;
            DEBUG_PRINT("Encontrado en bloque %" PRIu64, b);
            DEBUG_EXIT(0);
            return 0;
        }
    }
    DEBUG_ERROR("OID %" PRIu64 " no encontrado en el disco", oid);
    errno = ENOENT;
    return -1;
}

int lcfs_get_free_map(int fd, uint8_t **bitmap, uint64_t *bitmap_blocks) {
    DEBUG_ENTER();
    uint64_t map_block;
    if (lcfs_object_location(fd, LCFS_FREE_MAP_OID, &map_block) < 0) return -1;
    lcfs_obj_header hdr;
    if (lcfs_read_header(fd, map_block, &hdr) < 0) return -1;
    if (hdr.type != OBJ_TYPE_FREE_MAP) return -1;
    uint64_t bm_blocks = hdr.num_extents;
    uint8_t *bm = malloc(bm_blocks * LCFS_BLOCK_SIZE);
    if (!bm) return -1;
    for (uint64_t i = 0; i < bm_blocks; i++) {
        if (lcfs_read_block(fd, map_block + 1 + i, bm + i*LCFS_BLOCK_SIZE) < 0) {
            free(bm); return -1;
        }
    }
    *bitmap = bm;
    *bitmap_blocks = bm_blocks;
    DEBUG_EXIT(0);
    return 0;
}

int lcfs_set_free_map(int fd, const uint8_t *bitmap, uint64_t bitmap_blocks) {
    DEBUG_ENTER();
    uint64_t map_block;
    if (lcfs_object_location(fd, LCFS_FREE_MAP_OID, &map_block) < 0) return -1;
    lcfs_obj_header hdr;
    if (lcfs_read_header(fd, map_block, &hdr) < 0) return -1;
    hdr.size = bitmap_blocks * LCFS_BLOCK_SIZE;
    hdr.num_extents = bitmap_blocks;
    hdr.header_crc = 0;
    hdr.header_crc = header_crc(&hdr);
    if (lcfs_write_header(fd, map_block, &hdr) < 0) return -1;
    for (uint64_t i = 0; i < bitmap_blocks; i++) {
        if (lcfs_write_block(fd, map_block + 1 + i, bitmap + i*LCFS_BLOCK_SIZE) < 0) return -1;
    }
    DEBUG_EXIT(0);
    return 0;
}

int lcfs_alloc_block(int fd, uint64_t *block_num) {
    DEBUG_ENTER();
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
                free(bitmap); return -1;
            }
            free(bitmap);
            *block_num = i;
            DEBUG_PRINT("Bloque %" PRIu64 " asignado", i);
            DEBUG_EXIT(0);
            return 0;
        }
    }
    free(bitmap);
    DEBUG_ERROR("No hay bloques libres");
    errno = ENOSPC;
    return -1;
}

int lcfs_free_block(int fd, uint64_t block_num) {
    DEBUG_ENTER();
    uint8_t *bitmap;
    uint64_t bm_blocks;
    if (lcfs_get_free_map(fd, &bitmap, &bm_blocks) < 0) return -1;
    uint64_t byte_idx = block_num / 8;
    uint8_t bit = 1 << (block_num % 8);
    bitmap[byte_idx] &= ~bit;
    if (lcfs_set_free_map(fd, bitmap, bm_blocks) < 0) {
        free(bitmap); return -1;
    }
    free(bitmap);
    DEBUG_EXIT(0);
    return 0;
}

int lcfs_oid_map_add(int fd, lcfs_oid_t oid, uint64_t block) {
    DEBUG_ENTER();
    uint64_t map_block;
    if (lcfs_object_location(fd, LCFS_OID_MAP_OID, &map_block) < 0) return -1;
    uint8_t map_data[LCFS_BLOCK_SIZE];
    if (lcfs_read_block(fd, map_block, map_data) < 0) return -1;
    lcfs_obj_header hdr;
    memcpy(&hdr, map_data, sizeof(hdr));
    if (hdr.type != OBJ_TYPE_OID_MAP) return -1;
    uint64_t count = hdr.size / 16;
    if (count >= (LCFS_BLOCK_SIZE - LCFS_HEADER_SIZE) / 16) {
        DEBUG_ERROR("OID map lleno");
        errno = ENOSPC;
        return -1;
    }
    uint64_t *entries = (uint64_t*)(map_data + LCFS_HEADER_SIZE);
    entries[2*count] = oid;
    entries[2*count+1] = block;
    hdr.size += 16;
    hdr.header_crc = 0;
    hdr.header_crc = header_crc(&hdr);
    memcpy(map_data, &hdr, sizeof(hdr));
    if (lcfs_write_block(fd, map_block, map_data) < 0) return -1;
    DEBUG_EXIT(0);
    return 0;
}

int lcfs_oid_map_remove(int fd, lcfs_oid_t oid) {
    DEBUG_ENTER();
    uint64_t map_block;
    if (lcfs_object_location(fd, LCFS_OID_MAP_OID, &map_block) < 0) return -1;
    uint8_t map_data[LCFS_BLOCK_SIZE];
    if (lcfs_read_block(fd, map_block, map_data) < 0) return -1;
    lcfs_obj_header hdr;
    memcpy(&hdr, map_data, sizeof(hdr));
    if (hdr.type != OBJ_TYPE_OID_MAP) return -1;
    uint64_t count = hdr.size / 16;
    uint64_t *entries = (uint64_t*)(map_data + LCFS_HEADER_SIZE);
    for (uint64_t i = 0; i < count; i++) {
        if (entries[2*i] == oid) {
            if (i != count-1) {
                entries[2*i] = entries[2*(count-1)];
                entries[2*i+1] = entries[2*(count-1)+1];
            }
            hdr.size -= 16;
            hdr.header_crc = 0;
            hdr.header_crc = header_crc(&hdr);
            memcpy(map_data, &hdr, sizeof(hdr));
            if (lcfs_write_block(fd, map_block, map_data) < 0) return -1;
            DEBUG_EXIT(0);
            return 0;
        }
    }
    DEBUG_ERROR("OID %" PRIu64 " no encontrado en OID map", oid);
    errno = ENOENT;
    return -1;
}

int lcfs_create_object(int fd, uint16_t type, lcfs_oid_t parent_oid,
                       const char *name, lcfs_oid_t *new_oid, uint64_t *block_num) {
    DEBUG_ENTER();
    uint64_t blk;
    if (lcfs_alloc_block(fd, &blk) < 0) return -1;
    lcfs_obj_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, LCFS_MAGIC, LCFS_MAGIC_LEN);
    hdr.type = type;
    hdr.version = LCFS_VERSION;
    hdr.parent_oid = parent_oid;
    lcfs_obj_header sb;
    if (lcfs_read_header(fd, LCFS_SUPERBLOCK_OID, &sb) == 0) {
        hdr.oid = sb.generation++;
        sb.header_crc = 0;
        sb.header_crc = header_crc(&sb);
        if (lcfs_write_header(fd, LCFS_SUPERBLOCK_OID, &sb) < 0) {
            lcfs_free_block(fd, blk); return -1;
        }
    } else {
        hdr.oid = (uint64_t)time(NULL) << 32 | (uint64_t)rand();
    }
    uint8_t block[LCFS_BLOCK_SIZE] = {0};
    size_t name_len = strlen(name);
    if (name_len > LCFS_MAX_NAME_LEN) name_len = LCFS_MAX_NAME_LEN;
    memcpy(block + LCFS_HEADER_SIZE, &name_len, 2);
    memcpy(block + LCFS_HEADER_SIZE + 2, name, name_len);
    hdr.header_crc = 0;
    hdr.header_crc = header_crc(&hdr);
    memcpy(block, &hdr, sizeof(hdr));
    if (lcfs_write_block(fd, blk, block) < 0) {
        lcfs_free_block(fd, blk); return -1;
    }
    if (lcfs_oid_map_add(fd, hdr.oid, blk) < 0) {
        lcfs_free_block(fd, blk); return -1;
    }
    if (new_oid) *new_oid = hdr.oid;
    if (block_num) *block_num = blk;
    DEBUG_PRINT("Objeto creado con OID %" PRIu64 " en bloque %" PRIu64, hdr.oid, blk);
    DEBUG_EXIT(0);
    return 0;
                       }

                       // Función auxiliar para liberar los bloques de datos de un archivo
                       static void free_file_data_blocks(int fd, lcfs_obj_header *hdr, uint64_t obj_block) {
                           uint8_t obj_block_data[LCFS_BLOCK_SIZE];
                           if (lcfs_read_block(fd, obj_block, obj_block_data) < 0) return;
                           uint16_t name_len;
                           memcpy(&name_len, obj_block_data + LCFS_HEADER_SIZE, 2);
                           size_t data_start = LCFS_HEADER_SIZE + 2 + name_len;
                           if (hdr->num_extents > 0) {
                               lcfs_extent *extents = (lcfs_extent*)(obj_block_data + data_start);
                               uint64_t phys_start = extents[0].physical_block;
                               uint64_t num_blocks = (hdr->size + LCFS_BLOCK_SIZE - 1) / LCFS_BLOCK_SIZE;
                               for (uint64_t i = 0; i < num_blocks; i++) {
                                   lcfs_free_block(fd, phys_start + i);
                               }
                           }
                       }

                       int lcfs_delete_object(int fd, lcfs_oid_t oid) {
                           DEBUG_ENTER();
                           uint64_t block;
                           if (lcfs_object_location(fd, oid, &block) < 0) return -1;
                           lcfs_obj_header hdr;
                           if (lcfs_read_header(fd, block, &hdr) < 0) return -1;

                           if ((hdr.type == OBJ_TYPE_FILE || hdr.type == OBJ_TYPE_SYMLINK) && hdr.num_extents > 0) {
                               free_file_data_blocks(fd, &hdr, block);
                           }

                           lcfs_free_block(fd, block);
                           lcfs_oid_map_remove(fd, oid);
                           DEBUG_EXIT(0);
                           return 0;
                       }

                       int lcfs_lookup_name(int fd, lcfs_oid_t dir_oid, const char *name,
                                            lcfs_oid_t *child_oid, uint16_t *child_type) {
                           DEBUG_ENTER();
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
                                   DEBUG_PRINT("Encontrado: OID %" PRIu64 ", tipo %u", entry.child_oid, entry.child_type);
                                   DEBUG_EXIT(0);
                                   return 0;
                               }
                           }
                           DEBUG_ERROR("Nombre '%s' no encontrado en directorio OID %" PRIu64, name, dir_oid);
                           errno = ENOENT;
                           return -1;
                                            }

                                            int lcfs_add_dir_entry(int fd, lcfs_oid_t dir_oid, lcfs_oid_t child_oid,
                                                                   uint16_t child_type, const char *name) {
                                                DEBUG_ENTER();
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
                                                        DEBUG_EXIT(0);
                                                        return 0;
                                                    }
                                                    pos += sizeof(entry) + entry.name_len;
                                                }
                                                DEBUG_ERROR("No hay espacio en directorio %" PRIu64 " para nueva entrada", dir_oid);
                                                errno = ENOSPC;
                                                return -1;
                                                                   }

                                                                   int lcfs_remove_dir_entry(int fd, lcfs_oid_t dir_oid, const char *name) {
                                                                       DEBUG_ENTER();
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
                                                                       int found = 0;
                                                                       while (pos + sizeof(lcfs_dir_entry) <= LCFS_BLOCK_SIZE) {
                                                                           lcfs_dir_entry entry;
                                                                           memcpy(&entry, block + pos, sizeof(entry));
                                                                           if (entry.child_oid == 0) break;
                                                                           size_t entry_size = sizeof(entry) + entry.name_len;
                                                                           char entry_name[LCFS_MAX_NAME_LEN+1];
                                                                           memcpy(entry_name, block + pos + sizeof(entry), entry.name_len);
                                                                           entry_name[entry.name_len] = '\0';
                                                                           if (strcmp(entry_name, name) == 0) {
                                                                               size_t next_pos = pos + entry_size;
                                                                               size_t remaining = LCFS_BLOCK_SIZE - next_pos;
                                                                               memmove(block + pos, block + next_pos, remaining);
                                                                               memset(block + pos + remaining, 0, entry_size);
                                                                               hdr.size--;
                                                                               hdr.header_crc = 0;
                                                                               hdr.header_crc = header_crc(&hdr);
                                                                               memcpy(block, &hdr, sizeof(hdr));
                                                                               if (lcfs_write_block(fd, dir_block, block) < 0) return -1;
                                                                               found = 1;
                                                                               break;
                                                                           }
                                                                           pos += entry_size;
                                                                       }
                                                                       if (!found) {
                                                                           DEBUG_ERROR("Entrada '%s' no encontrada para eliminar", name);
                                                                           errno = ENOENT;
                                                                           return -1;
                                                                       }
                                                                       DEBUG_EXIT(0);
                                                                       return 0;
                                                                   }

                                                                   int lcfs_read_file(int fd, lcfs_oid_t oid, char *buf, size_t size, off_t offset) {
                                                                       DEBUG_ENTER();
                                                                       uint64_t block;
                                                                       if (lcfs_object_location(fd, oid, &block) < 0) return -1;
                                                                       lcfs_obj_header hdr;
                                                                       if (lcfs_read_header(fd, block, &hdr) < 0) return -1;
                                                                       if (hdr.type != OBJ_TYPE_FILE && hdr.type != OBJ_TYPE_SYMLINK) return -1;

                                                                       if (offset >= (off_t)hdr.size) return 0;
                                                                       if (size > (size_t)(hdr.size - offset)) size = hdr.size - offset;

                                                                       size_t total_read = 0;
                                                                       off_t cur_off = offset;
                                                                       uint8_t obj_block[LCFS_BLOCK_SIZE];
                                                                       if (lcfs_read_block(fd, block, obj_block) < 0) return -1;

                                                                       uint16_t name_len;
                                                                       memcpy(&name_len, obj_block + LCFS_HEADER_SIZE, 2);
                                                                       size_t data_start = LCFS_HEADER_SIZE + 2 + name_len;

                                                                       if (hdr.num_extents == 0) {
                                                                           // Archivo inline
                                                                           if (data_start + offset + size > LCFS_BLOCK_SIZE)
                                                                               size = LCFS_BLOCK_SIZE - data_start - offset;
                                                                           memcpy(buf, obj_block + data_start + offset, size);
                                                                           total_read = size;
                                                                       } else {
                                                                           // Un único extent contiguo
                                                                           lcfs_extent *extents = (lcfs_extent*)(obj_block + data_start);
                                                                           uint64_t phys_start = extents[0].physical_block;
                                                                           uint64_t num_blocks = (hdr.size + LCFS_BLOCK_SIZE - 1) / LCFS_BLOCK_SIZE;

                                                                           while (total_read < size && cur_off < (off_t)hdr.size) {
                                                                               uint64_t block_idx = cur_off / LCFS_BLOCK_SIZE;
                                                                               uint64_t off_in_block = cur_off % LCFS_BLOCK_SIZE;
                                                                               uint64_t phys_block = phys_start + block_idx;
                                                                               uint8_t data[LCFS_BLOCK_SIZE];
                                                                               if (lcfs_read_block(fd, phys_block, data) < 0) return -1;
                                                                               size_t len = LCFS_BLOCK_SIZE - off_in_block;
                                                                               if (cur_off + len > hdr.size) len = hdr.size - cur_off;
                                                                               if (len > size - total_read) len = size - total_read;
                                                                               memcpy(buf + total_read, data + off_in_block, len);
                                                                               total_read += len;
                                                                               cur_off += len;
                                                                           }
                                                                       }
                                                                       DEBUG_PRINT("Lectura devuelve %zu bytes", total_read);
                                                                       return total_read;
                                                                   }

                                                                   int lcfs_write_file(int fd, lcfs_oid_t oid, const char *buf, size_t size, off_t offset) {
                                                                       DEBUG_ENTER();
                                                                       uint64_t block;
                                                                       if (lcfs_object_location(fd, oid, &block) < 0) return -1;
                                                                       lcfs_obj_header hdr;
                                                                       if (lcfs_read_header(fd, block, &hdr) < 0) return -1;
                                                                       if (hdr.type != OBJ_TYPE_FILE) return -1;

                                                                       uint8_t obj_block[LCFS_BLOCK_SIZE];
                                                                       if (lcfs_read_block(fd, block, obj_block) < 0) return -1;
                                                                       uint16_t name_len;
                                                                       memcpy(&name_len, obj_block + LCFS_HEADER_SIZE, 2);
                                                                       size_t data_start = LCFS_HEADER_SIZE + 2 + name_len;

                                                                       if (hdr.num_extents == 0) {
                                                                           // Archivo inline
                                                                           if ((uint64_t)offset + size <= LCFS_BLOCK_SIZE - data_start) {
                                                                               if (offset + size > hdr.size) hdr.size = offset + size;
                                                                               memcpy(obj_block + data_start + offset, buf, size);
                                                                               hdr.header_crc = 0;
                                                                               hdr.header_crc = header_crc(&hdr);
                                                                               memcpy(obj_block, &hdr, sizeof(hdr));
                                                                               if (lcfs_write_block(fd, block, obj_block) < 0) return -1;
                                                                               return size;
                                                                           } else {
                                                                               // Necesita extent contiguo
                                                                               if (lcfs_truncate_file(fd, oid, offset + size) < 0) return -1;
                                                                               if (lcfs_read_header(fd, block, &hdr) < 0) return -1;
                                                                               if (lcfs_read_block(fd, block, obj_block) < 0) return -1;
                                                                               // Continuar abajo
                                                                           }
                                                                       }

                                                                       // Escritura en extent contiguo
                                                                       lcfs_extent *extents = (lcfs_extent*)(obj_block + data_start);
                                                                       uint64_t phys_start = extents[0].physical_block;
                                                                       uint64_t num_blocks = (hdr.size + LCFS_BLOCK_SIZE - 1) / LCFS_BLOCK_SIZE;

                                                                       off_t cur_off = offset;
                                                                       size_t total_written = 0;
                                                                       while (total_written < size && cur_off < (off_t)hdr.size) {
                                                                           uint64_t block_idx = cur_off / LCFS_BLOCK_SIZE;
                                                                           uint64_t off_in_block = cur_off % LCFS_BLOCK_SIZE;
                                                                           uint64_t phys_block = phys_start + block_idx;
                                                                           uint8_t data[LCFS_BLOCK_SIZE];
                                                                           if (lcfs_read_block(fd, phys_block, data) < 0) return -1;
                                                                           size_t len = LCFS_BLOCK_SIZE - off_in_block;
                                                                           if (cur_off + len > hdr.size) len = hdr.size - cur_off;
                                                                           if (len > size - total_written) len = size - total_written;
                                                                           memcpy(data + off_in_block, buf + total_written, len);
                                                                           if (lcfs_write_block(fd, phys_block, data) < 0) return -1;
                                                                           total_written += len;
                                                                           cur_off += len;
                                                                       }

                                                                       if (offset + size > hdr.size) {
                                                                           hdr.size = offset + size;
                                                                           hdr.header_crc = 0;
                                                                           hdr.header_crc = header_crc(&hdr);
                                                                           memcpy(obj_block, &hdr, sizeof(hdr));
                                                                           if (lcfs_write_block(fd, block, obj_block) < 0) return -1;
                                                                       }
                                                                       DEBUG_PRINT("Escritura devuelve %zu bytes", total_written);
                                                                       return total_written;
                                                                   }

                                                                   int lcfs_truncate_file(int fd, lcfs_oid_t oid, off_t new_size) {
                                                                       DEBUG_ENTER();
                                                                       uint64_t block;
                                                                       if (lcfs_object_location(fd, oid, &block) < 0) return -1;
                                                                       lcfs_obj_header hdr;
                                                                       if (lcfs_read_header(fd, block, &hdr) < 0) return -1;
                                                                       if (hdr.type != OBJ_TYPE_FILE) return -1;

                                                                       if (new_size < 0) new_size = 0;
                                                                       size_t max_inline = LCFS_BLOCK_SIZE - LCFS_HEADER_SIZE - 2 - LCFS_MAX_NAME_LEN;

                                                                       // Si es inline y cabe, solo cambiar tamaño
                                                                       if (hdr.num_extents == 0 && (size_t)new_size <= max_inline) {
                                                                           hdr.size = new_size;
                                                                           hdr.header_crc = 0;
                                                                           hdr.header_crc = header_crc(&hdr);
                                                                           if (lcfs_write_header(fd, block, &hdr) < 0) return -1;
                                                                           return 0;
                                                                       }

                                                                       // Si es inline y necesita crecer a extent
                                                                       if (hdr.num_extents == 0 && (size_t)new_size > max_inline) {
                                                                           uint8_t obj_block[LCFS_BLOCK_SIZE];
                                                                           if (lcfs_read_block(fd, block, obj_block) < 0) return -1;
                                                                           uint16_t name_len;
                                                                           memcpy(&name_len, obj_block + LCFS_HEADER_SIZE, 2);
                                                                           size_t data_start = LCFS_HEADER_SIZE + 2 + name_len;

                                                                           uint64_t num_blocks = (new_size + LCFS_BLOCK_SIZE - 1) / LCFS_BLOCK_SIZE;
                                                                           uint64_t start_block;
                                                                           int found = 0;
                                                                           for (uint64_t b = 0; b < (uint64_t)lseek(fd, 0, SEEK_END)/LCFS_BLOCK_SIZE; b++) {
                                                                               int ok = 1;
                                                                               for (uint64_t i = 0; i < num_blocks; i++) {
                                                                                   uint8_t *bm; uint64_t bm_blocks;
                                                                                   if (lcfs_get_free_map(fd, &bm, &bm_blocks) < 0) return -1;
                                                                                   uint64_t byte_idx = (b+i)/8;
                                                                                   uint8_t bit = 1 << ((b+i)%8);
                                                                                   if (bm[byte_idx] & bit) { ok = 0; free(bm); break; }
                                                                                   free(bm);
                                                                               }
                                                                               if (ok) { start_block = b; found = 1; break; }
                                                                           }
                                                                           if (!found) {
                                                                               DEBUG_ERROR("No se encontró secuencia contigua de %" PRIu64 " bloques", num_blocks);
                                                                               errno = ENOSPC;
                                                                               return -1;
                                                                           }

                                                                           // Marcar bloques ocupados
                                                                           for (uint64_t i = 0; i < num_blocks; i++) {
                                                                               uint8_t *bm; uint64_t bm_blocks;
                                                                               if (lcfs_get_free_map(fd, &bm, &bm_blocks) < 0) return -1;
                                                                               uint64_t byte_idx = (start_block+i)/8;
                                                                               uint8_t bit = 1 << ((start_block+i)%8);
                                                                               bm[byte_idx] |= bit;
                                                                               if (lcfs_set_free_map(fd, bm, bm_blocks) < 0) { free(bm); return -1; }
                                                                               free(bm);
                                                                           }

                                                                           // Copiar datos inline al primer bloque
                                                                           if (hdr.size > 0) {
                                                                               uint8_t inline_data[LCFS_BLOCK_SIZE];
                                                                               memcpy(inline_data, obj_block + data_start, hdr.size);
                                                                               if (lcfs_write_block(fd, start_block, inline_data) < 0) return -1;
                                                                           }

                                                                           // Actualizar header con extent
                                                                           lcfs_extent ext;
                                                                           ext.logical_block = 0;
                                                                           ext.physical_block = start_block;
                                                                           memcpy(obj_block + data_start, &ext, sizeof(ext));
                                                                           hdr.num_extents = 1;
                                                                           hdr.size = new_size;
                                                                           hdr.header_crc = 0;
                                                                           hdr.header_crc = header_crc(&hdr);
                                                                           memcpy(obj_block, &hdr, sizeof(hdr));
                                                                           if (lcfs_write_block(fd, block, obj_block) < 0) return -1;
                                                                           return 0;
                                                                       }

                                                                       // Si ya tiene extent contiguo, solo ajustar tamaño (reducción)
                                                                       if (hdr.num_extents > 0) {
                                                                           uint64_t current_blocks = (hdr.size + LCFS_BLOCK_SIZE - 1) / LCFS_BLOCK_SIZE;
                                                                           uint64_t needed_blocks = (new_size + LCFS_BLOCK_SIZE - 1) / LCFS_BLOCK_SIZE;
                                                                           if (needed_blocks > current_blocks) {
                                                                               DEBUG_ERROR("No se soporta expansión de extent existente");
                                                                               errno = ENOSPC;
                                                                               return -1;
                                                                           }
                                                                           hdr.size = new_size;
                                                                           hdr.header_crc = 0;
                                                                           hdr.header_crc = header_crc(&hdr);
                                                                           if (lcfs_write_header(fd, block, &hdr) < 0) return -1;
                                                                           return 0;
                                                                       }

                                                                       return 0;
                                                                   }

                                                                   int lcfs_get_object_size(int fd, lcfs_oid_t oid, uint32_t *size) {
                                                                       DEBUG_ENTER();
                                                                       uint64_t block;
                                                                       if (lcfs_object_location(fd, oid, &block) < 0) return -1;
                                                                       lcfs_obj_header hdr;
                                                                       if (lcfs_read_header(fd, block, &hdr) < 0) return -1;
                                                                       *size = hdr.size;
                                                                       DEBUG_PRINT("Tamaño del objeto OID %" PRIu64 ": %u", oid, *size);
                                                                       DEBUG_EXIT(0);
                                                                       return 0;
                                                                   }

                                                                   int lcfs_readlink(int fd, lcfs_oid_t oid, char *buf, size_t bufsize) {
                                                                       DEBUG_ENTER();
                                                                       uint64_t block;
                                                                       if (lcfs_object_location(fd, oid, &block) < 0) return -1;
                                                                       lcfs_obj_header hdr;
                                                                       if (lcfs_read_header(fd, block, &hdr) < 0) return -1;
                                                                       if (hdr.type != OBJ_TYPE_SYMLINK) return -1;
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
                                                                       } else {
                                                                           char *tmp = malloc(hdr.size + 1);
                                                                           if (!tmp) return -1;
                                                                           lcfs_read_file(fd, oid, tmp, hdr.size, 0);
                                                                           tmp[hdr.size] = '\0';
                                                                           strncpy(buf, tmp, bufsize);
                                                                           free(tmp);
                                                                       }
                                                                       DEBUG_PRINT("Target del symlink: '%s'", buf);
                                                                       DEBUG_EXIT(0);
                                                                       return 0;
                                                                   }

                                                                   int lcfs_create_dir(int fd, lcfs_oid_t parent_oid, const char *name, lcfs_oid_t *new_oid) {
                                                                       DEBUG_ENTER();
                                                                       lcfs_oid_t oid;
                                                                       if (lcfs_create_object(fd, OBJ_TYPE_DIR, parent_oid, name, &oid, NULL) < 0) return -1;
                                                                       if (lcfs_add_dir_entry(fd, parent_oid, oid, OBJ_TYPE_DIR, name) < 0) {
                                                                           lcfs_delete_object(fd, oid);
                                                                           return -1;
                                                                       }
                                                                       if (new_oid) *new_oid = oid;
                                                                       DEBUG_EXIT(0);
                                                                       return 0;
                                                                   }

                                                                   int lcfs_create_symlink(int fd, lcfs_oid_t parent_oid, const char *name, const char *target, lcfs_oid_t *new_oid) {
                                                                       DEBUG_ENTER();
                                                                       lcfs_oid_t oid;
                                                                       if (lcfs_create_object(fd, OBJ_TYPE_SYMLINK, parent_oid, name, &oid, NULL) < 0) return -1;
                                                                       uint64_t block;
                                                                       if (lcfs_object_location(fd, oid, &block) < 0) return -1;
                                                                       lcfs_obj_header hdr;
                                                                       if (lcfs_read_header(fd, block, &hdr) < 0) return -1;
                                                                       uint8_t obj_block[LCFS_BLOCK_SIZE];
                                                                       if (lcfs_read_block(fd, block, obj_block) < 0) return -1;
                                                                       uint16_t name_len;
                                                                       memcpy(&name_len, obj_block + LCFS_HEADER_SIZE, 2);
                                                                       size_t data_start = LCFS_HEADER_SIZE + 2 + name_len;
                                                                       size_t target_len = strlen(target);
                                                                       if (target_len > LCFS_BLOCK_SIZE - data_start) {
                                                                           lcfs_delete_object(fd, oid);
                                                                           errno = ENAMETOOLONG;
                                                                           return -1;
                                                                       }
                                                                       memcpy(obj_block + data_start, target, target_len);
                                                                       hdr.size = target_len;
                                                                       hdr.header_crc = 0;
                                                                       hdr.header_crc = header_crc(&hdr);
                                                                       memcpy(obj_block, &hdr, sizeof(hdr));
                                                                       if (lcfs_write_block(fd, block, obj_block) < 0) {
                                                                           lcfs_delete_object(fd, oid);
                                                                           return -1;
                                                                       }
                                                                       if (lcfs_add_dir_entry(fd, parent_oid, oid, OBJ_TYPE_SYMLINK, name) < 0) {
                                                                           lcfs_delete_object(fd, oid);
                                                                           return -1;
                                                                       }
                                                                       if (new_oid) *new_oid = oid;
                                                                       DEBUG_EXIT(0);
                                                                       return 0;
                                                                   }

                                                                   int lcfs_rename(int fd, lcfs_oid_t old_parent, const char *old_name,
                                                                                   lcfs_oid_t new_parent, const char *new_name) {
                                                                       DEBUG_ENTER();
                                                                       lcfs_oid_t oid;
                                                                       uint16_t type;
                                                                       if (lcfs_lookup_name(fd, old_parent, old_name, &oid, &type) < 0) return -1;
                                                                       lcfs_oid_t dummy;
                                                                       if (lcfs_lookup_name(fd, new_parent, new_name, &dummy, &type) == 0) {
                                                                           errno = EEXIST;
                                                                           return -1;
                                                                       }
                                                                       if (lcfs_add_dir_entry(fd, new_parent, oid, type, new_name) < 0) return -1;
                                                                       if (lcfs_remove_dir_entry(fd, old_parent, old_name) < 0) {
                                                                           lcfs_remove_dir_entry(fd, new_parent, new_name);
                                                                           return -1;
                                                                       }
                                                                       uint64_t block;
                                                                       if (lcfs_object_location(fd, oid, &block) < 0) return -1;
                                                                       lcfs_obj_header hdr;
                                                                       if (lcfs_read_header(fd, block, &hdr) < 0) return -1;
                                                                       hdr.parent_oid = new_parent;
                                                                       hdr.header_crc = 0;
                                                                       hdr.header_crc = header_crc(&hdr);
                                                                       if (lcfs_write_header(fd, block, &hdr) < 0) return -1;
                                                                       DEBUG_EXIT(0);
                                                                       return 0;
                                                                                   }

                                                                                   int lcfs_unlink(int fd, lcfs_oid_t parent_oid, const char *name) {
                                                                                       DEBUG_ENTER();
                                                                                       lcfs_oid_t oid;
                                                                                       uint16_t type;
                                                                                       if (lcfs_lookup_name(fd, parent_oid, name, &oid, &type) < 0) return -1;
                                                                                       if (type == OBJ_TYPE_DIR) {
                                                                                           DEBUG_ERROR("'%s' es un directorio, use rmdir", name);
                                                                                           errno = EISDIR;
                                                                                           return -1;
                                                                                       }
                                                                                       if (lcfs_remove_dir_entry(fd, parent_oid, name) < 0) return -1;
                                                                                       if (lcfs_delete_object(fd, oid) < 0) return -1;
                                                                                       DEBUG_EXIT(0);
                                                                                       return 0;
                                                                                   }

                                                                                   int lcfs_rebuild_free_map(int fd, uint8_t **bitmap, uint64_t *bitmap_blocks) {
                                                                                       DEBUG_ENTER();
                                                                                       off_t dev_size = lseek(fd, 0, SEEK_END);
                                                                                       uint64_t total_blocks = dev_size / LCFS_BLOCK_SIZE;
                                                                                       uint64_t bm_blocks = (total_blocks + LCFS_BLOCK_SIZE*8 - 1) / (LCFS_BLOCK_SIZE*8);
                                                                                       uint8_t *bm = calloc(bm_blocks, LCFS_BLOCK_SIZE);
                                                                                       if (!bm) return -1;
                                                                                       for (uint64_t b = 0; b < total_blocks; b++) {
                                                                                           uint8_t block[LCFS_BLOCK_SIZE];
                                                                                           if (lcfs_read_block(fd, b, block) < 0) continue;
                                                                                           lcfs_obj_header hdr;
                                                                                           memcpy(&hdr, block, sizeof(hdr));
                                                                                           if (memcmp(hdr.magic, LCFS_MAGIC, LCFS_MAGIC_LEN) == 0 &&
                                                                                               lcfs_validate_header(&hdr) == 0) {
                                                                                               uint64_t byte_idx = b / 8;
                                                                                           uint8_t bit = 1 << (b % 8);
                                                                                           bm[byte_idx] |= bit;
                                                                                           if ((hdr.type == OBJ_TYPE_FILE || hdr.type == OBJ_TYPE_SYMLINK) && hdr.num_extents > 0) {
                                                                                               uint16_t name_len;
                                                                                               memcpy(&name_len, block + LCFS_HEADER_SIZE, 2);
                                                                                               size_t data_start = LCFS_HEADER_SIZE + 2 + name_len;
                                                                                               lcfs_extent *extents = (lcfs_extent*)(block + data_start);
                                                                                               uint64_t phys_start = extents[0].physical_block;
                                                                                               uint64_t num_blocks = (hdr.size + LCFS_BLOCK_SIZE - 1) / LCFS_BLOCK_SIZE;
                                                                                               for (uint64_t i = 0; i < num_blocks; i++) {
                                                                                                   uint64_t phys = phys_start + i;
                                                                                                   if (phys < total_blocks) {
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
                                                                                       DEBUG_EXIT(0);
                                                                                       return 0;
                                                                                   }

                                                                                   int lcfs_rebuild_oid_map(int fd) {
                                                                                       DEBUG_ENTER();
                                                                                       off_t dev_size = lseek(fd, 0, SEEK_END);
                                                                                       uint64_t total_blocks = dev_size / LCFS_BLOCK_SIZE;
                                                                                       uint8_t *bitmap = NULL;
                                                                                       uint64_t bm_blocks = 0;
                                                                                       if (lcfs_rebuild_free_map(fd, &bitmap, &bm_blocks) < 0) return -1;
                                                                                       free(bitmap);

                                                                                       uint64_t map_block;
                                                                                       if (lcfs_object_location(fd, LCFS_OID_MAP_OID, &map_block) < 0) {
                                                                                           if (lcfs_alloc_block(fd, &map_block) < 0) return -1;
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
                                                                                           if (lcfs_write_header(fd, map_block, &om) < 0) return -1;
                                                                                       }

                                                                                       uint8_t map_data[LCFS_BLOCK_SIZE] = {0};
                                                                                       if (lcfs_read_block(fd, map_block, map_data) < 0) return -1;
                                                                                       lcfs_obj_header map_hdr;
                                                                                       memcpy(&map_hdr, map_data, sizeof(map_hdr));
                                                                                       uint64_t count = map_hdr.size / 16;
                                                                                       uint64_t *entries = (uint64_t*)(map_data + LCFS_HEADER_SIZE);

                                                                                       for (uint64_t b = 0; b < total_blocks; b++) {
                                                                                           lcfs_obj_header hdr;
                                                                                           if (lcfs_read_header(fd, b, &hdr) == 0) {
                                                                                               int exists = 0;
                                                                                               for (uint64_t i = 0; i < count; i++) {
                                                                                                   if (entries[2*i] == hdr.oid) { exists = 1; break; }
                                                                                               }
                                                                                               if (!exists) {
                                                                                                   entries[2*count] = hdr.oid;
                                                                                                   entries[2*count+1] = b;
                                                                                                   count++;
                                                                                                   if (count * 16 > LCFS_BLOCK_SIZE - LCFS_HEADER_SIZE) break;
                                                                                               }
                                                                                           }
                                                                                       }
                                                                                       map_hdr.size = count * 16;
                                                                                       map_hdr.header_crc = 0;
                                                                                       map_hdr.header_crc = header_crc(&map_hdr);
                                                                                       memcpy(map_data, &map_hdr, sizeof(map_hdr));
                                                                                       if (lcfs_write_block(fd, map_block, map_data) < 0) return -1;
                                                                                       DEBUG_EXIT(0);
                                                                                       return 0;
                                                                                   }
