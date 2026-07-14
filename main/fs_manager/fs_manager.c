#include "fs_manager.h"

#include "esp_log.h"
#include "stdio.h"
#include "string.h"
#include "dirent.h"
#include "errno.h"

#define TAG  "[FS]"

/* --------------------------------------------------------------------------
 * Basic I/O
 * -------------------------------------------------------------------------- */

bool fs_exists(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f)
    {
        fclose(f);
        return true;
    }
    return false;
}

bool fs_read(const char *path, char *buf, uint16_t size)
{
    ESP_LOGI(TAG, "Reading: %s", path);

    FILE *f = fopen(path, "r");
    if (!f)
    {
        ESP_LOGE(TAG, "Failed to open for reading: %s", path);
        return false;
    }

    fread(buf, 1, size, f);
    fclose(f);

    ESP_LOGD(TAG, "Read OK");
    return true;
}

bool fs_write(const char *path, char *buf, uint16_t size)
{
    ESP_LOGI(TAG, "Writing: %s (%u bytes)", path, size);

    FILE *f = fopen(path, "w");
    if (!f)
    {
        ESP_LOGE(TAG, "Failed to open for writing: %s", path);
        return false;
    }

    errno = 0;
    size_t written = fwrite(buf, 1, size, f);
    fflush(f);
    fclose(f);

    if (written != size)
    {
        ESP_LOGE(TAG, "====== FS WRITE FAILURE ======");
        ESP_LOGE(TAG, "Path     : %s", path);
        ESP_LOGE(TAG, "Expected : %u bytes", size);
        ESP_LOGE(TAG, "Written  : %u bytes", (unsigned)written);
        ESP_LOGE(TAG, "errno    : %d", errno);
        ESP_LOGE(TAG, "==============================");
        return false;
    }

    ESP_LOGD(TAG, "Write OK (%u bytes)", (unsigned)written);
    return true;
}

bool fs_read_chunk(const char *path, uint8_t *buf, uint32_t offset, uint32_t size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        ESP_LOGE(TAG, "Failed to open: %s", path);
        return false;
    }

    if (fseek(f, (long)offset, SEEK_SET) != 0)
    {
        ESP_LOGE(TAG, "fseek failed at offset %lu", (unsigned long)offset);
        fclose(f);
        return false;
    }

    size_t got = fread(buf, 1, size, f);
    fclose(f);

    if (got != size)
    {
        /* Partial read at end-of-file is acceptable for the caller to handle */
        ESP_LOGW(TAG, "Read %zu / %lu bytes from %s", got, (unsigned long)size, path);
        return (got > 0);
    }

    return true;
}

uint32_t fs_get_file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    uint32_t size = (uint32_t)ftell(f);
    fclose(f);

    return size;
}

/* --------------------------------------------------------------------------
 * Directory utilities
 * -------------------------------------------------------------------------- */

void fs_scan(const char *path)
{
    static uint8_t depth = 0;

    const char * const indent[4] =
    {
        "- ",
        "  - ",
        "    - ",
        "      - ",
    };

    if (depth == 0)
    {
        ESP_LOGI(TAG, "Scan: %s", path);
    }

    if (depth >= 4)
    {
        ESP_LOGE(TAG, "fs_scan: max recursion depth reached");
        return;
    }

    DIR *dir = opendir(path);
    if (!dir)
    {
        ESP_LOGW(TAG, "Cannot open dir: %s", path);
        return;
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL)
    {
        ESP_LOGI(TAG, "%s%s", indent[depth], de->d_name);

        if (de->d_type == DT_DIR)
        {
            char child[257];
            snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
            depth++;
            fs_scan(child);
            depth--;
        }
    }

    closedir(dir);
}

bool fs_delete(const char *path)
{
    static uint8_t depth = 0;

    if (depth == 0)
    {
        ESP_LOGI(TAG, "Delete: %s", path);
    }

    if (depth >= 4)
    {
        ESP_LOGE(TAG, "fs_delete: max recursion depth reached");
        return false;
    }

    DIR *dir = opendir(path);

    if (!dir)
    {
        /* Single file */
        if (remove(path) == 0)
        {
            ESP_LOGI(TAG, "Deleted: %s", path);
            return true;
        }
        ESP_LOGE(TAG, "Failed to delete: %s (errno=%d)", path, errno);
        return false;
    }

    /* Directory: recurse then remove */
    struct dirent *de;
    char child[257];

    while ((de = readdir(dir)) != NULL)
    {
        if (de->d_name[0] == '.') continue;

        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);

        if (de->d_type == DT_DIR)
        {
            depth++;
            fs_delete(child);
            depth--;
        }
        else
        {
            if (remove(child) == 0)
            {
                ESP_LOGI(TAG, "Deleted: %s", child);
            }
            else
            {
                ESP_LOGE(TAG, "Failed to delete: %s (errno=%d)", child, errno);
            }
        }
    }

    closedir(dir);

    if (remove(path) == 0)
    {
        ESP_LOGI(TAG, "Deleted dir: %s", path);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to delete dir: %s (errno=%d)", path, errno);
    }

    return true;
}

