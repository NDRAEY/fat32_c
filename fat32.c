#include "fat32.h"
#include "fat_utf16_utf8.h"
#include "lfn.h"
#include "vfs.h"

#include <stdint.h>
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

    fat->cluster_base = ((info->reserved_sectors + two_fats) - 2) * info->sectors_per_cluster * info->bytes_per_sector;
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
        printf("[DIR ] %s     @%d\n", filename, (entry->high_cluster << 16) | entry->low_cluster);
    } else {
        printf("[FILE] %s, Size: %u bytes  @%d\n", filename, entry->file_size, (entry->high_cluster << 16) | entry->low_cluster);
    }
}

direntry_t* read_directory(fat_t* fat, uint32_t start_cluster) {
    printf("Directory cluster: %d\n", start_cluster);

    uint32_t cluster_count = read_cluster_chain(fat, start_cluster, true, NULL);

    printf("Directory at cluster: %d uses %d clusters\n", start_cluster, cluster_count);

    char* cluster_data = calloc(cluster_count, fat->cluster_size);
    read_cluster_chain(fat, start_cluster, false, cluster_data);

    int32_t current_offset = cluster_count * fat->cluster_size - 32;   // We must start from the end.

    uint32_t cluster_size = fat->cluster_size;
    uint32_t offset = start_cluster * cluster_size;
    fseek(fat->image, offset, SEEK_SET);

    direntry_t* dir = calloc(1, sizeof(direntry_t));
    direntry_t* dirptr = dir;

    // HINT: Both LFN and directory entry are 32 bytes long.
    DirectoryEntry_t* prev = (DirectoryEntry_t*)(cluster_data + cluster_count);
    uint16_t in_name_buffer[256] = {0};
    size_t in_name_ptr = 0;
    char out_name_buffer[256] = {0};

    do {
        DirectoryEntry_t* entry = (DirectoryEntry_t*)(cluster_data + current_offset);

        if (entry->name[0] == 0x00) {
            goto next;
        }

        if ((uint8_t)entry->name[0] == 0xE5) {
            // Удалённый файл
            goto next;
        }

        if (entry->attributes & ATTR_LONG_FILE_NAME) {
            LFN_t* lfn = (LFN_t*)(cluster_data + current_offset);

            for(int p1 = 0; p1 < 5; p1++) {
                if(lfn->first_name_chunk[p1] == 0x0000) {
                    goto lfn_next;
                }

                in_name_buffer[in_name_ptr++] = lfn->first_name_chunk[p1];
            }

            for(int p2 = 0; p2 < 6; p2++) {
                if(lfn->second_name_chunk[p2] == 0x0000) {
                    goto lfn_next;
                }

                in_name_buffer[in_name_ptr++] = lfn->second_name_chunk[p2];
            }
           
            for(int p3 = 0; p3 < 2; p3++) {
                if(lfn->third_name_chunk[p3] == 0x0000) {
                    goto lfn_next;
                }

                in_name_buffer[in_name_ptr++] = lfn->third_name_chunk[p3];
            }

lfn_next:
            if(lfn->attr_number & 0x40) {
                utf16_to_utf8(in_name_buffer, in_name_ptr, out_name_buffer);

                printf("[%.8s] Long file name: %s\n", prev->name, out_name_buffer);
       
                dirptr->name = calloc(256, 1);
                memcpy(dirptr->name, out_name_buffer, 256);

                dirptr->type = (prev->attributes & ATTR_DIRECTORY) ? ENT_DIRECTORY : ENT_FILE;
                dirptr->size = prev->file_size;
                dirptr->priv_data = (void*)(size_t)((prev->high_cluster << 16) | prev->low_cluster);

                if(current_offset != 0) {
                    dirptr->next = calloc(1, sizeof(direntry_t));
                    dirptr = dirptr->next;
                }

                memset(in_name_buffer, 0, 512);
                memset(out_name_buffer, 0, 256);
                in_name_ptr = 0;
            }

            current_offset -= 32;
            continue;
        } 
       
        print_directory_entry(entry);

        if(!(current_offset - 32 >= 0 && ((entry - 1)->attributes & ATTR_LONG_FILE_NAME))) {
            dirptr->name = calloc(12, 1);
            memcpy(dirptr->name, entry->name, 8);
            memcpy(dirptr->name + 8, entry->ext, 3);

            dirptr->type = (entry->attributes & ATTR_DIRECTORY) ? ENT_DIRECTORY : ENT_FILE;
            dirptr->size = entry->file_size;
            dirptr->priv_data = (void*)(size_t)((entry->high_cluster << 16) | entry->low_cluster);

            if(current_offset != 0) {
                dirptr->next = calloc(1, sizeof(direntry_t));
                dirptr = dirptr->next;
            }
        }
next:
        current_offset -= 32;

        prev = entry;
    } while (current_offset >= 0); 

    free(cluster_data);

    return dir;
}

