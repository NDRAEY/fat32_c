#include "fat32.h"
#include "lfn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void print_directory_entry(DirectoryEntry_t* entry) {
    char filename[13];
    snprintf(filename, sizeof(filename), "%.8s.%.3s", entry->name, entry->ext);
    if (entry->attributes & ATTR_DIRECTORY) {
        printf("[DIR ] %s\n", filename);
    } else {
        printf("[FILE] %s, Size: %u bytes\n", filename, entry->file_size);
    }
}

void read_directory(fat_t* fat, uint32_t start_cluster) {
    printf("Directory cluster: %d\n", start_cluster);

    uint32_t cluster_size = fat->cluster_size;
    uint32_t offset = start_cluster * cluster_size;
    fseek(fat->image, offset, SEEK_SET);

    DirectoryEntry_t entry;
    while (fread(&entry, sizeof(DirectoryEntry_t), 1, fat->image) != 0) {
        if (entry.name[0] == 0x00) {
            // Конец каталога
            break;
        }
        if (entry.name[0] == 0xE5) {
            // Удалённый файл
            continue;
        }

        if (entry.attributes & ATTR_LONG_FILE_NAME) {
            printf("^--- Has LFN\n");

            LFN_t lfn;

            fseek(fat->image, ftell(fat->image), SEEK_SET);
            
            fread(&lfn, sizeof(LFN_t), 1, fat->image);

            printf("Attr: %x\n", lfn.attr_number);
            // Обработка длинных имён файлов здесь, если необходимо
            // Например: read_long_file_name(fat, entry.start_cluster);
        }
        
        print_directory_entry(&entry);
    }
}

void read_file_data(fat_t* fat, uint32_t start_cluster) {
    uint32_t cluster_size = fat->cluster_size;
    uint32_t cluster = start_cluster;
    while (cluster < 0x0FFFFFF8) {  // Проверка на последний кластер в цепочке
        uint32_t offset = cluster * cluster_size;
        fseek(fat->image, offset, SEEK_SET);
        uint8_t buffer[cluster_size];
        fread(buffer, cluster_size, 1, fat->image);

        // Обработка данных файла (например, сохранить в файл или вывести на экран)
        // fwrite(buffer, 1, cluster_size, output_file);

        // Перейти к следующему кластеру
        cluster = fat->fat_chain[cluster];
    }
}

int main() {
    fat_t myfat;

    fat32_init("disk.img", &myfat);

    printf("Cluster size: %d\n", myfat.cluster_size);
    printf("Fat offset: %d\n", myfat.fat_offset);
    printf("Fat size: %d\n", myfat.fat_size);
    printf("Reserved FAT offset: %d\n", myfat.reserved_fat_offset);
    printf("Root directory offset: %d\n", myfat.root_directory_offset);

    // Прочитать записи каталога в корневой директории
    read_directory(&myfat, myfat.root_directory_offset / myfat.cluster_size);

    // Пример: чтение данных файла (необходимо указать правильный кластер)
    // read_file_data(&myfat, 2);

    fat32_deinit(&myfat);
}
