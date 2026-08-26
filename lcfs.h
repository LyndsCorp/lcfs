#ifndef LCFS_H
#define LCFS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#define LCFS_MAGIC "LCFSOBJ\x01"
#define LCFS_MAGIC_LEN 8
#define LCFS_BLOCK_SIZE 4096
#define LCFS_HEADER_SIZE 64
#define LCFS_VERSION 0x0200
#define LCFS_MAX_NAME_LEN 64
#define LCFS_MAX_INLINE_SIZE (LCFS_BLOCK_SIZE - LCFS_HEADER_SIZE - 2 - LCFS_MAX_NAME_LEN)
#define LCFS_ROOT_OID 3
#define LCFS_FREE_MAP_OID 1
#define LCFS_OID_MAP_OID 2
#define LCFS_SUPERBLOCK_OID 0

typedef uint64_t lcfs_oid_t;

enum lcfs_obj_type {
    OBJ_TYPE_FILE = 0x0001,
    OBJ_TYPE_DIR = 0x0002,
    OBJ_TYPE_SYMLINK = 0x0003,
    OBJ_TYPE_EXTENT_TABLE = 0x0004,
    OBJ_TYPE_META_EXTENT = 0x0005,
    OBJ_TYPE_FREE_MAP = 0x0007,
    OBJ_TYPE_OID_MAP = 0x0006,
    OBJ_TYPE_SUPERBLOCK = 0x0008,
};

#define OBJ_FLAG_DIRTY 0x01
#define OBJ_FLAG_SPARSE 0x02
#define OBJ_FLAG_HAS_DATA_CRC 0x04
#define OBJ_FLAG_DELETED 0x08

typedef struct {
    uint8_t  magic[8];
    uint64_t oid;
    uint16_t type;
    uint16_t version;
    uint32_t size;
    uint32_t num_extents;
    uint32_t header_crc;
    uint32_t flags;
    uint64_t parent_oid;
    uint64_t next_sibling_oid;
    uint64_t first_child_oid;
    uint32_t generation;
} __attribute__((packed)) lcfs_obj_header;

typedef struct {
    uint64_t child_oid;
    uint16_t child_type;
    uint16_t name_len;
} __attribute__((packed)) lcfs_dir_entry;

typedef struct {
    uint64_t logical_block;
    uint64_t physical_block;
    uint64_t block_count;
} __attribute__((packed)) lcfs_extent;

uint32_t lcfs_crc32c(uint32_t crc, const void *buf, size_t len);
int lcfs_read_header(int fd, uint64_t block_num, lcfs_obj_header *hdr);
int lcfs_write_header(int fd, uint64_t block_num, const lcfs_obj_header *hdr);
int lcfs_read_block(int fd, uint64_t block_num, void *buf);
int lcfs_write_block(int fd, uint64_t block_num, const void *buf);
int lcfs_read_object_name(int fd, uint64_t block_num, char *name, size_t max_len);
int lcfs_init_superblock(int fd, uint64_t total_blocks);
int lcfs_alloc_block(int fd, uint64_t *block_num);
int lcfs_free_block(int fd, uint64_t block_num);
int lcfs_get_free_map(int fd, uint8_t **bitmap, uint64_t *bitmap_blocks);
int lcfs_set_free_map(int fd, const uint8_t *bitmap, uint64_t bitmap_blocks);
int lcfs_create_object(int fd, uint16_t type, lcfs_oid_t parent_oid,
                       const char *name, lcfs_oid_t *new_oid, uint64_t *block_num);
int lcfs_delete_object(int fd, lcfs_oid_t oid);
int lcfs_lookup_name(int fd, lcfs_oid_t dir_oid, const char *name,
                     lcfs_oid_t *child_oid, uint16_t *child_type);
int lcfs_add_dir_entry(int fd, lcfs_oid_t dir_oid, lcfs_oid_t child_oid,
                       uint16_t child_type, const char *name);
int lcfs_remove_dir_entry(int fd, lcfs_oid_t dir_oid, const char *name);
int lcfs_read_file(int fd, lcfs_oid_t oid, char *buf, size_t size, off_t offset);
int lcfs_write_file(int fd, lcfs_oid_t oid, const char *buf, size_t size, off_t offset);
int lcfs_truncate_file(int fd, lcfs_oid_t oid, off_t new_size);
int lcfs_get_object_size(int fd, lcfs_oid_t oid, uint32_t *size);
int lcfs_readlink(int fd, lcfs_oid_t oid, char *buf, size_t bufsize);
int lcfs_object_location(int fd, lcfs_oid_t oid, uint64_t *block_num);
int lcfs_rebuild_free_map(int fd, uint8_t **bitmap, uint64_t *bitmap_blocks);
int lcfs_rebuild_oid_map(int fd);
int lcfs_oid_map_add(int fd, lcfs_oid_t oid, uint64_t block);
int lcfs_oid_map_remove(int fd, lcfs_oid_t oid);
int lcfs_create_dir(int fd, lcfs_oid_t parent_oid, const char *name, lcfs_oid_t *new_oid);
int lcfs_create_symlink(int fd, lcfs_oid_t parent_oid, const char *name, const char *target, lcfs_oid_t *new_oid);
int lcfs_rename(int fd, lcfs_oid_t old_parent, const char *old_name,
                lcfs_oid_t new_parent, const char *new_name);
int lcfs_unlink(int fd, lcfs_oid_t parent_oid, const char *name);
int lcfs_rmdir(int fd, lcfs_oid_t parent_oid, const char *name);
int lcfs_validate_header(const lcfs_obj_header *hdr);

#endif
