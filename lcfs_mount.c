#define FUSE_USE_VERSION 31
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include "lcfs.h"

static int lcfs_fd;

static int lcfs_getattr(const char *path, struct stat *stbuf) {
    memset(stbuf, 0, sizeof(struct stat));
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }
    lcfs_oid_t root_oid = LCFS_ROOT_OID;
    // Buscar el objeto por path
    // Simplificación: solo soportamos rutas de un nivel
    const char *name = path + 1;
    lcfs_oid_t child_oid;
    uint16_t child_type;
    if (lcfs_lookup_name(lcfs_fd, root_oid, name, &child_oid, &child_type) == 0) {
        if (child_type == OBJ_TYPE_DIR) {
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
        } else if (child_type == OBJ_TYPE_FILE) {
            stbuf->st_mode = S_IFREG | 0644;
            stbuf->st_nlink = 1;
            uint32_t size;
            if (lcfs_get_object_size(lcfs_fd, child_oid, &size) == 0) {
                stbuf->st_size = size;
            }
        } else if (child_type == OBJ_TYPE_SYMLINK) {
            stbuf->st_mode = S_IFLNK | 0777;
            stbuf->st_nlink = 1;
            char target[1024];
            if (lcfs_readlink(lcfs_fd, child_oid, target, sizeof(target)) == 0) {
                stbuf->st_size = strlen(target);
            }
        }
        return 0;
    }
    return -ENOENT;
}

static int lcfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi) {
    if (strcmp(path, "/") != 0) return -ENOENT;
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    // Listar hijos del root
    uint64_t root_block;
    if (lcfs_object_location(lcfs_fd, LCFS_ROOT_OID, &root_block) < 0) return -EIO;
    lcfs_obj_header hdr;
    if (lcfs_read_header(lcfs_fd, root_block, &hdr) < 0) return -EIO;
    uint8_t block[LCFS_BLOCK_SIZE];
    if (lcfs_read_block(lcfs_fd, root_block, block) < 0) return -EIO;
    uint16_t dname_len;
    memcpy(&dname_len, block + LCFS_HEADER_SIZE, 2);
    size_t pos = LCFS_HEADER_SIZE + 2 + dname_len;
    while (pos + sizeof(lcfs_dir_entry) <= LCFS_BLOCK_SIZE) {
        lcfs_dir_entry entry;
        memcpy(&entry, block + pos, sizeof(entry));
        if (entry.child_oid == 0) break;
        pos += sizeof(entry);
        char name[LCFS_MAX_NAME_LEN+1];
        memcpy(name, block + pos, entry.name_len);
        name[entry.name_len] = '\0';
        pos += entry.name_len;
        filler(buf, name, NULL, 0);
    }
    return 0;
                        }

                        static int lcfs_open(const char *path, struct fuse_file_info *fi) {
                            return 0;
                        }

                        static int lcfs_read(const char *path, char *buf, size_t size, off_t offset,
                                             struct fuse_file_info *fi) {
                            lcfs_oid_t root_oid = LCFS_ROOT_OID;
                            const char *name = path + 1;
                            lcfs_oid_t child_oid;
                            uint16_t child_type;
                            if (lcfs_lookup_name(lcfs_fd, root_oid, name, &child_oid, &child_type) == 0) {
                                if (child_type == OBJ_TYPE_FILE) {
                                    return lcfs_read_file(lcfs_fd, child_oid, buf, size, offset);
                                }
                            }
                            return -ENOENT;
                                             }

                                             static int lcfs_readlink(const char *path, char *buf, size_t size) {
                                                 lcfs_oid_t root_oid = LCFS_ROOT_OID;
                                                 const char *name = path + 1;
                                                 lcfs_oid_t child_oid;
                                                 uint16_t child_type;
                                                 if (lcfs_lookup_name(lcfs_fd, root_oid, name, &child_oid, &child_type) == 0) {
                                                     if (child_type == OBJ_TYPE_SYMLINK) {
                                                         return lcfs_readlink(lcfs_fd, child_oid, buf, size);
                                                     }
                                                 }
                                                 return -ENOENT;
                                             }

                                             static struct fuse_operations lcfs_oper = {
                                                 .getattr = lcfs_getattr,
                                                 .readdir = lcfs_readdir,
                                                 .open = lcfs_open,
                                                 .read = lcfs_read,
                                                 .readlink = lcfs_readlink,
                                             };

                                             int main(int argc, char *argv[]) {
                                                 if (argc < 3) {
                                                     fprintf(stderr, "Uso: %s <imagen_lcfs> <punto_montaje> [opciones_fuse]\n", argv[0]);
                                                     return 1;
                                                 }
                                                 const char *image = argv[1];
                                                 lcfs_fd = open(image, O_RDONLY);
                                                 if (lcfs_fd < 0) {
                                                     perror("open");
                                                     return 1;
                                                 }
                                                 // Ajustar argumentos para FUSE
                                                 argv[1] = argv[2];
                                                 argc--;
                                                 return fuse_main(argc, argv, &lcfs_oper, NULL);
                                             }