int fs_list_files(const char *path, const char *extension,
                  fs_file_info_t *out_list, int max_count)
{
    DIR *dir = opendir(path);
    if (!dir)
    {
        ESP_LOGE(TAG, "Cannot open dir: %s", path);
        return 0;
    }

    struct dirent *entry;
    int   count   = 0;
    size_t ext_len = extension ? strlen(extension) : 0;

    while ((entry = readdir(dir)) != NULL && count < max_count)
    {
        if (entry->d_name[0] == '.')      continue;
        if (entry->d_type  != DT_REG)     continue;

        size_t name_len = strlen(entry->d_name);

        if (extension && ext_len > 0)
        {
            if (name_len < ext_len) continue;
            if (strcmp(&entry->d_name[name_len - ext_len], extension) != 0) continue;
        }

        size_t path_len = strlen(path);

        /* Reserve 1 for '/' and 1 for '\0' */
        if ((path_len + 1 + name_len) >= sizeof(out_list[0].filepath))
        {
            ESP_LOGW(TAG, "Path too long, skipping: %s/%s", path, entry->d_name);
            continue;
        }

        snprintf(out_list[count].filepath, sizeof(out_list[count].filepath),
                 "%.255s/%.255s", path, entry->d_name);

        out_list[count].size = fs_get_file_size(out_list[count].filepath);
        count++;
    }

    closedir(dir);
    return count;
}

/* --------------------------------------------------------------------------
 * Display helpers
 * -------------------------------------------------------------------------- */

#define BOX_WIDTH        54
#define BOX_TOP_LEFT     "\xe2\x95\x94"   /* U+2554 */
#define BOX_TOP_RIGHT    "\xe2\x95\x97"   /* U+2557 */
#define BOX_BOTTOM_LEFT  "\xe2\x95\x9a"   /* U+255A */
#define BOX_BOTTOM_RIGHT "\xe2\x95\x9d"   /* U+255D */
#define BOX_VERTICAL     "\xe2\x95\x91"   /* U+2551 */
#define BOX_HORIZONTAL   "\xe2\x95\x90"   /* U+2550 */
#define BOX_T_RIGHT      "\xe2\x95\xa0"   /* U+2560 */
#define BOX_T_LEFT       "\xe2\x95\xa3"   /* U+2563 */

static void print_box_hline(const char *left, const char *right)
{
    printf("%s", left);
    for (int i = 0; i < BOX_WIDTH + 2; i++) printf("%s", BOX_HORIZONTAL);
    printf("%s\n", right);
}

static void print_box_separator(void)
{
    print_box_hline(BOX_T_RIGHT, BOX_T_LEFT);
}

static void print_boxed_line(const char *content)
{
    int len     = (int)strlen(content);
    int padding = BOX_WIDTH - len;
    if (padding < 0) padding = 0;

    printf("%s %s", BOX_VERTICAL, content);
    for (int i = 0; i < padding; i++) printf(" ");
    printf(" %s\n", BOX_VERTICAL);
}

void fs_print_file_list(const char *title, const char *path,
                        fs_file_info_t *file_list, int count)
{
    /* "[%2d] %-35s %7lu B": 5 + 35 + 1 + 10 + 2 + NUL = 54 bytes max */
    char line[64];

    print_box_hline(BOX_TOP_LEFT, BOX_TOP_RIGHT);
    print_boxed_line(title);
    print_box_separator();

    snprintf(line, sizeof(line), "Path: %s", path);
    print_boxed_line(line);
    print_box_separator();

    if (count == 0)
    {
        print_boxed_line("No files found");
    }
    else
    {
        for (int i = 0; i < count; i++)
        {
            const char *name = strrchr(file_list[i].filepath, '/');
            name = name ? name + 1 : file_list[i].filepath;

            /* Print the row directly — avoids snprintf format-truncation
             * warnings caused by unbounded string width specifiers. */
            printf("%s [%2d] %-35.35s %7lu B",
                   BOX_VERTICAL, i + 1, name, (unsigned long)file_list[i].size);

            /* Pad to BOX_WIDTH and close the box border */
            int used = 5 + 35 + 9;  /* "[%2d] " + name(35) + " 9999999 B" */
            int pad  = BOX_WIDTH - used;
            for (int p = 0; p < pad; p++) printf(" ");
            printf(" %s\n", BOX_VERTICAL);
        }
        print_box_separator();
        snprintf(line, sizeof(line), "Total: %d file(s)", count);
        print_boxed_line(line);
    }

    print_box_hline(BOX_BOTTOM_LEFT, BOX_BOTTOM_RIGHT);
    printf("\n");
}

