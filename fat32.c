#include "fat32.h"
#include "fat_utf16_utf8.h"
#include "lfn.h"
#include "vfs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fat32_init(const char* filename, fat_t* fat) {
    FILE* file = fopen(filename, "r+b");
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
    uint32_t cluster_count = read_cluster_chain(fat, start_cluster, true, NULL);

    char* cluster_data = calloc(cluster_count, fat->cluster_size);
    read_cluster_chain(fat, start_cluster, false, cluster_data);

    int32_t current_offset = cluster_count * fat->cluster_size - 32;   // We must start from the end.

    uint32_t cluster_size = fat->cluster_size;
    uint32_t offset = start_cluster * cluster_size;
    fseek(fat->image, offset, SEEK_SET);

    direntry_t* dir = calloc(1, sizeof(direntry_t));
    direntry_t* dirptr = dir;

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
        
        cluster = fat->fat_chain[cluster];
        cluster_count++;
    }

    return cluster_count;
}

size_t read_cluster_chain_advanced(fat_t* fat, uint32_t start_cluster, size_t byte_offset, size_t size, bool probe, void* out) {
    uint32_t cluster_size = fat->cluster_size;
    uint32_t cluster = start_cluster;
    size_t total_bytes_read = 0;
    size_t cluster_count = 0;
    
    while (byte_offset >= cluster_size) {
        cluster = fat->fat_chain[cluster];
        byte_offset -= cluster_size;
        cluster_count++;
        
        if (cluster >= 0x0FFFFFF8) {
            return total_bytes_read;  // End of chain reached
        }
    }

    while (cluster < 0x0FFFFFF8 && total_bytes_read < size) {
        if (!probe) {
            uint32_t offset = fat->cluster_base + (cluster * cluster_size) + byte_offset;
            size_t bytes_to_read = cluster_size - byte_offset;
            
            if (bytes_to_read > size - total_bytes_read) {
                bytes_to_read = size - total_bytes_read;
            }

            fseek(fat->image, offset, SEEK_SET);
            fread(((char*)out) + total_bytes_read, bytes_to_read, 1, fat->image);

            total_bytes_read += bytes_to_read;
        }

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

    /* FIXME: KLUDGE! Replace it with `dirclose` when port to Veonter (Other OS) */
    do {
        direntry_t* k = orig;
        orig = orig->next;
        free(k);
    } while(orig);

    return found_cluster;
}

size_t fat32_search(fat_t* fat, const char* path) {
    printf("Searching: %s\n", path);

    size_t cluster = fat->fat->root_directory_offset_in_clusters;

    char temp_name[256] = {0};

    while (*path != '\0') {
        while (*path == '/') {
            path++;
        }

        if (*path == '\0') {
            break; // Reached end of path
        }

        const char* next_slash = path;
        while (*next_slash != '/' && *next_slash != '\0') {
            next_slash++;
        }

        size_t name_length = next_slash - path;
        if (name_length >= sizeof(temp_name)) {
            return 0;
        }

        strncpy(temp_name, path, name_length);
        temp_name[name_length] = '\0';

        cluster = fat32_search_on_cluster(fat, cluster, temp_name);
        if (cluster == 0) {
            return 0;
        }

        path = next_slash;
    }

    return cluster;
}

size_t fat32_get_file_size(fat_t* fat, const char* filename) {
    size_t len = strlen(filename);
    const char* end = filename + len;

    while(*end != '/') {
        end--;
    }

    char path[256] = {0};
    char e_filename[256] = {0};

    strncpy(path, filename, end - filename);

    printf("Path: %s\n", path);

    strncpy(e_filename, end + 1, len - (end - filename));

    printf("Filename: %s\n", e_filename);

    
    size_t clust = fat32_search(fat, path);

    if(clust == 0) {
        return clust;
    }

    direntry_t* entries = read_directory(fat, clust);
    direntry_t* orig = entries;

    size_t sz = 0;

    do {
        printf("-> %s\n", entries->name);
        
        if(strcmp(entries->name, e_filename) == 0) {
            sz = entries->size;
            break;
        }
        
        entries = entries->next;
    } while(entries);


    return sz;
}


size_t fat32_find_free_cluster(fat_t* fat) {
    for(int i = 0, sz = fat->fat_size / sizeof(uint32_t); i < sz; i++) {
        if(fat->fat_chain[i] == 0) {
            return i;
        }
    }

    return 0;
}

void fat32_find_free_entry(fat_t* fat, size_t dir_cluster, size_t* out_cluster_number, size_t* out_offset) {
    size_t cluster_count = read_cluster_chain(fat, dir_cluster, true, NULL);

    char* cluster_data = calloc(cluster_count, fat->cluster_size);
    read_cluster_chain(fat, dir_cluster, false, cluster_data);

    size_t entry_size = 32;  // Directory entry size is 32 bytes
    size_t total_entries = (cluster_count * fat->cluster_size) / entry_size;

    for (size_t i = 0; i < total_entries; i++) {
        char* entry = cluster_data + (i * entry_size);

        if (entry[0] == 0x00 || entry[0] == 0xE5) {
            size_t cluster_index = i / (fat->cluster_size / entry_size);
            size_t offset_within_cluster = (i % (fat->cluster_size / entry_size)) * entry_size;

            *out_cluster_number = dir_cluster;
            *out_offset = offset_within_cluster;

            free(cluster_data);
            return;
        }
    }

    *out_cluster_number = 0;
    *out_offset = 0;
    free(cluster_data);
}

size_t fat32_get_last_cluster_in_chain(fat_t* fat, size_t start_cluster) {
    if (start_cluster < 2 || start_cluster >= 0x0FFFFFF8) {
        return 0;  // Invalid start cluster
    }

    size_t current_cluster = start_cluster;

    while (current_cluster < 0x0FFFFFF8) {
        size_t next_cluster = fat->fat_chain[current_cluster];

        if (next_cluster >= 0x0FFFFFF8) {
            break;  // Reached the end of the chain
        }

        current_cluster = next_cluster;
    }

    return current_cluster;
}

void fat32_allocate_cluster(fat_t* fat, size_t for_cluster) {
    size_t last_cluster = fat32_get_last_cluster_in_chain(fat, for_cluster);

    if (last_cluster == 0) {
        return;
    }

    size_t new_cluster = fat32_find_free_cluster(fat);

    if (new_cluster == 0) {
        return;
    }

    fat->fat_chain[last_cluster] = new_cluster;

    fat->fat_chain[new_cluster] = 0x0FFFFFF8;

    uint32_t cluster_size = fat->cluster_size;
    uint32_t offset = fat->cluster_base + (new_cluster * cluster_size);
    fseek(fat->image, offset, SEEK_SET);
    char* zero_buffer = calloc(1, cluster_size);
    fwrite(zero_buffer, cluster_size, 1, fat->image);
    free(zero_buffer);
}

void fat32_flush(fat_t* f) {
    fseek(f->image, f->fat_offset, SEEK_SET);
    fwrite(f->fat_chain, 1, f->fat_size, f->image);
}

size_t fat32_create_file(fat_t* fat, size_t dir_cluster, const char* filename, bool is_file) {
    if (filename == NULL || strlen(filename) == 0 || strlen(filename) > 255) {
        return 0;
    }

    size_t out_cluster_number = 0;
    size_t out_offset = 0;    // PIKA PIKA 
    fat32_find_free_entry(fat, dir_cluster, &out_cluster_number, &out_offset);

    printf("%d %d\n", out_cluster_number, out_offset);

    if (out_cluster_number == 0) {
        return 0;
    }

    size_t new_cluster = fat32_find_free_cluster(fat);
    if (new_cluster == 0) {
        return 0;
    }

    printf("Cluster: %d\n", new_cluster);

    fat->fat_chain[new_cluster] = 0x0FFFFFF8;

    char sfn[12] = {0};  // 8.3 format (8 chars + '.' + 3 chars)
    LFN2SFN(filename, sfn);

    printf("SFN: %.11s\n", sfn);

    DirectoryEntry_t entry = {0};
    memset(&entry, 0, sizeof(DirectoryEntry_t));
    memcpy(entry.name, sfn, 8);
    memcpy(entry.ext, sfn + 8, 3);
    entry.attributes = is_file ? 0x20 : 0x10; // 0x20 for files, 0x10 for directories
    entry.high_cluster = (new_cluster >> 16) & 0xFFFF;
    entry.low_cluster = new_cluster & 0xFFFF;
    entry.file_size = 0; // Directories have size 0

    if(!is_file) {
        DirectoryEntry_t entry = {0};
        memset(entry.name, ' ', 8);
        memset(entry.ext, ' ', 3);

        entry.attributes |= ATTR_DIRECTORY;

        entry.name[0] = '.';

        size_t off = fat->cluster_base + (new_cluster * fat->cluster_size);

        fseek(fat->image, off, SEEK_SET);
        fwrite(&entry, sizeof(DirectoryEntry_t), 1, fat->image);

        entry.name[1] = '.';
        fwrite(&entry, sizeof(DirectoryEntry_t), 1, fat->image);
    }

    uint32_t cluster_size = fat->cluster_size;
    uint32_t entry_offset = fat->cluster_base + (out_cluster_number * cluster_size) + out_offset;
    
    unsigned short utf16_name[256] = {0};
    utf8_to_utf16(filename, utf16_name);
    
    size_t lfn_entry_count = (strlen(filename) + 12) / 13;
    
    entry_offset += lfn_entry_count * 32;

    for (size_t i = 0; i < lfn_entry_count; i++) {
        LFN_t lfn_entry = {0};
        lfn_entry.attribute = ATTR_LONG_FILE_NAME;
        lfn_entry.checksum = lfn_checksum(sfn);

        printf("LFN!\n");

        lfn_entry.attr_number = (i == lfn_entry_count - 1) ? 0x40 : 0x00; // Set LAST_LONG_ENTRY flag for the last entry
        lfn_entry.attr_number |= (uint8_t)(i + 1); // Set the sequence number

        size_t char_index = i * 13;
        for (int p1 = 0; p1 < 5 && char_index < strlen(filename); p1++) {
            lfn_entry.first_name_chunk[p1] = utf16_name[char_index++];
        }
        for (int p2 = 0; p2 < 6 && char_index < strlen(filename); p2++) {
            lfn_entry.second_name_chunk[p2] = utf16_name[char_index++];
        }
        for (int p3 = 0; p3 < 2 && char_index < strlen(filename); p3++) {
            lfn_entry.third_name_chunk[p3] = utf16_name[char_index++];
        }

        entry_offset -= 32; // Move to the position for the LFN entry
        fseek(fat->image, entry_offset, SEEK_SET);
        fwrite(&lfn_entry, sizeof(LFN_t), 1, fat->image);
    }

    entry_offset += lfn_entry_count * 32;
    
    fseek(fat->image, entry_offset, SEEK_SET);
    fwrite(&entry, sizeof(DirectoryEntry_t), 1, fat->image);

    fat32_flush(fat);

    return new_cluster;
}

size_t fat32_write_experimental(fat_t* fat, size_t start_cluster, size_t file_size, size_t offset, size_t size, size_t* out_file_size, const char* buffer) {
    size_t bytes_written = 0;
    size_t cluster_size = fat->cluster_size;
    size_t current_cluster = start_cluster;

    // Calculate the offset within the first cluster
    size_t cluster_offset = offset % cluster_size;

    // Calculate the number of clusters needed for the write
    size_t initial_cluster_offset = offset / cluster_size;
    size_t end_offset = offset + size;
    size_t total_clusters_needed = (end_offset + cluster_size - 1) / cluster_size;

    // Traverse to the correct starting cluster based on the initial offset
    for (size_t i = 0; i < initial_cluster_offset; i++) {
        current_cluster = fat->fat_chain[current_cluster];
        if (current_cluster >= 0x0FFFFFF8) {
            // Reached the end of the chain unexpectedly
            current_cluster = 0;
            break;
        }
    }

    if (current_cluster == 0) {
        // Handle file extension if needed
        current_cluster = fat32_find_free_cluster(fat);
        if (current_cluster == 0) {
            // No free clusters available, cannot proceed
            *out_file_size = file_size;
            return 0;
        }
        fat->fat_chain[start_cluster] = current_cluster;
    }

    // Start writing data
    size_t buffer_offset = 0;
    while (buffer_offset < size) {
        // Calculate the number of bytes to write in the current cluster
        size_t write_size = cluster_size - cluster_offset;
        if (write_size > size - buffer_offset) {
            write_size = size - buffer_offset;
        }

        // Calculate the offset in the FAT image
        size_t write_offset = fat->cluster_base + (current_cluster * cluster_size) + cluster_offset;

        // Write the data
        fseek(fat->image, write_offset, SEEK_SET);
        fwrite(buffer + buffer_offset, 1, write_size, fat->image);

        // Update tracking variables
        buffer_offset += write_size;
        bytes_written += write_size;
        cluster_offset = 0; // Only the first cluster might have an initial offset

        // Move to the next cluster if needed
        if (buffer_offset < size) {
            size_t next_cluster = fat->fat_chain[current_cluster];
            if (next_cluster >= 0x0FFFFFF8) {
                // Allocate a new cluster if needed
                next_cluster = fat32_find_free_cluster(fat);
                if (next_cluster == 0) {
                    // No more clusters available
                    break;
                }
                fat->fat_chain[current_cluster] = next_cluster;
                fat->fat_chain[next_cluster] = 0x0FFFFFF8; // Mark new cluster as end of the chain
            }
            current_cluster = next_cluster;
        }
    }

    // Update the file size if it has grown
    *out_file_size = offset + bytes_written > file_size ? offset + bytes_written : file_size;

    fat32_flush(fat);

    return bytes_written;
}

void fat32_get_file_info_coords(fat_t* fat, uint32_t dir_cluster, const char* filename, size_t* out_cluster, size_t* out_offset) {
    uint32_t cluster_size = fat->cluster_size;
    uint32_t current_cluster = dir_cluster;
    size_t current_offset = 0;

    // Buffer to store the reconstructed LFN
    uint16_t in_name_buffer[256] = {0};
    size_t in_name_ptr = 0;
    char out_name_buffer[256] = {0};

    while (current_cluster < 0x0FFFFFF8) {
        char* cluster_data = calloc(1, cluster_size);
        read_cluster_chain(fat, current_cluster, false, cluster_data);

        for (size_t offset = 0; offset < cluster_size; offset += 32) {
            DirectoryEntry_t* entry = (DirectoryEntry_t*)(cluster_data + offset);

            if (entry->name[0] == 0x00) {
                // End of directory
                free(cluster_data);
                return;
            }

            if ((uint8_t)entry->name[0] == 0xE5) {
                // Deleted entry, skip
                continue;
            }

            if (entry->attributes & ATTR_LONG_FILE_NAME) {
                // Handle LFN entry
                LFN_t* lfn = (LFN_t*)(cluster_data + offset);
                in_name_ptr = 0; // Reset the name pointer

                for (int i = 0; i < 5; i++) {
                    if (lfn->first_name_chunk[i] == 0x0000) break;
                    in_name_buffer[in_name_ptr++] = lfn->first_name_chunk[i];
                }

                for (int i = 0; i < 6; i++) {
                    if (lfn->second_name_chunk[i] == 0x0000) break;
                    in_name_buffer[in_name_ptr++] = lfn->second_name_chunk[i];
                }

                for (int i = 0; i < 2; i++) {
                    if (lfn->third_name_chunk[i] == 0x0000) break;
                    in_name_buffer[in_name_ptr++] = lfn->third_name_chunk[i];
                }

                // If this is the last LFN part (indicated by bit 6 of the sequence number), process it
                if (lfn->attr_number & 0x40) {
                    utf16_to_utf8(in_name_buffer, in_name_ptr, out_name_buffer);

                    // Compare the reconstructed LFN with the target filename
                    if (strcmp(out_name_buffer, filename) == 0) {
                        offset += (lfn->attr_number & ~0x40) * 32;

                        bool ow = offset / fat->cluster_size;
                        
                        if(ow) {
                            current_cluster++;
                            offset %= fat->cluster_size;
                        }

                        *out_cluster = current_cluster;
                        *out_offset = offset;
                        free(cluster_data);
                        return;
                    }

                    // Reset the buffers for the next file entry
                    memset(in_name_buffer, 0, sizeof(in_name_buffer));
                    memset(out_name_buffer, 0, sizeof(out_name_buffer));
                }
            } else if (!(entry->attributes & ATTR_LONG_FILE_NAME)) {
                // Handle short file name (SFN) entries (non-LFN case)
                char sfn[12] = {0};
                memcpy(sfn, entry->name, 8);
                if (entry->ext[0] != ' ') {
                    strcat(sfn, ".");
                    strncat(sfn, entry->ext, 3);
                }

                // Compare the SFN with the target filename
                if (strcmp(sfn, filename) == 0) {
                    *out_cluster = current_cluster;
                    *out_offset = offset;
                    free(cluster_data);
                    return;
                }
            }
        }

        // Move to the next cluster in the chain
        current_cluster = fat->fat_chain[current_cluster];
        free(cluster_data);
    }

    // If we reach here, the file was not found
    *out_cluster = 0;
    *out_offset = 0;
}

DirectoryEntry_t fat32_read_file_info(fat_t* fat, size_t dir_clust, const char* file) {
    size_t out_clust, out_offset;
    DirectoryEntry_t de = {0};

    fat32_get_file_info_coords(fat, dir_clust, file, &out_clust, &out_offset);

    size_t offset = fat->cluster_base + (out_clust * fat->cluster_size) + out_offset;
    fseek(fat->image, offset, SEEK_SET);

    fread(&de, sizeof(DirectoryEntry_t), 1, fat->image);

    return de; 
}

void fat32_write_file_info(fat_t* fat, size_t dir_clust, const char* file, DirectoryEntry_t ent) {
    size_t out_clust, out_offset;

    fat32_get_file_info_coords(fat, dir_clust, file, &out_clust, &out_offset);

    size_t offset = fat->cluster_base + (out_clust * fat->cluster_size) + out_offset;
    fseek(fat->image, offset, SEEK_SET);

    fwrite(&ent, sizeof(DirectoryEntry_t), 1, fat->image);
}

void fat32_write_size(fat_t* fat, size_t fp_cluster, size_t fp_offset, size_t size) {
    size_t offset = fat->cluster_base + (fp_cluster * fat->cluster_size) + fp_offset;
    printf("=====> %x\n", offset);
    fseek(fat->image, offset, SEEK_SET);

    DirectoryEntry_t entry;
    fread(&entry, sizeof(DirectoryEntry_t), 1, fat->image);

    fseek(fat->image, offset, SEEK_SET);
    
    entry.file_size = size;

    fwrite(&entry, sizeof(DirectoryEntry_t), 1, fat->image);
}

void fat32_write(fat_t* fat, const char* path, size_t offset, size_t size, const char* buffer) {
    size_t out_file_size;

    size_t cluster = fat32_search(fat, path);

    size_t filesize = fat32_get_file_size(fat, path);

    fat32_write_experimental(fat, cluster, filesize, offset, size, &out_file_size, buffer);

    size_t fcl, fof;

    // TODO: Get directory cluster and filename

    const char* file = path + strlen(path);

    while(*file != '/') {
        file--;
    }

    file++;

    
    char* dirp = calloc((file - path) + 1, 1);

    memcpy(dirp, path, file - path);

    printf("Path: %s\n", dirp);
    printf("File: %s\n", file);

    size_t dir_cluster = fat32_search(fat, dirp);
     
    free(dirp);

    fat32_get_file_info_coords(fat, dir_cluster, file, &fcl, &fof);
    fat32_write_size(fat, fcl, fof, out_file_size);
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

    direntry_t* dir = read_directory(&myfat, myfat.fat->root_directory_offset_in_clusters);
    direntry_t* orig = dir;

    fast_traverse(dir);

    //fat32_create_file(&myfat, 2, "Gavno", false);
    //size_t cluster = fat32_create_file(&myfat, 2, "Pokemon.txt", true);

    //printf("File at cluster: %zu\n", cluster);

    char* memory = "Pikachu forever!!!\n";

    //size_t cluster = fat32_create_file(&myfat, 2, "Pokemon.txt", true);

    fat32_write(&myfat, "/Pokemon.txt", 4, strlen(memory), memory);

    fat32_deinit(&myfat);
}
