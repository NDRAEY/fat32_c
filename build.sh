#!/bin/bash
fallocate -l 128M disk.img

sudo mkfs.fat -F 32 disk.img

sudo mount disk.img /mnt/y

cd ../../../

sudo mkdir /mnt/y/sukablyat

cd ../../../mnt/y/

sudo bash -c "echo 'Aaaa' > aaa.txt"

sudo umount /mnt/y
