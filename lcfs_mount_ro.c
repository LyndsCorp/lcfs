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
#include <limits.h>
#include "lcfs.h"

static int lcfs_fd;
static mode_t default_mode = 0755;

static void split_path(const char *path, char *dir, char *name) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        strcpy(dir, "/");
        strcpy(name, path);
        return;
    }
    if (slash == path) {
        strcpy(dir, "/");
        strcpy(name, slash + 1);
    } else {
        size_t len = slash - path;
        strncpy(dir, path, len);
        dir[len] = '\0';
        strcpy(name, slash + 1);
    }
}

static int resolve_path(const char *path, lcfs_oid_t *oid, uint16_t *type) {
    if (strcmp(path, "/") == 0) {
        *oid = LCFS_ROOT_OID;
        *type = OBJ_TYPE_DIR;
        return 0;
    }

    char *copy = strdup(path);
    if (!copy) return -ENOMEM;
    char *saveptr = NULL;
    char *component = strtok_r(copy, "/", &saveptr);
    lcfs_oid_t current_oid = LCFS_ROOT_OID;
    uint16_t current_type = OBJ_TYPE_DIR;

    while (component != NULL) {
        lcfs_oid_t child_oid;
        uint16_t child_type;
        if (lcfs_lookup_name(lcfs_fd, current_oid, component, &child_oid, &child_type) < 0) {
            free(copy);
            return -ENOENT;
        }
        current_oid = child_oid;
        current_type = child_type;
        component = strtok_r(NULL, "/", &saveptr);
        if (component != NULL && current_type != OBJ_TYPE_DIR) {
            free(copy);
            return -ENOTDIR;
        }
    }
    free(copy);
    *oid = current_oid;
    *type = current_type;
    return 0;
}

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

    lcfs_oid_t oid;
    uint16_t type;
    if (resolve_path(path, &oid, &type) == 0) {
        if (type == OBJ_TYPE_DIR) {
            stbuf->st_mode = S_IFDIR | default_mode;
            stbuf->st_nlink = 2;
        } else if (type == OBJ_TYPE_FILE) {
            stbuf->st_mode = S_IFREG | default_mode;
            stbuf->st_nlink = 1;
            uint32_t size;
            lcfs_get_object_size(lcfs_fd, oid, &size);
            stbuf->st_size = size;
        } else if (type == OBJ_TYPE_SYMLINK) {
            stbuf->st_mode = S_IFLNK | 0777;
            stbuf->st_nlink = 1;
            char target[1024];
            lcfs_readlink(lcfs_fd, oid, target, sizeof(target));
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

                            lcfs_oid_t dir_oid;
                            uint16_t dir_type;
                            if (resolve_path(path, &dir_oid, &dir_type) < 0)
                                return -ENOENT;
                            if (dir_type != OBJ_TYPE_DIR)
                                return -ENOTDIR;

                            filler(buf, ".", NULL, 0, 0);
                            filler(buf, "..", NULL, 0, 0);

                            uint64_t dir_block;
                            if (lcfs_object_location(lcfs_fd, dir_oid, &dir_block) < 0)
                                return -EIO;

                            uint8_t block[LCFS_BLOCK_SIZE];
                            if (lcfs_read_block(lcfs_fd, dir_block, block) < 0)
                                return -EIO;

                            uint16_t dname_len;
                            memcpy(&dname_len, block + LCFS_HEADER_SIZE, 2);
                            size_t pos = LCFS_HEADER_SIZE + 2 + dname_len;

                            while (pos + sizeof(lcfs_dir_entry) <= LCFS_BLOCK_SIZE) {
                                lcfs_dir_entry entry;
                                memcpy(&entry, block + pos, sizeof(entry));

                                if (entry.child_oid == 0)
                                    break;

                                // Validar name_len para no salirnos del bloque
                                if (entry.name_len == 0 || entry.name_len > LCFS_MAX_NAME_LEN ||
                                    pos + sizeof(lcfs_dir_entry) + entry.name_len > LCFS_BLOCK_SIZE) {
                                    // Entrada corrupta, terminamos el listado
                                    break;
                                    }

                                    pos += sizeof(lcfs_dir_entry);
                                char name[LCFS_MAX_NAME_LEN + 1];
                                memcpy(name, block + pos, entry.name_len);
                                name[entry.name_len] = '\0';
                                pos += entry.name_len;

                                filler(buf, name, NULL, 0, 0);
                            }

                            return 0;
                                                }

                                                static int lcfs_open(const char *path, struct fuse_file_info *fi) {
                                                    (void)fi;
                                                    lcfs_oid_t oid;
                                                    uint16_t type;
                                                    if (resolve_path(path, &oid, &type) < 0)
                                                        return -ENOENT;
                                                    if (type != OBJ_TYPE_FILE)
                                                        return -EISDIR;
                                                    return 0;
                                                }

                                                static int lcfs_read(const char *path, char *buf, size_t size, off_t offset,
                                                                     struct fuse_file_info *fi) {
                                                    (void)fi;
                                                    lcfs_oid_t oid;
                                                    uint16_t type;
                                                    if (resolve_path(path, &oid, &type) < 0)
                                                        return -ENOENT;
                                                    if (type != OBJ_TYPE_FILE)
                                                        return -EISDIR;
                                                    return lcfs_read_file(lcfs_fd, oid, buf, size, offset);
                                                                     }

                                                                     static int lcfs_fuse_readlink(const char *path, char *buf, size_t size) {
                                                                         lcfs_oid_t oid;
                                                                         uint16_t type;
                                                                         if (resolve_path(path, &oid, &type) < 0)
                                                                             return -ENOENT;
                                                                         if (type != OBJ_TYPE_SYMLINK)
                                                                             return -EINVAL;
                                                                         return lcfs_readlink(lcfs_fd, oid, buf, size);
                                                                     }

                                                                     static int lcfs_statfs(const char *path, struct statvfs *stbuf) {
                                                                         (void)path;
                                                                         off_t dev_size = lseek(lcfs_fd, 0, SEEK_END);
                                                                         if (dev_size < 0) return -errno;
                                                                         uint64_t total_blocks = dev_size / LCFS_BLOCK_SIZE;

                                                                         uint8_t *bitmap = NULL;
                                                                         uint64_t bm_blocks = 0;
                                                                         if (lcfs_get_free_map(lcfs_fd, &bitmap, &bm_blocks) < 0) {
                                                                             stbuf->f_blocks = total_blocks;
                                                                             stbuf->f_bfree = 0;
                                                                             stbuf->f_bavail = 0;
                                                                         } else {
                                                                             uint64_t free_blocks = 0;
                                                                             for (uint64_t i = 0; i < total_blocks; i++) {
                                                                                 uint64_t byte_idx = i / 8;
                                                                                 uint8_t bit = 1 << (i % 8);
                                                                                 if (!(bitmap[byte_idx] & bit)) free_blocks++;
                                                                             }
                                                                             free(bitmap);
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
                                                                         .getattr    = lcfs_getattr,
                                                                         .readdir    = lcfs_readdir,
                                                                         .open       = lcfs_open,
                                                                         .read       = lcfs_read,
                                                                         .readlink   = lcfs_fuse_readlink,
                                                                         .statfs     = lcfs_statfs,
                                                                     };

                                                                     int main(int argc, char *argv[]) {
                                                                         int i = 1;
                                                                         while (i < argc) {
                                                                             if (strcmp(argv[i], "--chmod") == 0 && i + 1 < argc) {
                                                                                 default_mode = strtol(argv[i + 1], NULL, 8) & 0777;
                                                                                 for (int j = i; j < argc - 2; j++) {
                                                                                     argv[j] = argv[j + 2];
                                                                                 }
                                                                                 argc -= 2;
                                                                             } else {
                                                                                 i++;
                                                                             }
                                                                         }

                                                                         if (argc < 3) {
                                                                             fprintf(stderr, "Uso: %s <imagen_lcfs> <punto_montaje> [opciones_fuse]\n", argv[0]);
                                                                             return 1;
                                                                         }
                                                                         const char *image = argv[1];
                                                                         const char *mountpoint = argv[2];

                                                                         lcfs_fd = open(image, O_RDONLY);
                                                                         if (lcfs_fd < 0) {
                                                                             perror("open");
                                                                             return 1;
                                                                         }

                                                                         int fuse_argc = 0;
                                                                         char *fuse_argv[argc + 5];
                                                                         fuse_argv[fuse_argc++] = argv[0];
                                                                         fuse_argv[fuse_argc++] = "-o";
                                                                         fuse_argv[fuse_argc++] = "fsname=lcfs,subtype=lcfs";

                                                                         for (int j = 3; j < argc; j++) {
                                                                             fuse_argv[fuse_argc++] = argv[j];
                                                                         }
                                                                         fuse_argv[fuse_argc++] = (char *)mountpoint;
                                                                         fuse_argv[fuse_argc] = NULL;

                                                                         return fuse_main(fuse_argc, fuse_argv, &lcfs_oper, NULL);
                                                                     }
