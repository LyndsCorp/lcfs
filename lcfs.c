#include "lcfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

// CRC32C table
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

// Header CRC computation
static uint32_t header_crc(const lcfs_obj_header *hdr) {
    lcfs_obj_header tmp = *hdr;
    tmp.header_crc = 0;
    return lcfs_crc32c(0, &tmp, sizeof(tmp));
}

// Block I/O
int lcfs_read_block(int fd, uint64_t block_num, void *buf) {
    off_t off = (off_t)block_num * LCFS_BLOCK_SIZE;
    if (lseek(fd, off, SEEK_SET) < 0) return -1;
    ssize_t ret = read(fd, buf, LCFS_BLOCK_SIZE);
    if (ret != LCFS_BLOCK_SIZE) return -1;
    return 0;
}

int lcfs_write_block(int fd, uint64_t block_num, const void *buf) {
    off_t off = (off_t)block_num * LCFS_BLOCK_SIZE;
    if (lseek(fd, off, SEEK_SET) < 0) return -1;
    ssize_t ret = write(fd, buf, LCFS_BLOCK_SIZE);
    if (ret != LCFS_BLOCK_SIZE) return -1;
    return 0;
}

// Header I/O
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
    return lcfs_write_block(fd, block_num, block);
}

// OID generation: use superblock counter if available, else random
lcfs_oid_t lcfs_generate_oid(int fd) {
    lcfs_obj_header sb;
    if (lcfs_read_header(fd, LCFS_SUPERBLOCK_OID, &sb) == 0 && sb.type == OBJ_TYPE_SUPERBLOCK) {
        // Store next_oid in sb.size? In our mkfs, we used sb.size for root oid. We'll define a proper superblock struct later.
        // For now, we'll use a global counter file? No, keep simple: use random for now.
    }
    // Fallback: random 64-bit
    uint64_t oid;
    // Use /dev/urandom or rand()? For simplicity, use time + rand
    static int seeded = 0;
    if (!seeded) {
        srand(time(NULL) ^ getpid());
        seeded = 1;
    }
    oid = ((uint64_t)rand() << 32) ^ rand();
    // Ensure not 0 or reserved
    if (oid < 16) oid += 16;
    return oid;
}

// Find object by OID using OID map or scan
int lcfs_find_object(int fd, lcfs_oid_t oid, uint64_t *block_num) {
    // First try OID map (if exists)
    lcfs_obj_header map_hdr;
    if (lcfs_read_header(fd, LCFS_OID_MAP_OID, &map_hdr) == 0 && map_hdr.type == OBJ_TYPE_OID_MAP) {
        // OID map is stored as a hash table in its data blocks; we can implement later.
        // For now, we skip and do full scan.
    }
    // Full scan: read all blocks, check header
    off_t end = lseek(fd, 0, SEEK_END);
    if (end < 0) return -1;
    uint64_t total_blocks = end / LCFS_BLOCK_SIZE;
    lcfs_obj_header hdr;
    for (uint64_t i = 0; i < total_blocks; i++) {
        if (lcfs_read_header(fd, i, &hdr) == 0) {
            if (hdr.oid == oid) {
                if (block_num) *block_num = i;
                return 0;
            }
        }
    }
    errno = ENOENT;
    return -1;
}

// Update OID map (simple append to a list; not optimized)
int lcfs_update_oid_map(int fd, lcfs_oid_t oid, uint64_t block_num) {
    // In this minimal implementation, we skip; OID map will be rebuilt by recover.
    return 0; // success but no-op
}

