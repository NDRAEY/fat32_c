#include "fat32.h"
#include <stdlib.h>

void fat32_init(const char* filename, fat_t* fat) {
    FILE* file = fopen(filename, "rb");

    fat->image = file;


    FATInfo_t* info = calloc(1, sizeof(FATInfo_t));

    fread(info, sizeof(FATInfo_t), 1, file);

    fat->fat = info;


    fat->cluster_size = info->bytes_per_sector * info->sectors_per_cluster;
    fat->fat_offset = info->reserved_sectors * info->bytes_per_sector;
    fat->fat_size = info->fat_size_in_sectors * info->bytes_per_sector;
    fat->reserved_fat_offset = (info->reserved_sectors + info->fat_size_in_sectors) * info->bytes_per_sector;
    
    uint32_t two_fats = info->fat_size_in_sectors * 2;
    uint32_t tot_cluster = (info->reserved_sectors + two_fats) + ((info->root_directory_offset_in_clusters - 2) * info->sectors_per_cluster);

    fat->root_directory_offset = tot_cluster * info->bytes_per_sector;

    fat->fat_chain = calloc(fat->fat_size, 1);

    fseek(fat->image, fat->fat_offset, SEEK_SET);
    fread(fat->fat_chain, fat->fat_size, 1, file);
}

void fat32_deinit(fat_t* fat) {
    fclose(fat->image);

    free(fat->fat_chain);
    free(fat->fat);
}

int main() {
    fat_t myfat;

    fat32_init("disk.img", &myfat);

    printf("Cluster size: %d\n", myfat.cluster_size);
    printf("Fat offset: %d\n", myfat.fat_offset);
    printf("Fat size: %d\n", myfat.fat_size);
    printf("Reserved FAT offset: %d\n", myfat.reserved_fat_offset);
    printf("Root directory offset: %d\n", myfat.root_directory_offset);

    for(int i = 0; i < 1024; i++) {
        printf("%08x ", myfat.fat_chain[i]);
    }

    printf("\n");

    fat32_deinit(&myfat);
}