// Returns cluster count and reads cluster data.
size_t read_cluster_chain(fat_t* fat, uint32_t start_cluster, bool probe, void* out) {
    uint32_t cluster_count = 0;

    uint32_t cluster_size = fat->cluster_size;
    uint32_t cluster = start_cluster;
    while (cluster < 0x0FFFFFF8) {  // Проверка на последний кластер в цепочке
        if(!probe) {
            uint32_t offset = fat->cluster_base + (cluster * cluster_size);
            printf("Offset: %x\n", offset);
            fseek(fat->image, offset, SEEK_SET);

            fread(((char*)out) + (cluster_count * cluster_size), cluster_size, 1, fat->image);
        }
        
        // Перейти к следующему кластеру
        cluster = fat->fat_chain[cluster];
        cluster_count++;
    }

    return cluster_count;
}

// Returns count of read bytes!!!
size_t read_cluster_chain_advanced(fat_t* fat, uint32_t start_cluster, size_t byte_offset, size_t size, bool probe, void* out) {
    uint32_t cluster_size = fat->cluster_size;
    uint32_t cluster = start_cluster;
    size_t total_bytes_read = 0;
    size_t cluster_count = 0;
    
// Skip initial clusters based on byte_offset
    while (byte_offset >= cluster_size) {
        cluster = fat->fat_chain[cluster];
        byte_offset -= cluster_size;
        cluster_count++;
        
        if (cluster >= 0x0FFFFFF8) {
            return total_bytes_read;  // End of chain reached
        }
    }

    // Now we are positioned at the correct cluster and byte offset within it
    while (cluster < 0x0FFFFFF8 && total_bytes_read < size) {
        if (!probe) {
            uint32_t offset = fat->cluster_base + (cluster * cluster_size) + byte_offset;
            size_t bytes_to_read = cluster_size - byte_offset;
            
            // Ensure we don’t read more than requested
            if (bytes_to_read > size - total_bytes_read) {
                bytes_to_read = size - total_bytes_read;
            }

            fseek(fat->image, offset, SEEK_SET);
            fread(((char*)out) + total_bytes_read, bytes_to_read, 1, fat->image);

            total_bytes_read += bytes_to_read;
        }

        // Move to the next cluster in the chain
        cluster = fat->fat_chain[cluster];
        cluster_count++;
        byte_offset = 0;  // Only the first iteration needs a non-zero byte offset
    }

    return total_bytes_read;
}

void fast_traverse(direntry_t* dir) {
    do {
        printf("T: %d; Name: %s; Size: %zu; (-> %p) (priv: %u)\n", dir->type, dir->name, dir->size, dir->next, dir->priv_data);
        dir = dir->next;
    } while(dir);
}

size_t fat32_search_on_cluster(fat_t* fat, size_t cluster, const char* name) {
    uint32_t found_cluster = 0;

    direntry_t* entries = read_directory(fat, cluster);
    direntry_t* orig = entries;

    fast_traverse(entries);

    do {
        printf("1: '%s'; 2: '%s'; %d\n", entries->name, name, strcmp(entries->name, name));
        if(strcmp(entries->name, name) == 0) {
            found_cluster = (uint32_t)(size_t)entries->priv_data;

            printf("Found cluster: %d (%p)\n", found_cluster, entries->priv_data);
            break;
        }

        entries = entries->next;
    } while(entries);

    // FIXME: KLUDGE! Replace it with `dirclose` when port to Veonter (Other OS)
    do {
        direntry_t* k = orig;
        orig = orig->next;
        free(k);
    } while(orig);


    return found_cluster;
}

int main() {
    fat_t myfat;

    fat32_init("disk.img", &myfat);

    printf("Cluster size: %d\n", myfat.cluster_size);
    printf("Fat offset: %d\n", myfat.fat_offset);
    printf("Fat size: %d\n", myfat.fat_size);
    printf("Reserved FAT offset: %d\n", myfat.reserved_fat_offset);
    printf("Root directory offset: %d\n", myfat.root_directory_offset);
    printf("Root directory cluster: %d\n", myfat.fat->root_directory_offset_in_clusters);
    printf("Cluster base: %d\n", myfat.cluster_base);

    // Прочитать записи каталога в корневой директории
    direntry_t* dir = read_directory(&myfat, myfat.fat->root_directory_offset_in_clusters);
    direntry_t* orig = dir;

    fast_traverse(dir);

    // Пример: чтение данных файла (необходимо указать правильный кластер)
    // read_file_data(&myfat, 2);

    fat32_deinit(&myfat);
}
