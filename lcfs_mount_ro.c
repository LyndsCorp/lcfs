#define FUSE_USE_VERSION 31
#define _POSIX_C_SOURCE 200809L

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include "lcfs.h"

static int lcfs_fd;
static mode_t default_mode = 0755;

static int lcfs_getattr(const char *path, struct stat *stbuf,
                        struct fuse_file_info *fi) {
    (void)fi;
    memset(stbuf, 0, sizeof(struct stat));
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_atime = time(NULL);
    stbuf->st_mtime = time(NULL);
    stbuf->st_ctime = time(NULL);

    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | default_mode;
        stbuf->st_nlink = 2;
        return 0;
    }
    lcfs_oid_t root_oid = LCFS_ROOT_OID;
    const char *name = path + 1;
    lcfs_oid_t child_oid;
    uint16_t child_type;
    if (lcfs_lookup_name(lcfs_fd, root_oid, name, &child_oid, &child_type) == 0) {
        if (child_type == OBJ_TYPE_DIR) {
            stbuf->st_mode = S_IFDIR | default_mode;
            stbuf->st_nlink = 2;
        } else if (child_type == OBJ_TYPE_FILE) {
            stbuf->st_mode = S_IFREG | default_mode;
            stbuf->st_nlink = 1;
            uint32_t size;
            lcfs_get_object_size(lcfs_fd, child_oid, &size);
            stbuf->st_size = size;
        } else if (child_type == OBJ_TYPE_SYMLINK) {
            stbuf->st_mode = S_IFLNK | 0777;
            stbuf->st_nlink = 1;
            char target[1024];
            lcfs_readlink(lcfs_fd, child_oid, target, sizeof(target));
            stbuf->st_size = strlen(target);
        }
        return 0;
    }
    return -ENOENT;
                        }

                        static int lcfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                                                off_t offset, struct fuse_file_info *fi,
                                                enum fuse_readdir_flags flags) {
                            (void)offset;
                            (void)fi;
                            (void)flags;
                            if (strcmp(path, "/") != 0) return -ENOENT;
                            filler(buf, ".", NULL, 0, 0);
                            filler(buf, "..", NULL, 0, 0);
                            uint64_t root_block;
                            if (lcfs_object_location(lcfs_fd, LCFS_ROOT_OID, &root_block) < 0) return -EIO;
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
                                filler(buf, name, NULL, 0, 0);
                            }
                            return 0;
                                                }

                                                static int lcfs_open(const char *path, struct fuse_file_info *fi) {
                                                    (void)path;
                                                    (void)fi;
                                                    return 0;
                                                }

                                                static int lcfs_read(const char *path, char *buf, size_t size, off_t offset,
                                                                     struct fuse_file_info *fi) {
                                                    (void)fi;
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

                                                                     static int lcfs_fuse_readlink(const char *path, char *buf, size_t size) {
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

                                                                     static int lcfs_statfs(const char *path, struct statvfs *stbuf) {
                                                                         (void)path;
                                                                         off_t dev_size = lseek(lcfs_fd, 0, SEEK_END);
                                                                         if (dev_size < 0) return -errno;
                                                                         uint64_t total_blocks = dev_size / LCFS_BLOCK_SIZE;

                                                                         uint8_t *bitmap = NULL;
                                                                         uint64_t bm_blocks = 0;
                                                                         if (lcfs_get_free_map(lcfs_fd, &bitmap, &bm_blocks) < 0) {
                                                                             // Si falla, asumir todo ocupado
                                                                             stbuf->f_blocks = total_blocks;
                                                                             stbuf->f_bfree = 0;
                                                                             stbuf->f_bavail = 0;
                                                                         } else {
                                                                             uint64_t free_blocks = 0;
                                                                             // Contar solo hasta total_blocks, no hasta total_bits
                                                                             for (uint64_t i = 0; i < total_blocks; i++) {
                                                                                 uint64_t byte_idx = i / 8;
                                                                                 uint8_t bit = 1 << (i % 8);
                                                                                 if (!(bitmap[byte_idx] & bit)) free_blocks++;
                                                                             }
                                                                             free(bitmap);
                                                                             // Por seguridad, acotar
                                                                             if (free_blocks > total_blocks) free_blocks = total_blocks;
                                                                             stbuf->f_blocks = total_blocks;
                                                                             stbuf->f_bfree = free_blocks;
                                                                             stbuf->f_bavail = free_blocks;
                                                                         }

                                                                         stbuf->f_bsize = LCFS_BLOCK_SIZE;
                                                                         stbuf->f_frsize = LCFS_BLOCK_SIZE;
                                                                         stbuf->f_files = 0;
                                                                         stbuf->f_ffree = 0;
                                                                         stbuf->f_favail = 0;
                                                                         stbuf->f_fsid = 0;
                                                                         stbuf->f_flag = 0;
                                                                         stbuf->f_namemax = LCFS_MAX_NAME_LEN;
                                                                         return 0;
                                                                     }

                                                                     static struct fuse_operations lcfs_oper = {
                                                                         .getattr = lcfs_getattr,
                                                                         .readdir = lcfs_readdir,
                                                                         .open = lcfs_open,
                                                                         .read = lcfs_read,
                                                                         .readlink = lcfs_fuse_readlink,
                                                                         .statfs = lcfs_statfs,
                                                                     };

                                                                     int main(int argc, char *argv[]) {
                                                                         for (int i = 1; i < argc; i++) {
                                                                             if (strcmp(argv[i], "--chmod") == 0 && i+1 < argc) {
                                                                                 default_mode = strtol(argv[i+1], NULL, 8) & 0777;
                                                                                 for (int j = i; j < argc-2; j++) {
                                                                                     argv[j] = argv[j+2];
                                                                                 }
                                                                                 argc -= 2;
                                                                                 i--;
                                                                             }
                                                                         }
                                                                         char *new_argv[argc + 4];
                                                                         new_argv[0] = argv[0];
                                                                         new_argv[1] = "-o";
                                                                         new_argv[2] = "fsname=lcfs,subtype=lcfs";
                                                                         for (int i = 1; i < argc; i++) {
                                                                             new_argv[i+2] = argv[i];
                                                                         }
                                                                         argc += 2;
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
                                                                         return fuse_main(argc, new_argv, &lcfs_oper, NULL);
                                                                     }
