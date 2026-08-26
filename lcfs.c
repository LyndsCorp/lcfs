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

/* ========================== CRC32C ========================== */
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

/* ========================== I/O básico ========================== */
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
    fsync(fd);
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
    if (max_len == 0) { errno = EINVAL; return -1; }
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

/* ========================== Superbloque ========================== */
int lcfs_init_superblock(int fd, uint64_t total_blocks) {
    DEBUG_ENTER();
    // Superbloque
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

    // Calcular bloques necesarios para el mapa libre
    uint64_t bm_blocks = (total_blocks + LCFS_BLOCK_SIZE*8 - 1) / (LCFS_BLOCK_SIZE*8);
    uint8_t *bitmap = calloc(bm_blocks, LCFS_BLOCK_SIZE);
    if (!bitmap) return -1;
    bitmap[0] |= 0x03; // bloques 0 y 1 ocupados

    // ---- Objeto del mapa libre (bloque 1) ----
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

    uint8_t obj_block[LCFS_BLOCK_SIZE] = {0};
    uint16_t name_len = 0;
    memcpy(obj_block, &fm, sizeof(fm));
    memcpy(obj_block + LCFS_HEADER_SIZE, &name_len, 2);
    size_t data_start = LCFS_HEADER_SIZE + 2 + name_len;
    lcfs_extent *extents = (lcfs_extent*)(obj_block + data_start);
    for (uint64_t i = 0; i < bm_blocks; i++) {
        extents[i].logical_block = i;
        extents[i].physical_block = 2 + i;
        extents[i].block_count = 1;
    }
    if (lcfs_write_block(fd, 1, obj_block) < 0) { free(bitmap); return -1; }

    // Escribir los bloques de datos del bitmap
    for (uint64_t i = 0; i < bm_blocks; i++) {
        uint64_t byte_idx = (2 + i) / 8;
        uint8_t bit = 1 << ((2 + i) % 8);
        bitmap[byte_idx] |= bit;
        if (lcfs_write_block(fd, 2 + i, bitmap + i*LCFS_BLOCK_SIZE) < 0) {
            free(bitmap); return -1;
        }
    }

    // ---- Objeto del mapa OID ----
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

    // Actualizar mapa libre en disco
    if (lcfs_set_free_map(fd, bitmap, bm_blocks) < 0) { free(bitmap); return -1; }
    free(bitmap);

    // ---- Crear directorio raíz con OID temporal ----
    uint64_t root_blk;
    lcfs_oid_t root_oid_tmp;
    if (lcfs_create_object(fd, OBJ_TYPE_DIR, 0, "/", &root_oid_tmp, &root_blk) < 0) return -1;

    // Cambiar su OID a LCFS_ROOT_OID (3)
    lcfs_obj_header hdr;
    if (lcfs_read_header(fd, root_blk, &hdr) < 0) return -1;
    hdr.oid = LCFS_ROOT_OID;
    hdr.header_crc = 0;
    hdr.header_crc = header_crc(&hdr);
    if (lcfs_write_header(fd, root_blk, &hdr) < 0) return -1;

    // Actualizar el mapa OID: eliminar el OID temporal y añadir el 3
    if (lcfs_oid_map_remove(fd, root_oid_tmp) < 0) return -1;
    if (lcfs_oid_map_add(fd, LCFS_ROOT_OID, root_blk) < 0) return -1;

    // Actualizar superbloque
    if (lcfs_read_header(fd, 0, &sb) < 0) return -1;
    sb.first_child_oid = LCFS_ROOT_OID;
    sb.generation = 4;
    sb.header_crc = 0;
    sb.header_crc = header_crc(&sb);
    if (lcfs_write_header(fd, 0, &sb) < 0) return -1;

    DEBUG_EXIT(0);
    return 0;
}

/* ========================== Mapa libre (usando extents) ========================== */
static int read_free_map_data(int fd, uint8_t **data, uint64_t *len) {
    uint64_t map_block;
    if (lcfs_object_location(fd, LCFS_FREE_MAP_OID, &map_block) < 0) return -1;
    uint8_t obj_block[LCFS_BLOCK_SIZE];
    if (lcfs_read_block(fd, map_block, obj_block) < 0) return -1;
    lcfs_obj_header tmp;
    memcpy(&tmp, obj_block, sizeof(tmp));
    if (tmp.type != OBJ_TYPE_FREE_MAP) return -1;

    uint16_t name_len;
    memcpy(&name_len, obj_block + LCFS_HEADER_SIZE, 2);
    size_t data_start = LCFS_HEADER_SIZE + 2 + name_len;
    lcfs_extent *extents = (lcfs_extent*)(obj_block + data_start);

    size_t total_bytes = tmp.size;
    uint8_t *buf = malloc(total_bytes ? total_bytes : 1);
    if (!buf) return -1;
    if (total_bytes == 0) { *data = buf; *len = 0; return 0; }

    uint64_t offset = 0;
    for (uint32_t i = 0; i < tmp.num_extents; i++) {
        for (uint64_t j = 0; j < extents[i].block_count; j++) {
            if (offset >= total_bytes) break;
            if (lcfs_read_block(fd, extents[i].physical_block + j, buf + offset) < 0) {
                free(buf);
                return -1;
            }
            offset += LCFS_BLOCK_SIZE;
        }
    }
    *data = buf;
    *len = total_bytes;
    return 0;
}

