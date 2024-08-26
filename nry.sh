#!/bin/bash

set -e

rm disk.img

fallocate -l 128M disk.img && sudo mkfs.fat -F 32 disk.img
make && ./fat32
sudo mount disk.img /mnt && ls /mnt -lh && sudo umount /mnt
