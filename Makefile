CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2
LDFLAGS = -lfuse3

TARGETS = lcfs-mkfs lcfs-mount lcfs-recover lcfs-info

all: $(TARGETS)

lcfs-mkfs: lcfs_mkfs.c lcfs.c lcfs.h
	$(CC) $(CFLAGS) -o $@ lcfs_mkfs.c lcfs.c

lcfs-mount: lcfs_mount.c lcfs.c lcfs.h
	$(CC) $(CFLAGS) -o $@ lcfs_mount.c lcfs.c $(LDFLAGS)

lcfs-recover: lcfs_recover.c lcfs.c lcfs.h
	$(CC) $(CFLAGS) -o $@ lcfs_recover.c lcfs.c

lcfs-info: lcfs_info.c lcfs.c lcfs.h
	$(CC) $(CFLAGS) -o $@ lcfs_info.c lcfs.c

clean:
	rm -f $(TARGETS)