static int write_free_map_data(int fd, const uint8_t *data, size_t len) {
    uint64_t map_block;
    if (lcfs_object_location(fd, LCFS_FREE_MAP_OID, &map_block) < 0) return -1;
    uint8_t obj_block[LCFS_BLOCK_SIZE];
    if (lcfs_read_block(fd, map_block, obj_block) < 0) return -1;
    lcfs_obj_header tmp;
    memcpy(&tmp, obj_block, sizeof(tmp));
    if (tmp.type != OBJ_TYPE_FREE_MAP) return -1;

    uint16_t name_len;
    memcpy(&name_len, obj_block + LCFS_HEADER_SIZE, 2);
    size_t data_start = LCFS_HEADER_SIZE + 2 + name_len;
    lcfs_extent *extents = (lcfs_extent*)(obj_block + data_start);

    uint64_t needed_blocks = (len + LCFS_BLOCK_SIZE - 1) / LCFS_BLOCK_SIZE;
    uint64_t current_blocks = tmp.size / LCFS_BLOCK_SIZE;
    uint32_t count = tmp.num_extents;

    if (needed_blocks > current_blocks) {
        uint64_t extra = needed_blocks - current_blocks;
        if (count > 0) {
            uint64_t last_phys = extents[count-1].physical_block + extents[count-1].block_count;
            int ok = 1;
            for (uint64_t i = 0; i < extra; i++) {
                uint8_t *bm; uint64_t bm_blocks;
                if (lcfs_get_free_map(fd, &bm, &bm_blocks) < 0) return -1;
                uint64_t byte_idx = (last_phys + i) / 8;
                uint8_t bit = 1 << ((last_phys + i) % 8);
                if (bm[byte_idx] & bit) { ok = 0; free(bm); break; }
                free(bm);
            }
            if (ok) {
                for (uint64_t i = 0; i < extra; i++) {
                    uint8_t *bm; uint64_t bm_blocks;
                    if (lcfs_get_free_map(fd, &bm, &bm_blocks) < 0) return -1;
                    uint64_t byte_idx = (last_phys + i) / 8;
                    uint8_t bit = 1 << ((last_phys + i) % 8);
                    bm[byte_idx] |= bit;
                    if (lcfs_set_free_map(fd, bm, bm_blocks) < 0) { free(bm); return -1; }
                    free(bm);
                }
                extents[count-1].block_count += extra;
            } else {
                for (uint64_t i = 0; i < extra; i++) {
                    uint64_t new_blk;
                    if (lcfs_alloc_block(fd, &new_blk) < 0) return -1;
                    if (count < (LCFS_BLOCK_SIZE - data_start) / sizeof(lcfs_extent)) {
                        extents[count].logical_block = current_blocks + i;
                        extents[count].physical_block = new_blk;
                        extents[count].block_count = 1;
                        count++;
                    } else {
                        lcfs_free_block(fd, new_blk);
                        errno = ENOSPC;
                        return -1;
                    }
                }
            }
        } else {
            for (uint64_t i = 0; i < extra; i++) {
                uint64_t new_blk;
                if (lcfs_alloc_block(fd, &new_blk) < 0) return -1;
                if (count < (LCFS_BLOCK_SIZE - data_start) / sizeof(lcfs_extent)) {
                    extents[count].logical_block = i;
                    extents[count].physical_block = new_blk;
                    extents[count].block_count = 1;
                    count++;
                } else {
                    lcfs_free_block(fd, new_blk);
                    errno = ENOSPC;
                    return -1;
                }
            }
        }
        tmp.num_extents = count;
        tmp.size = needed_blocks * LCFS_BLOCK_SIZE;
        tmp.header_crc = 0;
        tmp.header_crc = header_crc(&tmp);
        memcpy(obj_block, &tmp, sizeof(tmp));
        if (lcfs_write_block(fd, map_block, obj_block) < 0) return -1;
    } else if (needed_blocks < current_blocks) {
        uint64_t to_free = current_blocks - needed_blocks;
        for (int i = count - 1; i >= 0 && to_free > 0; i--) {
            if (extents[i].block_count > to_free) {
                uint64_t start = extents[i].physical_block + extents[i].block_count - to_free;
                for (uint64_t j = 0; j < to_free; j++) {
                    lcfs_free_block(fd, start + j);
                }
                extents[i].block_count -= to_free;
                to_free = 0;
            } else {
                for (uint64_t j = 0; j < extents[i].block_count; j++) {
                    lcfs_free_block(fd, extents[i].physical_block + j);
                }
                to_free -= extents[i].block_count;
                count--;
            }
        }
        tmp.num_extents = count;
        tmp.size = needed_blocks * LCFS_BLOCK_SIZE;
        tmp.header_crc = 0;
        tmp.header_crc = header_crc(&tmp);
        memcpy(obj_block, &tmp, sizeof(tmp));
        if (lcfs_write_block(fd, map_block, obj_block) < 0) return -1;
    }

    uint64_t offset = 0;
    for (uint32_t i = 0; i < tmp.num_extents; i++) {
        for (uint64_t j = 0; j < extents[i].block_count; j++) {
            if (offset >= len) break;
            size_t to_write = (len - offset > LCFS_BLOCK_SIZE) ? LCFS_BLOCK_SIZE : (len - offset);
            uint8_t block[LCFS_BLOCK_SIZE] = {0};
            memcpy(block, data + offset, to_write);
            if (lcfs_write_block(fd, extents[i].physical_block + j, block) < 0) return -1;
            offset += LCFS_BLOCK_SIZE;
        }
    }
    return 0;
}

int lcfs_get_free_map(int fd, uint8_t **bitmap, uint64_t *bitmap_blocks) {
    DEBUG_ENTER();
    uint8_t *data;
    uint64_t len;
    if (read_free_map_data(fd, &data, &len) < 0) return -1;
    *bitmap = data;
    *bitmap_blocks = len / LCFS_BLOCK_SIZE;
    DEBUG_EXIT(0);
    return 0;
}

int lcfs_set_free_map(int fd, const uint8_t *bitmap, uint64_t bitmap_blocks) {
    DEBUG_ENTER();
    if (write_free_map_data(fd, bitmap, bitmap_blocks * LCFS_BLOCK_SIZE) < 0) return -1;
    DEBUG_EXIT(0);
    return 0;
}

/* ========================== Asignación / liberación de bloques ========================== */
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
                bitmap[byte_idx] &= ~bit;
                free(bitmap);
                return -1;
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
    off_t dev_size = lseek(fd, 0, SEEK_END);
    if (dev_size < 0) return -1;
    uint64_t total_blocks = dev_size / LCFS_BLOCK_SIZE;
    if (block_num >= total_blocks) {
        errno = EINVAL;
        return -1;
    }
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
    DEBUG_EXIT(0);
    return 0;
}

/* ========================== OID Map (extensible) ========================== */
static int read_oid_map_data(int fd, uint8_t **data, size_t *len) {
    uint64_t map_block;
    if (lcfs_object_location(fd, LCFS_OID_MAP_OID, &map_block) < 0) return -1;
    uint8_t obj_block[LCFS_BLOCK_SIZE];
    if (lcfs_read_block(fd, map_block, obj_block) < 0) return -1;
    lcfs_obj_header tmp;
    memcpy(&tmp, obj_block, sizeof(tmp));
    if (tmp.type != OBJ_TYPE_OID_MAP) return -1;

    uint16_t name_len;
    memcpy(&name_len, obj_block + LCFS_HEADER_SIZE, 2);
    size_t data_start = LCFS_HEADER_SIZE + 2 + name_len;
    lcfs_extent *extents = (lcfs_extent*)(obj_block + data_start);

    size_t total = tmp.size;
    uint8_t *buf = malloc(total ? total : 1);
    if (!buf) return -1;
    if (total == 0) { *data = buf; *len = 0; return 0; }

    uint64_t offset = 0;
    for (uint32_t i = 0; i < tmp.num_extents; i++) {
        for (uint64_t j = 0; j < extents[i].block_count; j++) {
            if (offset >= total) break;
            if (lcfs_read_block(fd, extents[i].physical_block + j, buf + offset) < 0) {
                free(buf);
                return -1;
            }
            offset += LCFS_BLOCK_SIZE;
        }
    }
    *data = buf;
    *len = total;
    return 0;
}

