CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2
FUSE_CFLAGS = $(shell pkg-config --cflags fuse3)
FUSE_LIBS = $(shell pkg-config --libs fuse3)
LDFLAGS = $(FUSE_LIBS)

# Si se invoca make DEBUG=1, se activa -DDEBUG
ifeq ($(DEBUG),1)
    CFLAGS += -DDEBUG -O0 -g
endif

TARGETS = lcfs-mkfs lcfs-mount-ro lcfs-mount-rw lcfs-recover lcfs-info

all: $(TARGETS)

lcfs-mkfs: lcfs_mkfs.c lcfs.c lcfs.h debug.h
	$(CC) $(CFLAGS) -o $@ lcfs_mkfs.c lcfs.c

lcfs-mount-ro: lcfs_mount_ro.c lcfs.c lcfs.h debug.h
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -o $@ lcfs_mount_ro.c lcfs.c $(FUSE_LIBS)

lcfs-mount-rw: lcfs_mount_rw.c lcfs.c lcfs.h debug.h
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -o $@ lcfs_mount_rw.c lcfs.c $(FUSE_LIBS)

lcfs-recover: lcfs_recover.c lcfs.c lcfs.h debug.h
	$(CC) $(CFLAGS) -o $@ lcfs_recover.c lcfs.c

lcfs-info: lcfs_info.c lcfs.c lcfs.h debug.h
	$(CC) $(CFLAGS) -o $@ lcfs_info.c lcfs.c

clean:
	rm -f $(TARGETS)

.PHONY: all clean
