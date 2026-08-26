#!/bin/bash
# Uso: ./lcfs-mount <imagen_lcfs> <punto_montaje> [--chmod XXX] [opciones_fuse]
set -e

if [ $# -lt 2 ]; then
    echo "Uso: $0 <imagen_lcfs> <punto_montaje> [--chmod XXX] [opciones_fuse]"
    exit 1
fi

IMAGE="$1"
MOUNTPOINT="$2"
shift 2

# Buscar un loop device libre
LOOP=$(losetup -f)
if [ -z "$LOOP" ]; then
    echo "No hay loop devices disponibles"
    exit 1
fi

# Asociar la imagen al loop
sudo losetup "$LOOP" "$IMAGE"

# Crear punto de montaje si no existe
mkdir -p "$MOUNTPOINT"

# Montar con FUSE sobre el dispositivo loop
# Se usa lcfs-mount-rw o lcfs-mount-ro según se prefiera
if [[ "$*" == *"--ro"* ]]; then
    sudo ./lcfs-mount-ro "$@" "$LOOP" "$MOUNTPOINT"
else
    sudo ./lcfs-mount-rw "$@" "$LOOP" "$MOUNTPOINT"
fi

echo "Montado $IMAGE en $MOUNTPOINT (loop: $LOOP)"