static int write_oid_map_data(int fd, const uint8_t *data, size_t len) {
    uint64_t map_block;
    if (lcfs_object_location(fd, LCFS_OID_MAP_OID, &map_block) < 0) return -1;
    uint8_t obj_block[LCFS_BLOCK_SIZE];
    if (lcfs_read_block(fd, map_block, obj_block) < 0) return -1;
    lcfs_obj_header tmp;
    memcpy(&tmp, obj_block, sizeof(tmp));
    if (tmp.type != OBJ_TYPE_OID_MAP) return -1;

    uint16_t name_len;
    memcpy(&name_len, obj_block + LCFS_HEADER_SIZE, 2);
    size_t data_start = LCFS_HEADER_SIZE + 2 + name_len;
    lcfs_extent *extents = (lcfs_extent*)(obj_block + data_start);

    uint64_t needed_blocks = (len + LCFS_BLOCK_SIZE - 1) / LCFS_BLOCK_SIZE;
    uint64_t current_blocks = tmp.size / LCFS_BLOCK_SIZE;
    uint32_t count = tmp.num_extents;

    if (needed_blocks > current_blocks) {
        uint64_t extra = needed_blocks - current_blocks;
        if (count > 0) {
            uint64_t last_phys = extents[count-1].physical_block + extents[count-1].block_count;
            int ok = 1;
            for (uint64_t i = 0; i < extra; i++) {
                uint8_t *bm; uint64_t bm_blocks;
                if (lcfs_get_free_map(fd, &bm, &bm_blocks) < 0) return -1;
                uint64_t byte_idx = (last_phys + i) / 8;
                uint8_t bit = 1 << ((last_phys + i) % 8);
                if (bm[byte_idx] & bit) { ok = 0; free(bm); break; }
                free(bm);
            }
            if (ok) {
                for (uint64_t i = 0; i < extra; i++) {
                    uint8_t *bm; uint64_t bm_blocks;
                    if (lcfs_get_free_map(fd, &bm, &bm_blocks) < 0) return -1;
                    uint64_t byte_idx = (last_phys + i) / 8;
                    uint8_t bit = 1 << ((last_phys + i) % 8);
                    bm[byte_idx] |= bit;
                    if (lcfs_set_free_map(fd, bm, bm_blocks) < 0) { free(bm); return -1; }
                    free(bm);
                }
                extents[count-1].block_count += extra;
            } else {
                for (uint64_t i = 0; i < extra; i++) {
                    uint64_t new_blk;
                    if (lcfs_alloc_block(fd, &new_blk) < 0) return -1;
                    if (count < (LCFS_BLOCK_SIZE - data_start) / sizeof(lcfs_extent)) {
                        extents[count].logical_block = current_blocks + i;
                        extents[count].physical_block = new_blk;
                        extents[count].block_count = 1;
                        count++;
                    } else {
                        lcfs_free_block(fd, new_blk);
                        errno = ENOSPC;
                        return -1;
                    }
                }
            }
        } else {
            for (uint64_t i = 0; i < extra; i++) {
                uint64_t new_blk;
                if (lcfs_alloc_block(fd, &new_blk) < 0) return -1;
                if (count < (LCFS_BLOCK_SIZE - data_start) / sizeof(lcfs_extent)) {
                    extents[count].logical_block = i;
                    extents[count].physical_block = new_blk;
                    extents[count].block_count = 1;
                    count++;
                } else {
                    lcfs_free_block(fd, new_blk);
                    errno = ENOSPC;
                    return -1;
                }
            }
        }
        tmp.num_extents = count;
        tmp.size = needed_blocks * LCFS_BLOCK_SIZE;
        tmp.header_crc = 0;
        tmp.header_crc = header_crc(&tmp);
        memcpy(obj_block, &tmp, sizeof(tmp));
        if (lcfs_write_block(fd, map_block, obj_block) < 0) return -1;
    } else if (needed_blocks < current_blocks) {
        uint64_t to_free = current_blocks - needed_blocks;
        for (int i = count - 1; i >= 0 && to_free > 0; i--) {
            if (extents[i].block_count > to_free) {
                uint64_t start = extents[i].physical_block + extents[i].block_count - to_free;
                for (uint64_t j = 0; j < to_free; j++) {
                    lcfs_free_block(fd, start + j);
                }
                extents[i].block_count -= to_free;
                to_free = 0;
            } else {
                for (uint64_t j = 0; j < extents[i].block_count; j++) {
                    lcfs_free_block(fd, extents[i].physical_block + j);
                }
                to_free -= extents[i].block_count;
                count--;
            }
        }
        tmp.num_extents = count;
        tmp.size = needed_blocks * LCFS_BLOCK_SIZE;
        tmp.header_crc = 0;
        tmp.header_crc = header_crc(&tmp);
        memcpy(obj_block, &tmp, sizeof(tmp));
        if (lcfs_write_block(fd, map_block, obj_block) < 0) return -1;
    }

    uint64_t offset = 0;
    for (uint32_t i = 0; i < tmp.num_extents; i++) {
        for (uint64_t j = 0; j < extents[i].block_count; j++) {
            if (offset >= len) break;
            size_t to_write = (len - offset > LCFS_BLOCK_SIZE) ? LCFS_BLOCK_SIZE : (len - offset);
            uint8_t block[LCFS_BLOCK_SIZE] = {0};
            memcpy(block, data + offset, to_write);
            if (lcfs_write_block(fd, extents[i].physical_block + j, block) < 0) return -1;
            offset += LCFS_BLOCK_SIZE;
        }
    }
    return 0;
}

int lcfs_oid_map_add(int fd, lcfs_oid_t oid, uint64_t block) {
    DEBUG_ENTER();
    uint8_t *data;
    size_t len;
    if (read_oid_map_data(fd, &data, &len) < 0) return -1;
    uint64_t count = len / 16;
    uint64_t *entries = (uint64_t*)data;
    for (uint64_t i = 0; i < count; i++) {
        if (entries[2*i] == oid) {
            free(data);
            errno = EEXIST;
            return -1;
        }
    }
    uint8_t *newdata = realloc(data, len + 16);
    if (!newdata) { free(data); return -1; }
    data = newdata;
    uint64_t *new_entries = (uint64_t*)data;
    new_entries[2*count] = oid;
    new_entries[2*count+1] = block;
    len += 16;
    if (write_oid_map_data(fd, data, len) < 0) {
        free(data);
        return -1;
    }
    free(data);
    DEBUG_EXIT(0);
    return 0;
}