// Free map functions (simplified: read/write bitmap object)
int lcfs_get_free_map(int fd, uint8_t **bitmap, uint64_t *bitmap_blocks) {
    lcfs_obj_header hdr;
    if (lcfs_read_header(fd, LCFS_FREE_MAP_OID, &hdr) < 0) return -1;
    if (hdr.type != OBJ_TYPE_FREE_MAP) return -1;
    uint64_t bm_blocks = (hdr.size + LCFS_BLOCK_SIZE - 1) / LCFS_BLOCK_SIZE;
    uint8_t *bm = malloc(bm_blocks * LCFS_BLOCK_SIZE);
    if (!bm) return -1;
    // Data blocks start at block after header block? In our design, free map object's data is stored in extents? Simpler: free map object occupies contiguous blocks from its header block+1.
    uint64_t start = LCFS_FREE_MAP_OID + 1; // assumption: header block is LCFS_FREE_MAP_OID (block 1)
    for (uint64_t i = 0; i < bm_blocks; i++) {
        if (lcfs_read_block(fd, start + i, bm + i*LCFS_BLOCK_SIZE) < 0) {
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
    if (lcfs_write_header(fd, LCFS_FREE_MAP_OID, &hdr) < 0) return -1;
    uint64_t start = LCFS_FREE_MAP_OID + 1;
    for (uint64_t i = 0; i < bitmap_blocks; i++) {
        if (lcfs_write_block(fd, start + i, bitmap + i*LCFS_BLOCK_SIZE) < 0) return -1;
    }
    return 0;
}

int lcfs_alloc_block(int fd, uint64_t *block_num) {
    uint8_t *bitmap;
    uint64_t bm_blocks;
    if (lcfs_get_free_map(fd, &bitmap, &bm_blocks) < 0) {
        // No free map, try to rebuild it
        if (lcfs_rebuild_free_map(fd) < 0) return -1;
        if (lcfs_get_free_map(fd, &bitmap, &bm_blocks) < 0) return -1;
    }
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
    if (lcfs_get_free_map(fd, &bitmap, &bm_blocks) < 0) {
        // Try rebuild
        if (lcfs_rebuild_free_map(fd) < 0) return -1;
        if (lcfs_get_free_map(fd, &bitmap, &bm_blocks) < 0) return -1;
    }
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

// Create object: allocate a block, assign OID, write header with name
int lcfs_create_object(int fd, uint16_t type, lcfs_oid_t parent_oid,
                       const char *name, lcfs_oid_t *new_oid, uint64_t *block_num) {
    uint64_t blk;
    if (lcfs_alloc_block(fd, &blk) < 0) return -1;
    lcfs_oid_t oid = lcfs_generate_oid(fd);
    lcfs_obj_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, LCFS_MAGIC, LCFS_MAGIC_LEN);
    hdr.oid = oid;
    hdr.type = type;
    hdr.version = LCFS_VERSION;
    hdr.parent_oid = parent_oid;
    hdr.generation = 1;
    hdr.header_crc = 0;
    hdr.header_crc = header_crc(&hdr);
    uint8_t block[LCFS_BLOCK_SIZE] = {0};
    memcpy(block, &hdr, sizeof(hdr));
    // Write name length and name at offset 64
    size_t name_len = strlen(name);
    if (name_len > LCFS_MAX_NAME_LEN) name_len = LCFS_MAX_NAME_LEN;
    uint16_t nlen = (uint16_t)name_len;
    memcpy(block + LCFS_HEADER_SIZE, &nlen, 2);
    memcpy(block + LCFS_HEADER_SIZE + 2, name, name_len);
    if (lcfs_write_block(fd, blk, block) < 0) {
        lcfs_free_block(fd, blk);
        return -1;
    }
    if (new_oid) *new_oid = oid;
    if (block_num) *block_num = blk;
    // Update OID map (optional)
    lcfs_update_oid_map(fd, oid, blk);
    return 0;
                       }

                       // Delete object: find its block, free all extents, free block
                       int lcfs_delete_object(int fd, lcfs_oid_t oid) {
                           uint64_t blk;
                           if (lcfs_find_object(fd, oid, &blk) < 0) return -1;
                           lcfs_obj_header hdr;
                           if (lcfs_read_header(fd, blk, &hdr) < 0) return -1;
                           // Free extents if file/symlink with external data
                           if (hdr.type == OBJ_TYPE_FILE || hdr.type == OBJ_TYPE_SYMLINK) {
                               // Read extents stored inline or in extent table
                               uint8_t block[LCFS_BLOCK_SIZE];
                               lcfs_read_block(fd, blk, block);
                               size_t name_len;
                               memcpy(&name_len, block + LCFS_HEADER_SIZE, 2);
                               size_t pos = LCFS_HEADER_SIZE + 2 + name_len;
                               if (hdr.num_extents > 0) {
                                   // Extents are stored inline if they fit, else in extent table
                                   // For simplicity, assume inline extents
                                   lcfs_extent *extents = (lcfs_extent *)(block + pos);
                                   for (uint32_t i = 0; i < hdr.num_extents; i++) {
                                       lcfs_free_block(fd, extents[i].physical_block);
                                   }
                               }
                           }
                           // Free the header block
                           lcfs_free_block(fd, blk);
                           // Remove from parent directory
                           if (hdr.parent_oid != 0) {
                               // We need parent's block; find it and remove entry
                               uint64_t parent_blk;
                               if (lcfs_find_object(fd, hdr.parent_oid, &parent_blk) == 0) {
                                   // Read parent directory, find entry, remove (set child_oid=0)
                                   uint8_t pblock[LCFS_BLOCK_SIZE];
                                   lcfs_read_block(fd, parent_blk, pblock);
                                   uint16_t pname_len;
                                   memcpy(&pname_len, pblock + LCFS_HEADER_SIZE, 2);
                                   size_t ppos = LCFS_HEADER_SIZE + 2 + pname_len;
                                   while (ppos + sizeof(lcfs_dir_entry) <= LCFS_BLOCK_SIZE) {
                                       lcfs_dir_entry entry;
                                       memcpy(&entry, pblock + ppos, sizeof(entry));
                                       if (entry.child_oid == 0) break;
                                       if (entry.child_oid == oid) {
                                           // Zero out entry
                                           memset(pblock + ppos, 0, sizeof(entry) + entry.name_len);
                                           // Rewrite block
                                           lcfs_write_block(fd, parent_blk, pblock);
                                           break;
                                       }
                                       ppos += sizeof(entry) + entry.name_len;
                                   }
                               }
                           }
                           return 0;
                       }

                       // Directory operations

                       int lcfs_lookup_name(int fd, lcfs_oid_t dir_oid, const char *name,
                                            lcfs_oid_t *child_oid, uint16_t *child_type) {
                           uint64_t dir_blk;
                           if (lcfs_find_object(fd, dir_oid, &dir_blk) < 0) return -1;
                           lcfs_obj_header hdr;
                           if (lcfs_read_header(fd, dir_blk, &hdr) < 0) return -1;
                           if (hdr.type != OBJ_TYPE_DIR) return -1;
                           uint8_t block[LCFS_BLOCK_SIZE];
                           lcfs_read_block(fd, dir_blk, block);
                           uint16_t name_len;
                           memcpy(&name_len, block + LCFS_HEADER_SIZE, 2);
                           size_t pos = LCFS_HEADER_SIZE + 2 + name_len;
                           while (pos + sizeof(lcfs_dir_entry) <= LCFS_BLOCK_SIZE) {
                               lcfs_dir_entry entry;
                               memcpy(&entry, block + pos, sizeof(entry));
                               if (entry.child_oid == 0) break;
                               char entry_name[LCFS_MAX_NAME_LEN+1];
                               memcpy(entry_name, block + pos + sizeof(entry), entry.name_len);
                               entry_name[entry.name_len] = '\0';
                               if (strcmp(entry_name, name) == 0) {
                                   if (child_oid) *child_oid = entry.child_oid;
                                   if (child_type) *child_type = entry.child_type;
                                   return 0;
                               }
                               pos += sizeof(entry) + entry.name_len;
                           }
                           errno = ENOENT;
                           return -1;
                                            }

                                            int lcfs_add_dir_entry(int fd, lcfs_oid_t dir_oid, lcfs_oid_t child_oid,
                                                                   uint16_t child_type, const char *name) {
                                                uint64_t dir_blk;
                                                if (lcfs_find_object(fd, dir_oid, &dir_blk) < 0) return -1;
                                                lcfs_obj_header hdr;
                                                if (lcfs_read_header(fd, dir_blk, &hdr) < 0) return -1;
                                                uint8_t block[LCFS_BLOCK_SIZE];
                                                lcfs_read_block(fd, dir_blk, block);
                                                uint16_t dname_len;
                                                memcpy(&dname_len, block + LCFS_HEADER_SIZE, 2);
                                                size_t pos = LCFS_HEADER_SIZE + 2 + dname_len;
                                                while (pos + sizeof(lcfs_dir_entry) <= LCFS_BLOCK_SIZE) {
                                                    lcfs_dir_entry entry;
                                                    memcpy(&entry, block + pos, sizeof(entry));
                                                    if (entry.child_oid == 0) {
                                                        // Found free slot
                                                        entry.child_oid = child_oid;
                                                        entry.child_type = child_type;
                                                        entry.name_len = strlen(name);
                                                        if (entry.name_len > LCFS_MAX_NAME_LEN) entry.name_len = LCFS_MAX_NAME_LEN;
                                                        memcpy(block + pos, &entry, sizeof(entry));
                                                        memcpy(block + pos + sizeof(entry), name, entry.name_len);
                                                        // Update header size (number of entries)
                                                        hdr.size++;
                                                        hdr.header_crc = 0;
                                                        hdr.header_crc = header_crc(&hdr);
                                                        memcpy(block, &hdr, sizeof(hdr));
                                                        lcfs_write_block(fd, dir_blk, block);
                                                        return 0;
                                                    }
                                                    pos += sizeof(entry) + entry.name_len;
                                                }
                                                errno = ENOSPC;
                                                return -1;
                                                                   }

                                                                   int lcfs_remove_dir_entry(int fd, lcfs_oid_t dir_oid, const char *name) {
                                                                       // Similar to lookup and zero out entry
                                                                       uint64_t dir_blk;
                                                                       if (lcfs_find_object(fd, dir_oid, &dir_blk) < 0) return -1;
                                                                       lcfs_obj_header hdr;
                                                                       if (lcfs_read_header(fd, dir_blk, &hdr) < 0) return -1;
                                                                       uint8_t block[LCFS_BLOCK_SIZE];
                                                                       lcfs_read_block(fd, dir_blk, block);
                                                                       uint16_t dname_len;
                                                                       memcpy(&dname_len, block + LCFS_HEADER_SIZE, 2);
                                                                       size_t pos = LCFS_HEADER_SIZE + 2 + dname_len;
                                                                       while (pos + sizeof(lcfs_dir_entry) <= LCFS_BLOCK_SIZE) {
                                                                           lcfs_dir_entry entry;
                                                                           memcpy(&entry, block + pos, sizeof(entry));
                                                                           if (entry.child_oid == 0) break;
                                                                           char entry_name[LCFS_MAX_NAME_LEN+1];
                                                                           memcpy(entry_name, block + pos + sizeof(entry), entry.name_len);
                                                                           entry_name[entry.name_len] = '\0';
                                                                           if (strcmp(entry_name, name) == 0) {
                                                                               // Zero out entry
                                                                               memset(block + pos, 0, sizeof(entry) + entry.name_len);
                                                                               hdr.size--;
                                                                               hdr.header_crc = 0;
                                                                               hdr.header_crc = header_crc(&hdr);
                                                                               memcpy(block, &hdr, sizeof(hdr));
                                                                               lcfs_write_block(fd, dir_blk, block);
                                                                               return 0;
                                                                           }
                                                                           pos += sizeof(entry) + entry.name_len;
                                                                       }
                                                                       errno = ENOENT;
                                                                       return -1;
                                                                   }

                                                                   // File operations

                                                                   int lcfs_get_object_size(int fd, lcfs_oid_t oid, uint32_t *size) {
                                                                       uint64_t blk;
                                                                       if (lcfs_find_object(fd, oid, &blk) < 0) return -1;
                                                                       lcfs_obj_header hdr;
                                                                       if (lcfs_read_header(fd, blk, &hdr) < 0) return -1;
                                                                       *size = hdr.size;
                                                                       return 0;
                                                                   }

                                                                   int lcfs_read_file(int fd, lcfs_oid_t oid, char *buf, size_t size, off_t offset) {
                                                                       uint64_t blk;
                                                                       if (lcfs_find_object(fd, oid, &blk) < 0) return -1;
                                                                       lcfs_obj_header hdr;
                                                                       if (lcfs_read_header(fd, blk, &hdr) < 0) return -1;
                                                                       if (hdr.type != OBJ_TYPE_FILE) return -1;
                                                                       uint8_t block[LCFS_BLOCK_SIZE];
                                                                       lcfs_read_block(fd, blk, block);
                                                                       uint16_t name_len;
                                                                       memcpy(&name_len, block + LCFS_HEADER_SIZE, 2);
                                                                       size_t data_offset = LCFS_HEADER_SIZE + 2 + name_len;
                                                                       if (hdr.num_extents == 0) {
                                                                           // Inline data
                                                                           if (offset + size > hdr.size) size = hdr.size - offset;
                                                                           memcpy(buf, block + data_offset + offset, size);
                                                                           return size;
                                                                       } else {
                                                                           // Extents: read from physical blocks
                                                                           size_t bytes_read = 0;
                                                                           uint32_t logical_block = offset / LCFS_BLOCK_SIZE;
                                                                           uint32_t offset_in_block = offset % LCFS_BLOCK_SIZE;
                                                                           // For simplicity, assume extents are stored inline at data_offset
                                                                           lcfs_extent *extents = (lcfs_extent *)(block + data_offset);
                                                                           while (size > 0 && logical_block < hdr.num_extents) {
                                                                               uint64_t phys = extents[logical_block].physical_block;
                                                                               uint8_t data_block[LCFS_BLOCK_SIZE];
                                                                               lcfs_read_block(fd, phys, data_block);
                                                                               size_t to_copy = LCFS_BLOCK_SIZE - offset_in_block;
                                                                               if (to_copy > size) to_copy = size;
                                                                               memcpy(buf + bytes_read, data_block + offset_in_block, to_copy);
                                                                               bytes_read += to_copy;
                                                                               size -= to_copy;
                                                                               offset_in_block = 0;
                                                                               logical_block++;
                                                                           }
                                                                           return bytes_read;
                                                                       }
                                                                   }

                                                                   int lcfs_write_file(int fd, lcfs_oid_t oid, const char *buf, size_t size, off_t offset) {
                                                                       // For simplicity, only support writing inline if fits, else return ENOSPC
                                                                       uint64_t blk;
                                                                       if (lcfs_find_object(fd, oid, &blk) < 0) return -1;
                                                                       lcfs_obj_header hdr;
                                                                       if (lcfs_read_header(fd, blk, &hdr) < 0) return -1;
                                                                       if (hdr.type != OBJ_TYPE_FILE) return -1;
                                                                       uint8_t block[LCFS_BLOCK_SIZE];
                                                                       lcfs_read_block(fd, blk, block);
                                                                       uint16_t name_len;
                                                                       memcpy(&name_len, block + LCFS_HEADER_SIZE, 2);
                                                                       size_t data_offset = LCFS_HEADER_SIZE + 2 + name_len;
                                                                       size_t max_inline = LCFS_BLOCK_SIZE - data_offset;
                                                                       if (offset + size <= max_inline && hdr.num_extents == 0) {
                                                                           // Write inline
                                                                           memcpy(block + data_offset + offset, buf, size);
                                                                           if (offset + size > hdr.size) hdr.size = offset + size;
                                                                           hdr.header_crc = 0;
                                                                           hdr.header_crc = header_crc(&hdr);
                                                                           memcpy(block, &hdr, sizeof(hdr));
                                                                           lcfs_write_block(fd, blk, block);
                                                                           return size;
                                                                       }
                                                                       errno = ENOSPC;
                                                                       return -1;
                                                                   }

                                                                   int lcfs_readlink(int fd, lcfs_oid_t oid, char *buf, size_t bufsize) {
                                                                       uint64_t blk;
                                                                       if (lcfs_find_object(fd, oid, &blk) < 0) return -1;
                                                                       lcfs_obj_header hdr;
                                                                       if (lcfs_read_header(fd, blk, &hdr) < 0) return -1;
                                                                       if (hdr.type != OBJ_TYPE_SYMLINK) return -1;
                                                                       uint8_t block[LCFS_BLOCK_SIZE];
                                                                       lcfs_read_block(fd, blk, block);
                                                                       uint16_t name_len;
                                                                       memcpy(&name_len, block + LCFS_HEADER_SIZE, 2);
                                                                       size_t data_offset = LCFS_HEADER_SIZE + 2 + name_len;
                                                                       // Symlink target is stored as inline data
                                                                       size_t len = hdr.size;
                                                                       if (len >= bufsize) len = bufsize - 1;
                                                                       memcpy(buf, block + data_offset, len);
                                                                       buf[len] = '\0';
                                                                       return 0;
                                                                   }

                                                                   // Recovery: scan all blocks and rebuild structures
                                                                   int lcfs_scan_objects(int fd, lcfs_oid_t **oids, uint64_t **blocks, int *count) {
                                                                       off_t end = lseek(fd, 0, SEEK_END);
                                                                       if (end < 0) return -1;
                                                                       uint64_t total_blocks = end / LCFS_BLOCK_SIZE;
                                                                       lcfs_oid_t *oid_list = malloc(sizeof(lcfs_oid_t) * total_blocks);
                                                                       uint64_t *block_list = malloc(sizeof(uint64_t) * total_blocks);
                                                                       if (!oid_list || !block_list) {
                                                                           free(oid_list);
                                                                           free(block_list);
                                                                           return -1;
                                                                       }
                                                                       int cnt = 0;
                                                                       lcfs_obj_header hdr;
                                                                       for (uint64_t i = 0; i < total_blocks; i++) {
                                                                           if (lcfs_read_header(fd, i, &hdr) == 0) {
                                                                               oid_list[cnt] = hdr.oid;
                                                                               block_list[cnt] = i;
                                                                               cnt++;
                                                                           }
                                                                       }
                                                                       *oids = oid_list;
                                                                       *blocks = block_list;
                                                                       *count = cnt;
                                                                       return 0;
                                                                   }

                                                                   int lcfs_rebuild_free_map(int fd) {
                                                                       // Scan all objects and mark occupied blocks
                                                                       off_t end = lseek(fd, 0, SEEK_END);
                                                                       if (end < 0) return -1;
                                                                       uint64_t total_blocks = end / LCFS_BLOCK_SIZE;
                                                                       uint64_t bm_blocks = (total_blocks + LCFS_BLOCK_SIZE*8 - 1) / (LCFS_BLOCK_SIZE*8);
                                                                       uint8_t *bitmap = calloc(bm_blocks, LCFS_BLOCK_SIZE);
                                                                       if (!bitmap) return -1;
                                                                       // Mark all blocks as free initially (already zeroed)
                                                                       // Iterate over all blocks
                                                                       lcfs_obj_header hdr;
                                                                       for (uint64_t i = 0; i < total_blocks; i++) {
                                                                           if (lcfs_read_header(fd, i, &hdr) == 0) {
                                                                               // Mark header block occupied
                                                                               bitmap[i/8] |= 1 << (i%8);
                                                                               // If object has extents, mark those blocks
                                                                               if (hdr.type == OBJ_TYPE_FILE || hdr.type == OBJ_TYPE_SYMLINK) {
                                                                                   uint8_t block[LCFS_BLOCK_SIZE];
                                                                                   lcfs_read_block(fd, i, block);
                                                                                   uint16_t name_len;
                                                                                   memcpy(&name_len, block + LCFS_HEADER_SIZE, 2);
                                                                                   size_t data_offset = LCFS_HEADER_SIZE + 2 + name_len;
                                                                                   if (hdr.num_extents > 0) {
                                                                                       lcfs_extent *extents = (lcfs_extent *)(block + data_offset);
                                                                                       for (uint32_t e = 0; e < hdr.num_extents; e++) {
                                                                                           uint64_t phys = extents[e].physical_block;
                                                                                           bitmap[phys/8] |= 1 << (phys%8);
                                                                                       }
                                                                                   }
                                                                               }
                                                                               // For directories, we don't have extents in this simple version
                                                                           }
                                                                       }
                                                                       // Write free map object (OID=1) and data blocks
                                                                       lcfs_obj_header fmh;
                                                                       memset(&fmh, 0, sizeof(fmh));
                                                                       memcpy(fmh.magic, LCFS_MAGIC, LCFS_MAGIC_LEN);
                                                                       fmh.oid = LCFS_FREE_MAP_OID;
                                                                       fmh.type = OBJ_TYPE_FREE_MAP;
                                                                       fmh.version = LCFS_VERSION;
                                                                       fmh.size = bm_blocks * LCFS_BLOCK_SIZE;
                                                                       fmh.num_extents = bm_blocks;
                                                                       fmh.header_crc = 0;
                                                                       fmh.header_crc = header_crc(&fmh);
                                                                       if (lcfs_write_header(fd, LCFS_FREE_MAP_OID, &fmh) < 0) {
                                                                           free(bitmap);
                                                                           return -1;
                                                                       }
                                                                       // Write bitmap data starting at block 2 (since block 1 is header, block 2 onwards data)
                                                                       uint64_t start = LCFS_FREE_MAP_OID + 1; // block 2
                                                                       for (uint64_t i = 0; i < bm_blocks; i++) {
                                                                           if (lcfs_write_block(fd, start + i, bitmap + i*LCFS_BLOCK_SIZE) < 0) {
                                                                               free(bitmap);
                                                                               return -1;
                                                                           }
                                                                       }
                                                                       free(bitmap);
                                                                       return 0;
                                                                   }

                                                                   int lcfs_rebuild_oid_map(int fd) {
                                                                       // Not implemented: for simplicity, we skip OID map. It can be rebuilt on demand by scanning.
                                                                       return 0;
                                                                   }
