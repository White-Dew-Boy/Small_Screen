#include "sd_file.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_log.h"

static const char *TAG = "sd_file";

void sd_file_join(const char *dir, const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", dir, name);
}

/* Sort order for the browser: directories first, then files; both groups
 * alphabetical (case-insensitive). FAT returns entries in insertion order,
 * which is usually arbitrary, so the listing is sorted here. */
static int entry_cmp(const void *a, const void *b)
{
    const sd_file_entry_t *ea = (const sd_file_entry_t *)a;
    const sd_file_entry_t *eb = (const sd_file_entry_t *)b;

    if (ea->is_dir != eb->is_dir) {
        return ea->is_dir ? -1 : 1; /* directories before files */
    }
    return strcasecmp(ea->name, eb->name);
}

esp_err_t sd_file_list(const char *path, sd_file_entry_t *entries,
                       size_t max_entries, size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = 0;
    }

    /* One open directory handle at a time: the FAT VFS file table is tiny
     * (max_files in the sd_card mount config), so keep this short. */
    DIR *dir = opendir(path);
    if (dir == NULL) {
        ESP_LOGW(TAG, "opendir(%s) failed", path);
        return ESP_ERR_NOT_FOUND;
    }

    size_t n = 0;
    struct dirent *de;
    while (n < max_entries && (de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }

        sd_file_entry_t *e = &entries[n];
        strncpy(e->name, de->d_name, SD_FILE_NAME_MAX);
        e->name[SD_FILE_NAME_MAX] = '\0';

        /* stat() tells directory vs file and the size. It performs a short
         * blocking SPI transaction per entry — acceptable for one directory
         * level (a few dozen entries at most on a small screen). */
        char full[256];
        sd_file_join(path, de->d_name, full, sizeof(full));
        struct stat st;
        if (stat(full, &st) == 0) {
            e->is_dir = S_ISDIR(st.st_mode);
            e->size   = (uint32_t)st.st_size;
        } else {
            /* Entry vanished while listing: show it as a file, size 0. */
            e->is_dir = false;
            e->size   = 0;
        }
        n++;
    }

    closedir(dir); /* free the directory handle as soon as possible */

    qsort(entries, n, sizeof(sd_file_entry_t), entry_cmp);

    if (out_count != NULL) {
        *out_count = n;
    }
    return ESP_OK;
}
