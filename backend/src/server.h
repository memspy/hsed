/*
 * server.h — the Unix domain socket front-end. Accepts connections, hands
 * each one to its own thread, and dispatches the small text command
 * protocol described in protocol.h to proc_scan/reclaim/tracer.
 */
#ifndef HSED_SERVER_H
#define HSED_SERVER_H

#include <signal.h>
#include <stddef.h>

/*
 * Binds `socket_path` (removing any stale socket file first), listens, and
 * serves clients until *shutdown_flag becomes non-zero (checked roughly
 * every 500ms). Cleans up (closes the listening socket, unlinks the socket
 * file) before returning. `max_capture` bounds how many bytes of a write()
 * payload STREAM will preview per event.
 *
 * Returns 0 on a clean shutdown, -1 if it couldn't even start listening.
 */
int hsed_server_run(const char *socket_path, size_t max_capture,
                     volatile sig_atomic_t *shutdown_flag);

#endif /* HSED_SERVER_H */
