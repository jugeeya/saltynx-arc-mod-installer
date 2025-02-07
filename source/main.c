#include "saltysd_core.h"
#include "saltysd_ipc.h"
#include "saltysd_dynamic.h"
#include <switch_min.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>

extern u32 __start__;

static char g_heap[0x8000];

void __libnx_init(void* ctx, Handle main_thread, void* saved_lr);
void __attribute__((weak)) NORETURN __libnx_exit(int rc);
void __nx_exit(int, void*);
void __libc_fini_array(void);
void __libc_init_array(void);

u32 __nx_applet_type = AppletType_None;

Handle orig_main_thread;
void* orig_ctx;
void* orig_saved_lr;

void __libnx_init(void* ctx, Handle main_thread, void* saved_lr) {
	extern char* fake_heap_start;
	extern char* fake_heap_end;

	fake_heap_start = &g_heap[0];
	fake_heap_end   = &g_heap[sizeof g_heap];
	
	orig_ctx = ctx;
	orig_main_thread = main_thread;
	orig_saved_lr = saved_lr;
	
	// Call constructors.
	//void __libc_init_array(void);
	__libc_init_array();
}

void __attribute__((weak)) NORETURN __libnx_exit(int rc) {
	// Call destructors.
	//void __libc_fini_array(void);
	__libc_fini_array();

	SaltySD_printf("SaltySD Plugin: jumping to %p\n", orig_saved_lr);

	__nx_exit(0, orig_saved_lr);
	while (true);
}

#define FILENAME_SIZE 0x120
#define FILE_READ_SIZE 0x5000

void current_time(int* hours, int* minutes, int* seconds) {
	time_t now;
	time(&now);

	struct tm *local = localtime(&now);
    *hours = local->tm_hour;
    *minutes = local->tm_min;
    *seconds = local->tm_sec;
}

int seek_files(FILE* f, uint64_t offset, FILE* arc) {
    // Set file pointers to start of file and offset respectively
    int ret = SaltySDCore_fseek(f, 0, SEEK_SET);
    if (ret) {
        SaltySD_printf("Failed to seek file with errno %d\n", ret);
        return ret;
    }

    ret = SaltySDCore_fseek(arc, offset, SEEK_SET);
    if (ret) {
        SaltySD_printf("Failed to seek offset %llx from start of data.arc with errno %d\n", offset, ret);
        return ret;
    }

    return 0;
}

int load_mod(char* path, uint64_t offset, FILE* arc) {
    FILE* f = SaltySDCore_fopen(path, "rb");
    if(f) {
        int ret = seek_files(f, offset, arc);
        if (!ret) {
            void* copy_buffer = malloc(FILE_READ_SIZE);
            uint64_t total_size = 0;

            // Copy in up to FILE_READ_SIZE byte chunks
            size_t size;
            do {
                size = SaltySDCore_fread(copy_buffer, 1, FILE_READ_SIZE, f);
                total_size += size;

                SaltySDCore_fwrite(copy_buffer, 1, size, arc);
            } while(size == FILE_READ_SIZE);

            free(copy_buffer);
            SaltySD_printf("Installed file '%s' with 0x%llx bytes\n", path, total_size);
        }

        SaltySDCore_fclose(f);
    } else {
        SaltySD_printf("Found file '%s', failed to get file handle\n", path, offset);
        return -1;
    }

    return 0;
}

int create_backup(char* mod_dir, char* filename, uint64_t offset, FILE* arc) {
    char* backup_path = malloc(FILENAME_SIZE);
    char* mod_path = malloc(FILENAME_SIZE);
    snprintf(backup_path, FILENAME_SIZE, "sdmc:/SaltySD/backups/0x%lx.backup", offset);
    snprintf(mod_path, FILENAME_SIZE, "sdmc:/SaltySD/%s/%s", mod_dir, filename);

    FILE* backup = SaltySDCore_fopen(backup_path, "wb");
    FILE* mod = SaltySDCore_fopen(mod_path, "rb");
    if(backup && mod) {
        SaltySDCore_fseek(mod, 0, SEEK_END);
        size_t filesize = SaltySDCore_ftell(mod);
        int ret = seek_files(backup, offset, arc);
        if (!ret) {
            void* copy_buffer = malloc(FILE_READ_SIZE);
            uint64_t total_size = 0;

            // Copy in up to FILE_READ_SIZE byte chunks
            size_t size;
            do {
                size_t to_read = FILE_READ_SIZE;
                if (filesize < FILE_READ_SIZE)
                    to_read = filesize;
                size = SaltySDCore_fread(copy_buffer, 1, to_read, arc);
                total_size += size;
                filesize -= size;

                SaltySDCore_fwrite(copy_buffer, 1, size, backup);
            } while(size == FILE_READ_SIZE);

            free(copy_buffer);
            SaltySD_printf("Created backup '%s' with 0x%llx bytes\n", backup_path, total_size);
        }

        SaltySDCore_fclose(backup);
        SaltySDCore_fclose(mod);
    } else {
        if (backup) SaltySDCore_fclose(backup);
        else SaltySD_printf("Attempted to create backup file '%s', failed to get backup file handle\n", backup_path, offset);

        if (mod) SaltySDCore_fclose(mod);
        else SaltySD_printf("Attempted to create backup file '%s', failed to get mod file handle '%s'\n", backup_path, offset, mod_path);
    }

    free(backup_path);
    free(mod_path);

    return 0;
}

