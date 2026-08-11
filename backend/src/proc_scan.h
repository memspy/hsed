#ifndef HSED_PROC_SCAN_H
#define HSED_PROC_SCAN_H

#include <sys/types.h>
#include <time.h>

#define HSED_PATH_MAX 4096
#define HSED_NAME_MAX 256
#define HSED_CMDLINE_MAX 1024


#define HSED_SCAN_THREADS_AUTO 0


#define HSED_UID_ANY ((uid_t)-1)

typedef struct {
    pid_t pid;
    int fd;
    char mode[3];           
    long long size;
    uid_t uid;
    char user[HSED_NAME_MAX];
    char comm[HSED_NAME_MAX];
    char cmdline[HSED_CMDLINE_MAX];
    char path[HSED_PATH_MAX];   
    unsigned long long inode;
    unsigned dev_major;
    unsigned dev_minor;
    time_t mtime;
} hsed_entry_t;

typedef struct {
    hsed_entry_t *items;
    size_t count;
    size_t capacity;
} hsed_list_t;


typedef struct {
    size_t count;
    long long total_bytes;
} hsed_stats_t;

void hsed_list_init(hsed_list_t *list);
void hsed_list_free(hsed_list_t *list);


int hsed_scan(hsed_list_t *out, long long min_size, pid_t only_pid,
              uid_t uid_filter, int num_threads);


int hsed_scan_stats(hsed_stats_t *out, long long min_size, uid_t uid_filter,
                     int num_threads);

#endif 
