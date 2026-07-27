#ifndef HSED_SERVER_H
#define HSED_SERVER_H

#include <signal.h>
#include <stddef.h>


int hsed_server_run(const char *socket_path, size_t max_capture,
                     volatile sig_atomic_t *shutdown_flag);

#endif 