#define UC(c) ((unsigned char)c)

char _isxdigit (unsigned char c)
{
    if (( c >= UC('0') && c <= UC('9') ) ||
        ( c >= UC('a') && c <= UC('f') ) ||
        ( c >= UC('A') && c <= UC('F') ))
        return 1;
    return 0;
}

unsigned char xtoc(char x) {
    if (x >= UC('0') && x <= UC('9'))
        return x - UC('0');
    else if (x >= UC('a') && x <= UC('f'))
        return (x - UC('a')) + 0xa;
    else if (x >= UC('A') && x <= UC('F'))
        return (x - UC('A')) + 0xA;
    return -1;
}

uint64_t hex_to_u64(char* str) {
    if(str[0] == '0' && str[1] == 'x')
        str += 2;
    uint64_t value = 0;
    while(_isxdigit(*str)) {
        value *= 0x10;
        value += xtoc(*str);
        str++;
    }
    return value;
}

char** mod_dirs = NULL;
size_t num_mod_dirs = 0;

void add_mod_dir(char* path) {
    mod_dirs = realloc(mod_dirs, ++num_mod_dirs * sizeof(const char*));
    mod_dirs[num_mod_dirs-1] = malloc(FILENAME_SIZE * sizeof(char));
    strcpy(mod_dirs[num_mod_dirs-1], path);
}

void remove_last_mod_dir() {
    free(mod_dirs[num_mod_dirs-1]);
    num_mod_dirs--;
}

int load_mods(FILE* f_arc) {
    char* mod_dir = malloc(FILENAME_SIZE);
    strcpy(mod_dir, mod_dirs[num_mod_dirs-1]);

    remove_last_mod_dir();

    char* tmp = malloc(FILENAME_SIZE);
    DIR *d;
    struct dirent *dir;
    int hours, minutes, seconds;

    SaltySD_printf("Searching mod dir '%s'...\n", mod_dir);
    
    snprintf(tmp, FILENAME_SIZE, "sdmc:/SaltySD/%s/", mod_dir);

    d = SaltySDCore_opendir(tmp);
    if (d)
    {
        while ((dir = SaltySDCore_readdir(d)) != NULL)
        {
            if(dir->d_type == DT_DIR) {
                if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0)
                    continue;
                snprintf(tmp, FILENAME_SIZE, "%s/%s", mod_dir, dir->d_name);
                add_mod_dir(tmp);
            } else {
                uint64_t offset = hex_to_u64(dir->d_name);
                if(offset){
                    SaltySD_printf("Found file '%s', offset = %ld\n", dir->d_name, offset);
                    if (strcmp(mod_dir, "backups") == 0) {
                        current_time(&hours, &minutes, &seconds);
                        SaltySD_printf("[%02d:%02d:%02d] About to install backup '%s'\n", hours, minutes, seconds, tmp);
                        snprintf(tmp, FILENAME_SIZE, "sdmc:/SaltySD/backups/%s", dir->d_name);
                        load_mod(tmp, offset, f_arc);

                        SaltySDCore_remove(tmp);
                        current_time(&hours, &minutes, &seconds);
                        SaltySD_printf("[%02d:%02d:%02d] Installed backup '%s' into arc and deleted it\n", hours, minutes, seconds, tmp);
                    } else {
                        current_time(&hours, &minutes, &seconds);
                        SaltySD_printf("[%02d:%02d:%02d] About to create backup '%s'\n", hours, minutes, seconds, dir->d_name);
                        create_backup(mod_dir, dir->d_name, offset, f_arc);

                        snprintf(tmp, FILENAME_SIZE, "sdmc:/SaltySD/%s/%s", mod_dir, dir->d_name);
                        SaltySD_printf("[%02d:%02d:%02d] About to install mod '%s/%s'\n", hours, minutes, seconds, mod_dir, dir->d_name);
                        load_mod(tmp, offset, f_arc);
                        current_time(&hours, &minutes, &seconds);
                        SaltySD_printf("[%02d:%02d:%02d] Installed mod '%s' into arc\n", hours, minutes, seconds, tmp);
                    }
                } else {
                    SaltySD_printf("Found file '%s', offset not parsable\n", dir->d_name);
                }
            }
        }
        SaltySDCore_closedir(d);
    } else {
        SaltySD_printf("Failed to open mod directory '%s'\n", mod_dir);
    }

    free(tmp);
    free(mod_dir);
    return 0;
}

int main(int argc, char *argv[])
{
    SaltySD_printf("BEGIN: SALTYSD MOD INSTALLER\n\n");

    FILE* f_arc = SaltySDCore_fopen("sdmc:/atmosphere/titles/01006A800016E000/romfs/data.arc", "r+b");
    if(!f_arc){
        SaltySD_printf("Failed to get file handle to data.arc\n");
        goto end;
    }

    // restore backups -> delete backups -> make backups for current mods -> install current mods
    SaltySD_printf("Installing backups...\n\n");
    add_mod_dir("backups");
    load_mods(f_arc);
    
    SaltySD_printf("Installing mods...\n\n");
    add_mod_dir("mods");
    while (num_mod_dirs > 0) {
        load_mods(f_arc);
    }

    free(mod_dirs);

    SaltySDCore_fclose(f_arc);

end:
    SaltySD_printf("END: SALTYSD MOD INSTALLER\n\n");
    __libnx_exit(0);
}
