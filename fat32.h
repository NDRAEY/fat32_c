#pragma once

#include <stdint.h>
#include <stdio.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE 0x20
#define ATTR_LONG_FILE_NAME 0x0F

typedef struct {
    char bootcode[3];
    char OEM[8];
    u16  bytes_per_sector;
    u8   sectors_per_cluster;
    u16  reserved_sectors;
    u8   copies;
    u16  root_entries;
    u16  small_sectors_number;
    u8   descriptor;
    u16  sectors_per_fat;
    u16  sectors_per_track;
    u16  heads;
    u32  hidden_sectors;
    u32  sectors_in_partition;
    u32  fat_size_in_sectors;
    u16  flags;
    u16  version_num;
    u32  root_directory_offset_in_clusters;
    u16  fsinfo_sector;
    u16  _;
    char reserved1[12];
    u8   disk_number;
    u8   flags1;
    u8   extended_boot_signature;
    u32  volume_serial_number;
    char volume_label[11];
    char fs_type[8];
    char bootcode_next[];
} __attribute__((packed)) FATInfo_t;

typedef struct {
    FILE* image;
    FATInfo_t* fat;

    uint32_t* fat_chain;

    uint32_t cluster_size;
    uint32_t fat_offset;
    uint32_t fat_size;
    uint32_t reserved_fat_offset;
    uint32_t root_directory_offset;
} fat_t;

typedef struct {
    char name[8];
    char ext[3];
    uint8_t attributes;
    uint8_t reserved;
    uint8_t creation_time_tenths;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access_date;
    uint16_t high_cluster;
    uint16_t modification_time;
    uint16_t modification_date;
    uint16_t low_cluster;
    uint32_t file_size;
} __attribute__((packed)) DirectoryEntry_t;

void read_directory(fat_t* fat, uint32_t start_cluster);
void read_file_data(fat_t* fat, uint32_t start_cluster);