int lcfs_oid_map_remove(int fd, lcfs_oid_t oid) {
    DEBUG_ENTER();
    uint8_t *data;
    size_t len;
    if (read_oid_map_data(fd, &data, &len) < 0) return -1;
    uint64_t count = len / 16;
    uint64_t *entries = (uint64_t*)data;
    int found = 0;
    for (uint64_t i = 0; i < count; i++) {
        if (entries[2*i] == oid) {
            if (i != count-1) {
                entries[2*i] = entries[2*(count-1)];
                entries[2*i+1] = entries[2*(count-1)+1];
            }
            len -= 16;
            found = 1;
            break;
        }
    }
    if (!found) {
        free(data);
        errno = ENOENT;
        return -1;
    }
    if (write_oid_map_data(fd, data, len) < 0) {
        free(data);
        return -1;
    }
    free(data);
    DEBUG_EXIT(0);
    return 0;
}

/* ========================== Localización de objetos ========================== */
int lcfs_object_location(int fd, lcfs_oid_t oid, uint64_t *block_num) {
    DEBUG_ENTER();

    if (oid != LCFS_OID_MAP_OID) {
        uint8_t *data;
        size_t len;
        if (read_oid_map_data(fd, &data, &len) == 0 && len > 0) {
            uint64_t count = len / 16;
            uint64_t *entries = (uint64_t*)data;
            for (uint64_t i = 0; i < count; i++) {
                if (entries[2*i] == oid) {
                    *block_num = entries[2*i+1];
                    free(data);
                    DEBUG_PRINT("Encontrado en bloque %" PRIu64 " (OID map)", *block_num);
                    DEBUG_EXIT(0);
                    return 0;
                }
            }
            free(data);
        }
    }

    off_t dev_size = lseek(fd, 0, SEEK_END);
    if (dev_size < 0) { errno = EIO; return -1; }
    uint64_t total_blocks = dev_size / LCFS_BLOCK_SIZE;
    for (uint64_t b = 0; b < total_blocks; b++) {
        lcfs_obj_header obj_hdr;
        if (lcfs_read_header(fd, b, &obj_hdr) == 0 && obj_hdr.oid == oid) {
            *block_num = b;
            DEBUG_PRINT("Encontrado en bloque %" PRIu64 " (scan)", b);
            DEBUG_EXIT(0);
            return 0;
        }
    }
    DEBUG_ERROR("OID %" PRIu64 " no encontrado", oid);
    errno = ENOENT;
    return -1;
}