/* --- Tree printer --- */

typedef struct
{
    int  current_count;
    int  max_items;
    bool truncated;
} tree_ctx_t;

static void truncate_name(const char *src, char *dst, int max_len)
{
    int len = (int)strlen(src);
    if (len <= max_len)
    {
        strcpy(dst, src);
        return;
    }
    int head = max_len - 8;
    if (head < 4) head = 4;

    strncpy(dst, src, head);
    dst[head] = '\0';
    strcat(dst, "....");
    strcat(dst, &src[len - 4]);
}

static void print_tree_node(const char *path, int depth,
                             const char *prefix, tree_ctx_t *ctx)
{
    if (depth >= 4)
    {
        char line[BOX_WIDTH + 1];
        snprintf(line, sizeof(line), "%s+- (max depth)", prefix);
        print_boxed_line(line);
        return;
    }

    if (ctx->current_count >= ctx->max_items)
    {
        if (!ctx->truncated)
        {
            char line[BOX_WIDTH + 1];
            snprintf(line, sizeof(line), "%s+- ... (truncated, max %d)",
                     prefix, ctx->max_items);
            print_boxed_line(line);
            ctx->truncated = true;
        }
        return;
    }

    DIR *dir = opendir(path);
    if (!dir) return;

    /* Count non-hidden entries for is_last detection */
    int total = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_name[0] != '.') total++;
    }
    rewinddir(dir);

    int current = 0;
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_name[0] == '.') continue;

        if (ctx->current_count >= ctx->max_items)
        {
            if (!ctx->truncated)
            {
                char line[BOX_WIDTH + 1];
                snprintf(line, sizeof(line), "%s+- ... (truncated)", prefix);
                print_boxed_line(line);
                ctx->truncated = true;
            }
            break;
        }

        current++;
        bool is_last = (current == total);
        const char *next_pfx = is_last ? "   " : "|  ";

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        char display[32];
        truncate_name(entry->d_name, display, 28);

        char line[BOX_WIDTH + 1];

        if (entry->d_type == DT_DIR)
        {
            snprintf(line, sizeof(line), "%s+- [D] %s/", prefix, display);
            print_boxed_line(line);
            ctx->current_count++;

            char child_prefix[16];
            snprintf(child_prefix, sizeof(child_prefix), "%s%s", prefix, next_pfx);
            print_tree_node(full_path, depth + 1, child_prefix, ctx);
        }
        else
        {
            uint32_t sz     = fs_get_file_size(full_path);
            int prefix_len  = (int)strlen(prefix);
            int left_len    = prefix_len + 3 + 4 + (int)strlen(display);
            int size_width  = 10;
            int padding     = BOX_WIDTH - left_len - size_width;
            if (padding < 1) padding = 1;

            snprintf(line, sizeof(line), "%s+- [F] %s%*s%8lu B",
                     prefix, display, padding, "", (unsigned long)sz);
            print_boxed_line(line);
            ctx->current_count++;
        }
    }

    closedir(dir);
}

void fs_print_tree(const char *path, const char *title, int max_items)
{
    tree_ctx_t ctx =
    {
        .current_count = 0,
        .max_items     = max_items,
        .truncated     = false,
    };

    char line[BOX_WIDTH + 1];

    print_box_hline(BOX_TOP_LEFT, BOX_TOP_RIGHT);
    print_boxed_line(title);
    print_box_separator();

    snprintf(line, sizeof(line), "Root: %s", path);
    print_boxed_line(line);
    print_box_separator();

    snprintf(line, sizeof(line), "[D] %s/", path);
    print_boxed_line(line);

    print_tree_node(path, 1, "", &ctx);

    print_box_separator();

    if (ctx.truncated)
    {
        snprintf(line, sizeof(line), "Displayed: %d item(s) (TRUNCATED)",
                 ctx.current_count);
    }
    else
    {
        snprintf(line, sizeof(line), "Total: %d item(s)", ctx.current_count);
    }
    print_boxed_line(line);

    print_box_hline(BOX_BOTTOM_LEFT, BOX_BOTTOM_RIGHT);
}
