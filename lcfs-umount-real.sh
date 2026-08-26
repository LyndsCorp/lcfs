#!/bin/bash
# Uso: ./lcfs-umount <punto_montaje>
if [ $# -ne 1 ]; then
    echo "Uso: $0 <punto_montaje>"
    exit 1
fi

MOUNTPOINT="$1"

# Desmontar FUSE
sudo fusermount -u "$MOUNTPOINT" 2>/dev/null || sudo umount "$MOUNTPOINT"

# Encontrar el loop asociado al punto de montaje
LOOP=$(losetup -j "$IMAGE" | cut -d: -f1)
if [ -n "$LOOP" ]; then
    sudo losetup -d "$LOOP"
    echo "Loop $LOOP liberado"
fi

# Eliminar directorio de montaje si quedó vacío
rmdir "$MOUNTPOINT" 2>/dev/null || true