/* ========================== Creación y eliminación de objetos ========================== */
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
    // Obtener nuevo OID del superbloque
    lcfs_obj_header sb;
    if (lcfs_read_header(fd, 0, &sb) == 0 && sb.type == OBJ_TYPE_SUPERBLOCK) {
        hdr.oid = sb.generation++;
        if (hdr.oid <= 3) hdr.oid = 4;
        sb.header_crc = 0;
        sb.header_crc = header_crc(&sb);
        if (lcfs_write_header(fd, 0, &sb) < 0) {
            lcfs_free_block(fd, blk);
            return -1;
        }
    } else {
        static int seeded = 0;
        if (!seeded) { srand(time(NULL)); seeded = 1; }
        do {
            hdr.oid = ((uint64_t)rand() << 32) | rand();
        } while (hdr.oid <= 3);
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
        lcfs_free_block(fd, blk);
        return -1;
    }
    if (lcfs_oid_map_add(fd, hdr.oid, blk) < 0) {
        lcfs_free_block(fd, blk);
        return -1;
    }
    if (new_oid) *new_oid = hdr.oid;
    if (block_num) *block_num = blk;
    DEBUG_PRINT("Objeto creado con OID %" PRIu64 " en bloque %" PRIu64, hdr.oid, blk);
    DEBUG_EXIT(0);
    return 0;
                       }

                       static void free_file_data_blocks(int fd, lcfs_obj_header *hdr, uint64_t obj_block) {
                           uint8_t obj_block_data[LCFS_BLOCK_SIZE];
                           if (lcfs_read_block(fd, obj_block, obj_block_data) < 0) return;
                           uint16_t name_len;
                           memcpy(&name_len, obj_block_data + LCFS_HEADER_SIZE, 2);
                           size_t data_start = LCFS_HEADER_SIZE + 2 + name_len;
                           if (hdr->num_extents > 0) {
                               lcfs_extent *extents = (lcfs_extent*)(obj_block_data + data_start);
                               for (uint32_t i = 0; i < hdr->num_extents; i++) {
                                   for (uint64_t j = 0; j < extents[i].block_count; j++) {
                                       lcfs_free_block(fd, extents[i].physical_block + j);
                                   }
                               }
                           }
                       }

                       int lcfs_delete_object(int fd, lcfs_oid_t oid) {
                           DEBUG_ENTER();
                           uint64_t block;
                           if (lcfs_object_location(fd, oid, &block) < 0) return -1;
                           lcfs_obj_header hdr;
                           if (lcfs_read_header(fd, block, &hdr) < 0) return -1;

                           if (hdr.type == OBJ_TYPE_DIR) {
                               uint8_t obj_data[LCFS_BLOCK_SIZE];
                               if (lcfs_read_block(fd, block, obj_data) < 0) return -1;
                               uint16_t name_len;
                               memcpy(&name_len, obj_data + LCFS_HEADER_SIZE, 2);
                               size_t pos = LCFS_HEADER_SIZE + 2 + name_len;
                               while (pos + sizeof(lcfs_dir_entry) <= LCFS_BLOCK_SIZE) {
                                   lcfs_dir_entry entry;
                                   memcpy(&entry, obj_data + pos, sizeof(entry));
                                   if (entry.child_oid == 0) break;
                                   errno = ENOTEMPTY;
                                   return -1;
                               }
                           }

                           if ((hdr.type == OBJ_TYPE_FILE || hdr.type == OBJ_TYPE_SYMLINK) && hdr.num_extents > 0) {
                               free_file_data_blocks(fd, &hdr, block);
                           }

                           if (lcfs_free_block(fd, block) < 0) return -1;
                           if (lcfs_oid_map_remove(fd, oid) < 0) return -1;
                           DEBUG_EXIT(0);
                           return 0;
                       }

                       /* ========================== Directorios ========================== */
                       int lcfs_lookup_name(int fd, lcfs_oid_t dir_oid, const char *name,
                                            lcfs_oid_t *child_oid, uint16_t *child_type) {
                           DEBUG_ENTER();
                           uint64_t dir_block;
                           if (lcfs_object_location(fd, dir_oid, &dir_block) < 0) return -1;
                           lcfs_obj_header hdr;
                           if (lcfs_read_header(fd, dir_block, &hdr) < 0) return -1;
                           if (hdr.type != OBJ_TYPE_DIR) { errno = ENOTDIR; return -1; }
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
                               if (entry.name_len > LCFS_MAX_NAME_LEN) break;
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
                                                if (lcfs_object_location(fd, dir_oid, &dir_block) < 0) { errno = ENOENT; return -1; }
                                                lcfs_obj_header hdr;
                                                if (lcfs_read_header(fd, dir_block, &hdr) < 0) { errno = EIO; return -1; }
                                                if (hdr.type != OBJ_TYPE_DIR) { errno = ENOTDIR; return -1; }
                                                uint8_t block[LCFS_BLOCK_SIZE];
                                                if (lcfs_read_block(fd, dir_block, block) < 0) { errno = EIO; return -1; }

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
                                                        if (lcfs_write_block(fd, dir_block, block) < 0) { errno = EIO; return -1; }
                                                        DEBUG_PRINT("Entrada añadida: %s (OID %" PRIu64 ")", name, child_oid);
                                                        DEBUG_EXIT(0);
                                                        return 0;
                                                    }
                                                    pos += sizeof(entry) + entry.name_len;
                                                    if (pos >= LCFS_BLOCK_SIZE) break;
                                                }
                                                DEBUG_ERROR("No hay espacio en directorio %" PRIu64 " para nueva entrada", dir_oid);
                                                errno = ENOSPC;
                                                return -1;
                                                                   }

                                                                   int lcfs_remove_dir_entry(int fd, lcfs_oid_t dir_oid, const char *name) {
                                                                       DEBUG_ENTER();
                                                                       uint64_t dir_block;
                                                                       if (lcfs_object_location(fd, dir_oid, &dir_block) < 0) { errno = ENOENT; return -1; }
                                                                       lcfs_obj_header hdr;
                                                                       if (lcfs_read_header(fd, dir_block, &hdr) < 0) { errno = EIO; return -1; }
                                                                       if (hdr.type != OBJ_TYPE_DIR) { errno = ENOTDIR; return -1; }
                                                                       uint8_t block[LCFS_BLOCK_SIZE];
                                                                       if (lcfs_read_block(fd, dir_block, block) < 0) { errno = EIO; return -1; }

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
                                                                           if (entry.name_len > LCFS_MAX_NAME_LEN) break;
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
                                                                               if (lcfs_write_block(fd, dir_block, block) < 0) { errno = EIO; return -1; }
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

                                                                   /* ========================== rmdir ========================== */
                                                                   int lcfs_rmdir(int fd, lcfs_oid_t parent_oid, const char *name) {
                                                                       DEBUG_ENTER();
                                                                       lcfs_oid_t child_oid;
                                                                       uint16_t child_type;
                                                                       if (lcfs_lookup_name(fd, parent_oid, name, &child_oid, &child_type) < 0) {
                                                                           errno = ENOENT;
                                                                           return -1;
                                                                       }
                                                                       if (child_type != OBJ_TYPE_DIR) {
                                                                           errno = ENOTDIR;
                                                                           return -1;
                                                                       }

                                                                       uint64_t block;
                                                                       if (lcfs_object_location(fd, child_oid, &block) < 0) return -1;
                                                                       uint8_t obj_data[LCFS_BLOCK_SIZE];
                                                                       if (lcfs_read_block(fd, block, obj_data) < 0) return -1;
                                                                       uint16_t name_len;
                                                                       memcpy(&name_len, obj_data + LCFS_HEADER_SIZE, 2);
                                                                       size_t pos = LCFS_HEADER_SIZE + 2 + name_len;
                                                                       while (pos + sizeof(lcfs_dir_entry) <= LCFS_BLOCK_SIZE) {
                                                                           lcfs_dir_entry entry;
                                                                           memcpy(&entry, obj_data + pos, sizeof(entry));
                                                                           if (entry.child_oid == 0) break;
                                                                           errno = ENOTEMPTY;
                                                                           return -1;
                                                                       }

                                                                       if (lcfs_remove_dir_entry(fd, parent_oid, name) < 0) return -1;
                                                                       if (lcfs_delete_object(fd, child_oid) < 0) {
                                                                           lcfs_add_dir_entry(fd, parent_oid, child_oid, OBJ_TYPE_DIR, name);
                                                                           return -1;
                                                                       }
                                                                       DEBUG_EXIT(0);
                                                                       return 0;
                                                                   }

                                                                   /* ========================== Extents auxiliares ========================== */
                                                                   static int find_extent(lcfs_extent *extents, uint32_t count, uint64_t logical_block) {
                                                                       for (uint32_t i = 0; i < count; i++) {
                                                                           if (logical_block >= extents[i].logical_block &&
                                                                               logical_block < extents[i].logical_block + extents[i].block_count) {
                                                                               return i;
                                                                               }
                                                                       }
                                                                       return -1;
                                                                   }

                                                                   /* ========================== Lectura de archivos ========================== */
                                                                   int lcfs_read_file(int fd, lcfs_oid_t oid, char *buf, size_t size, off_t offset) {
                                                                       DEBUG_ENTER();
                                                                       uint64_t block;
                                                                       if (lcfs_object_location(fd, oid, &block) < 0) return -1;
                                                                       lcfs_obj_header hdr;
                                                                       if (lcfs_read_header(fd, block, &hdr) < 0) return -1;
                                                                       if (hdr.type != OBJ_TYPE_FILE && hdr.type != OBJ_TYPE_SYMLINK) {
                                                                           errno = EINVAL;
                                                                           return -1;
                                                                       }
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
                                                                           if (data_start + offset + size > LCFS_BLOCK_SIZE)
                                                                               size = LCFS_BLOCK_SIZE - data_start - offset;
                                                                           memcpy(buf, obj_block + data_start + offset, size);
                                                                           total_read = size;
                                                                       } else {
                                                                           lcfs_extent *extents = (lcfs_extent*)(obj_block + data_start);
                                                                           uint32_t num_extents = hdr.num_extents;
                                                                           while (total_read < size && cur_off < (off_t)hdr.size) {
                                                                               uint64_t logical_block = cur_off / LCFS_BLOCK_SIZE;
                                                                               uint64_t off_in_block = cur_off % LCFS_BLOCK_SIZE;
                                                                               int idx = find_extent(extents, num_extents, logical_block);
                                                                               if (idx < 0) {
                                                                                   size_t len = LCFS_BLOCK_SIZE - off_in_block;
                                                                                   if (cur_off + len > hdr.size) len = hdr.size - cur_off;
                                                                                   if (len > size - total_read) len = size - total_read;
                                                                                   memset(buf + total_read, 0, len);
                                                                                   total_read += len;
                                                                                   cur_off += len;
                                                                                   continue;
                                                                               }
                                                                               uint64_t offset_in_extent = logical_block - extents[idx].logical_block;
                                                                               uint64_t phys_block = extents[idx].physical_block + offset_in_extent;
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

                                                                   /* ========================== Escritura de archivos (corregida) ========================== */
                                                                   int lcfs_write_file(int fd, lcfs_oid_t oid, const char *buf, size_t size, off_t offset) {
                                                                       DEBUG_ENTER();
                                                                       DEBUG_PRINT("Escribiendo archivo OID %" PRIu64 ", size %zu, offset %ld", oid, size, offset);

                                                                       uint64_t block;
                                                                       if (lcfs_object_location(fd, oid, &block) < 0) {
                                                                           DEBUG_ERROR("No se pudo localizar OID %" PRIu64, oid);
                                                                           return -1;
                                                                       }

                                                                       lcfs_obj_header hdr;
                                                                       if (lcfs_read_header(fd, block, &hdr) < 0) {
                                                                           DEBUG_ERROR("No se pudo leer encabezado");
                                                                           return -1;
                                                                       }
                                                                       if (hdr.type != OBJ_TYPE_FILE) {
                                                                           errno = EISDIR;
                                                                           return -1;
                                                                       }

                                                                       uint8_t obj_block[LCFS_BLOCK_SIZE];
                                                                       if (lcfs_read_block(fd, block, obj_block) < 0) {
                                                                           errno = EIO;
                                                                           return -1;
                                                                       }
                                                                       uint16_t name_len;
                                                                       memcpy(&name_len, obj_block + LCFS_HEADER_SIZE, 2);
                                                                       size_t data_start = LCFS_HEADER_SIZE + 2 + name_len;

                                                                       // Expandir si es necesario
                                                                       if ((uint64_t)offset + size > hdr.size) {
                                                                           if (lcfs_truncate_file(fd, oid, offset + size) < 0) {
                                                                               DEBUG_ERROR("lcfs_truncate_file falló");
                                                                               return -1;
                                                                           }
                                                                           if (lcfs_read_header(fd, block, &hdr) < 0) {
                                                                               errno = EIO;
                                                                               return -1;
                                                                           }
                                                                           if (lcfs_read_block(fd, block, obj_block) < 0) {
                                                                               errno = EIO;
                                                                               return -1;
                                                                           }
                                                                           memcpy(&name_len, obj_block + LCFS_HEADER_SIZE, 2);
                                                                           data_start = LCFS_HEADER_SIZE + 2 + name_len;
                                                                       }

                                                                       // Inline
                                                                       if (hdr.num_extents == 0) {
                                                                           if ((uint64_t)offset + size <= LCFS_BLOCK_SIZE - data_start) {
                                                                               memcpy(obj_block + data_start + offset, buf, size);
                                                                               if (offset + size > hdr.size) hdr.size = offset + size;
                                                                               hdr.header_crc = 0;
                                                                               hdr.header_crc = header_crc(&hdr);
                                                                               memcpy(obj_block, &hdr, sizeof(hdr));
                                                                               if (lcfs_write_block(fd, block, obj_block) < 0) {
                                                                                   errno = EIO;
                                                                                   return -1;
                                                                               }
                                                                               return size;
                                                                           } else {
                                                                               // Convertir a extents
                                                                               if (lcfs_truncate_file(fd, oid, offset + size) < 0) {
                                                                                   DEBUG_ERROR("Conversión a extents falló");
                                                                                   return -1;
                                                                               }
                                                                               if (lcfs_read_header(fd, block, &hdr) < 0) {
                                                                                   errno = EIO;
                                                                                   return -1;
                                                                               }
                                                                               if (lcfs_read_block(fd, block, obj_block) < 0) {
                                                                                   errno = EIO;
                                                                                   return -1;
                                                                               }
                                                                               memcpy(&name_len, obj_block + LCFS_HEADER_SIZE, 2);
                                                                               data_start = LCFS_HEADER_SIZE + 2 + name_len;
                                                                           }
                                                                       }

                                                                       // Escritura con extents
                                                                       lcfs_extent *extents = (lcfs_extent*)(obj_block + data_start);
                                                                       uint32_t num_extents = hdr.num_extents;
                                                                       off_t cur_off = offset;
                                                                       size_t total_written = 0;

                                                                       while (total_written < size && cur_off < (off_t)hdr.size) {
                                                                           uint64_t logical_block = cur_off / LCFS_BLOCK_SIZE;
                                                                           uint64_t off_in_block = cur_off % LCFS_BLOCK_SIZE;

                                                                           int idx = find_extent(extents, num_extents, logical_block);
                                                                           if (idx < 0) {
                                                                               uint64_t phys;
                                                                               if (lcfs_alloc_block(fd, &phys) < 0) {
                                                                                   DEBUG_ERROR("No se pudo asignar bloque para hueco");
                                                                                   break;
                                                                               }
                                                                               uint32_t pos = num_extents;
                                                                               for (uint32_t i = 0; i < num_extents; i++) {
                                                                                   if (extents[i].logical_block > logical_block) {
                                                                                       pos = i;
                                                                                       break;
                                                                                   }
                                                                               }
                                                                               if (num_extents < (LCFS_BLOCK_SIZE - data_start) / sizeof(lcfs_extent)) {
                                                                                   memmove(&extents[pos+1], &extents[pos], (num_extents - pos) * sizeof(lcfs_extent));
                                                                                   extents[pos].logical_block = logical_block;
                                                                                   extents[pos].physical_block = phys;
                                                                                   extents[pos].block_count = 1;
                                                                                   num_extents++;
                                                                                   hdr.num_extents = num_extents;
                                                                                   hdr.header_crc = 0;
                                                                                   hdr.header_crc = header_crc(&hdr);
                                                                                   memcpy(obj_block, &hdr, sizeof(hdr));
                                                                                   if (lcfs_write_block(fd, block, obj_block) < 0) {
                                                                                       lcfs_free_block(fd, phys);
                                                                                       errno = EIO;
                                                                                       return -1;
                                                                                   }
                                                                                   extents = (lcfs_extent*)(obj_block + data_start);
                                                                                   idx = pos;
                                                                               } else {
                                                                                   lcfs_free_block(fd, phys);
                                                                                   errno = ENOSPC;
                                                                                   return -1;
                                                                               }
                                                                           }

                                                                           uint64_t offset_in_extent = logical_block - extents[idx].logical_block;
                                                                           uint64_t phys_block = extents[idx].physical_block + offset_in_extent;

                                                                           uint8_t data[LCFS_BLOCK_SIZE];
                                                                           if (lcfs_read_block(fd, phys_block, data) < 0) {
                                                                               errno = EIO;
                                                                               return -1;
                                                                           }

                                                                           size_t len = LCFS_BLOCK_SIZE - off_in_block;
                                                                           if (cur_off + len > hdr.size) len = hdr.size - cur_off;
                                                                           if (len > size - total_written) len = size - total_written;

                                                                           memcpy(data + off_in_block, buf + total_written, len);
                                                                           if (lcfs_write_block(fd, phys_block, data) < 0) {
                                                                               errno = EIO;
                                                                               return -1;
                                                                           }

                                                                           total_written += len;
                                                                           cur_off += len;
                                                                       }

                                                                       DEBUG_PRINT("Escritura devolvió %zu bytes", total_written);
                                                                       return total_written;
                                                                   }

                                                                   /* ========================== Truncado (corregido) ========================== */
                                                                   int lcfs_truncate_file(int fd, lcfs_oid_t oid, off_t new_size) {
                                                                       DEBUG_ENTER();
                                                                       if (new_size < 0) new_size = 0;
                                                                       uint64_t block;
                                                                       if (lcfs_object_location(fd, oid, &block) < 0) return -1;
                                                                       lcfs_obj_header hdr;
                                                                       if (lcfs_read_header(fd, block, &hdr) < 0) return -1;
                                                                       if (hdr.type != OBJ_TYPE_FILE) {
                                                                           errno = EINVAL;
                                                                           return -1;
                                                                       }

                                                                       uint8_t obj_block[LCFS_BLOCK_SIZE];
                                                                       if (lcfs_read_block(fd, block, obj_block) < 0) {
                                                                           errno = EIO;
                                                                           return -1;
                                                                       }
                                                                       uint16_t name_len;
                                                                       memcpy(&name_len, obj_block + LCFS_HEADER_SIZE, 2);
                                                                       size_t data_start = LCFS_HEADER_SIZE + 2 + name_len;
                                                                       size_t max_inline = LCFS_BLOCK_SIZE - data_start;

                                                                       // Caso inline y tamaño cabe en inline
                                                                       if (hdr.num_extents == 0 && (size_t)new_size <= max_inline) {
                                                                           hdr.size = new_size;
                                                                           hdr.header_crc = 0;
                                                                           hdr.header_crc = header_crc(&hdr);
                                                                           memcpy(obj_block, &hdr, sizeof(hdr));
                                                                           if (lcfs_write_block(fd, block, obj_block) < 0) {
                                                                               errno = EIO;
                                                                               return -1;
                                                                           }
                                                                           return 0;
                                                                       }

                                                                       // Convertir de inline a extents
                                                                       if (hdr.num_extents == 0 && (size_t)new_size > max_inline) {
                                                                           uint8_t inline_data[LCFS_BLOCK_SIZE] = {0};
                                                                           if (hdr.size > 0) {
                                                                               memcpy(inline_data, obj_block + data_start, hdr.size);
                                                                           }
                                                                           uint64_t num_blocks = (new_size + LCFS_BLOCK_SIZE - 1) / LCFS_BLOCK_SIZE;
                                                                           if (num_blocks == 0) num_blocks = 1; // Asegurar al menos un bloque

                                                                           // Buscar bloques contiguos
                                                                           uint64_t start_block = 0;
                                                                           int contiguous = 0;
                                                                           off_t dev_size = lseek(fd, 0, SEEK_END);
                                                                           if (dev_size < 0) { errno = EIO; return -1; }
                                                                           uint64_t total_blocks = dev_size / LCFS_BLOCK_SIZE;
                                                                           for (uint64_t b = 0; b < total_blocks; b++) {
                                                                               int ok = 1;
                                                                               for (uint64_t i = 0; i < num_blocks; i++) {
                                                                                   uint8_t *bm; uint64_t bm_blocks;
                                                                                   if (lcfs_get_free_map(fd, &bm, &bm_blocks) < 0) return -1;
                                                                                   uint64_t byte_idx = (b + i) / 8;
                                                                                   uint8_t bit = 1 << ((b + i) % 8);
                                                                                   if (bm[byte_idx] & bit) { ok = 0; free(bm); break; }
                                                                                   free(bm);
                                                                               }
                                                                               if (ok) { start_block = b; contiguous = 1; break; }
                                                                           }

                                                                           lcfs_extent *extents = (lcfs_extent*)(obj_block + data_start);
                                                                           uint32_t count = 0;
                                                                           if (contiguous) {
                                                                               for (uint64_t i = 0; i < num_blocks; i++) {
                                                                                   uint8_t *bm; uint64_t bm_blocks;
                                                                                   if (lcfs_get_free_map(fd, &bm, &bm_blocks) < 0) return -1;
                                                                                   uint64_t byte_idx = (start_block + i) / 8;
                                                                                   uint8_t bit = 1 << ((start_block + i) % 8);
                                                                                   bm[byte_idx] |= bit;
                                                                                   if (lcfs_set_free_map(fd, bm, bm_blocks) < 0) { free(bm); return -1; }
                                                                                   free(bm);
                                                                               }
                                                                               extents[count].logical_block = 0;
                                                                               extents[count].physical_block = start_block;
                                                                               extents[count].block_count = num_blocks;
                                                                               count = 1;
                                                                           } else {
                                                                               for (uint64_t lb = 0; lb < num_blocks; lb++) {
                                                                                   uint64_t phys;
                                                                                   if (lcfs_alloc_block(fd, &phys) < 0) {
                                                                                       for (uint32_t i = 0; i < count; i++) {
                                                                                           for (uint64_t j = 0; j < extents[i].block_count; j++) {
                                                                                               lcfs_free_block(fd, extents[i].physical_block + j);
                                                                                           }
                                                                                       }
                                                                                       return -1;
                                                                                   }
                                                                                   if (count < (LCFS_BLOCK_SIZE - data_start) / sizeof(lcfs_extent)) {
                                                                                       extents[count].logical_block = lb;
                                                                                       extents[count].physical_block = phys;
                                                                                       extents[count].block_count = 1;
                                                                                       count++;
                                                                                   } else {
                                                                                       lcfs_free_block(fd, phys);
                                                                                       errno = ENOSPC;
                                                                                       return -1;
                                                                                   }
                                                                               }
                                                                           }

                                                                           hdr.num_extents = count;
                                                                           hdr.size = new_size;
                                                                           hdr.header_crc = 0;
                                                                           hdr.header_crc = header_crc(&hdr);
                                                                           memcpy(obj_block, &hdr, sizeof(hdr));
                                                                           if (lcfs_write_block(fd, block, obj_block) < 0) {
                                                                               errno = EIO;
                                                                               return -1;
                                                                           }

                                                                           // Escribir los datos inline en el primer bloque
                                                                           if (hdr.size > 0) {
                                                                               uint8_t data_block[LCFS_BLOCK_SIZE] = {0};
                                                                               size_t copy = (hdr.size < LCFS_BLOCK_SIZE) ? hdr.size : LCFS_BLOCK_SIZE;
                                                                               memcpy(data_block, inline_data, copy);
                                                                               if (lcfs_write_block(fd, extents[0].physical_block, data_block) < 0) {
                                                                                   errno = EIO;
                                                                                   return -1;
                                                                               }
                                                                           }
                                                                           return 0;
                                                                       }

                                                                       // Ya tiene extents
                                                                       if (hdr.num_extents > 0) {
                                                                           uint64_t current_blocks = (hdr.size + LCFS_BLOCK_SIZE - 1) / LCFS_BLOCK_SIZE;
                                                                           uint64_t needed_blocks = (new_size + LCFS_BLOCK_SIZE - 1) / LCFS_BLOCK_SIZE;
                                                                           lcfs_extent *extents = (lcfs_extent*)(obj_block + data_start);
                                                                           uint32_t count = hdr.num_extents;

                                                                           if (needed_blocks < current_blocks) {
                                                                               for (int i = count - 1; i >= 0; i--) {
                                                                                   uint64_t ext_start = extents[i].logical_block;
                                                                                   uint64_t ext_end = ext_start + extents[i].block_count;
                                                                                   if (ext_end > needed_blocks) {
                                                                                       uint64_t free_start = (needed_blocks > ext_start) ? needed_blocks : ext_start;
                                                                                       uint64_t to_free = extents[i].block_count - (free_start - ext_start);
                                                                                       for (uint64_t j = 0; j < to_free; j++) {
                                                                                           lcfs_free_block(fd, extents[i].physical_block + (free_start - ext_start) + j);
                                                                                       }
                                                                                       extents[i].block_count -= to_free;
                                                                                       if (extents[i].block_count == 0) {
                                                                                           for (uint32_t k = i; k < count - 1; k++) {
                                                                                               extents[k] = extents[k+1];
                                                                                           }
                                                                                           count--;
                                                                                       }
                                                                                   }
                                                                               }
                                                                               hdr.num_extents = count;
                                                                           }

                                                                           if (needed_blocks > current_blocks) {
                                                                               uint64_t lb = current_blocks;
                                                                               while (lb < needed_blocks) {
                                                                                   int idx = find_extent(extents, count, lb);
                                                                                   if (idx < 0) {
                                                                                       uint64_t phys;
                                                                                       if (lcfs_alloc_block(fd, &phys) < 0) return -1;
                                                                                       uint32_t pos = count;
                                                                                       for (uint32_t i = 0; i < count; i++) {
                                                                                           if (extents[i].logical_block > lb) {
                                                                                               pos = i;
                                                                                               break;
                                                                                           }
                                                                                       }
                                                                                       if (count < (LCFS_BLOCK_SIZE - data_start) / sizeof(lcfs_extent)) {
                                                                                           memmove(&extents[pos+1], &extents[pos], (count - pos) * sizeof(lcfs_extent));
                                                                                           extents[pos].logical_block = lb;
                                                                                           extents[pos].physical_block = phys;
                                                                                           extents[pos].block_count = 1;
                                                                                           count++;
                                                                                       } else {
                                                                                           lcfs_free_block(fd, phys);
                                                                                           errno = ENOSPC;
                                                                                           return -1;
                                                                                       }
                                                                                   }
                                                                                   lb++;
                                                                               }
                                                                               hdr.num_extents = count;
                                                                           }

                                                                           hdr.size = new_size;
                                                                           hdr.header_crc = 0;
                                                                           hdr.header_crc = header_crc(&hdr);
                                                                           memcpy(obj_block, &hdr, sizeof(hdr));
                                                                           if (lcfs_write_block(fd, block, obj_block) < 0) {
                                                                               errno = EIO;
                                                                               return -1;
                                                                           }
                                                                           return 0;
                                                                       }

                                                                       // Nunca debería llegar aquí
                                                                       errno = EINVAL;
                                                                       return -1;
                                                                   }

                                                                   /* ========================== Otras funciones ========================== */
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
                                                                       if (hdr.type != OBJ_TYPE_SYMLINK) {
                                                                           errno = EINVAL;
                                                                           return -1;
                                                                       }
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
                                                                           int saved_errno = errno;
                                                                           lcfs_delete_object(fd, oid);
                                                                           errno = saved_errno;
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

                                                                                   /* ========================== Reconstrucción ========================== */
                                                                                   int lcfs_rebuild_free_map(int fd, uint8_t **bitmap, uint64_t *bitmap_blocks) {
                                                                                       DEBUG_ENTER();
                                                                                       off_t dev_size = lseek(fd, 0, SEEK_END);
                                                                                       if (dev_size < 0) return -1;
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
                                                                                               for (uint32_t i = 0; i < hdr.num_extents; i++) {
                                                                                                   for (uint64_t j = 0; j < extents[i].block_count; j++) {
                                                                                                       uint64_t phys = extents[i].physical_block + j;
                                                                                                       if (phys < total_blocks) {
                                                                                                           byte_idx = phys / 8;
                                                                                                           bit = 1 << (phys % 8);
                                                                                                           bm[byte_idx] |= bit;
                                                                                                       }
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
                                                                                       if (dev_size < 0) return -1;
                                                                                       uint64_t total_blocks = dev_size / LCFS_BLOCK_SIZE;

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

                                                                                       uint8_t *data = NULL;
                                                                                       size_t len = 0;
                                                                                       for (uint64_t b = 0; b < total_blocks; b++) {
                                                                                           lcfs_obj_header hdr;
                                                                                           if (lcfs_read_header(fd, b, &hdr) == 0) {
                                                                                               uint8_t *tmp = realloc(data, len + 16);
                                                                                               if (!tmp) { free(data); return -1; }
                                                                                               data = tmp;
                                                                                               uint64_t *entries = (uint64_t*)data;
                                                                                               entries[len/16*2] = hdr.oid;
                                                                                               entries[len/16*2+1] = b;
                                                                                               len += 16;
                                                                                           }
                                                                                       }
                                                                                       if (write_oid_map_data(fd, data, len) < 0) { free(data); return -1; }
                                                                                       free(data);
                                                                                       DEBUG_EXIT(0);
                                                                                       return 0;
                                                                                   }
