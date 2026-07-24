# Hidden Space Explorer

A tool for the classic "`df -h` says 100%, `du -sh` finds nothing" incident.

**Architecture:** the privileged part — scanning `/proc`, `ftruncate`,
signals, `ptrace` — is a standalone C daemon (`hsedd`) that runs in the
background or on demand and accepts commands over a Unix socket. `hsed` is
the interactive TUI (Python/Textual), a thin client that connects to it.
Python never touches `/proc`, `ftruncate`, `kill`, or `ptrace` directly —
every privileged operation happens in `hsedd`.

```
┌──────────────┐  SCAN / TRUNCATE / HUP / KILL / STREAM   ┌───────────────────────┐
│  hsed         │ ────────────────────────────────────────► │  hsedd (C daemon)      │
│  (Python TUI) │            JSON-lines responses            │  /proc, ftruncate,     │
└──────────────┘ ◄──────────────────────────────────────── │  kill, ptrace(SEIZE)   │
                          Unix domain socket                 └───────────────────────┘
```

## Install

```bash
sudo dpkg -i hsed_1.0.0_amd64.deb
```

That's it — `hsed` and `hsedd` both land on `PATH`. The Python TUI runs
against a vendored copy of its dependencies bundled in the package, so
nothing else needs to be installed (no pip, no internet access needed).

Start the backend:

```bash
sudo systemctl enable --now hsed     # if systemd is available, or:
sudo hsedd                           # background daemon
sudo hsedd --foreground              # stays attached (Ctrl-C to stop)
```

Then just run:

```bash
sudo hsed
```

### Building the package yourself

```bash
cd backend && make                       # builds hsedd
cd ../packaging && ./build-deb.sh        # assembles + builds the .deb
```

See `packaging/build-deb.sh` for exactly what goes into the package (it's
the same steps as above: compile `hsedd`, vendor the Python deps, lay out
the `usr/`/`lib/` tree, `dpkg-deb --build`).

## The problem it solves

On Linux, deleting a file (`rm`, `unlink()`) only removes its directory
entry. If a process still holds an open file descriptor to that inode, the
kernel keeps the inode — and every block it occupies — alive until the last
fd is closed. `du` walks the directory tree, so it never sees these blocks.
`df` reports real filesystem block usage, so it does. The gap between the
two is exactly this: unlinked-but-open files.

Common causes:
- a service logs to a file that got deleted by a naive cleanup cron/log
  rotation that didn't `HUP`/`USR1` the process afterward — it keeps
  writing into the void, forever growing
- a crashed/restarted process leaked an fd to a large temp file
- a compromised process or container deliberately `open()`s a file, then
  `unlink()`s it immediately, to hide payload/exfil data from `find`,
  `du`, antivirus directory scans, and casual `ls`

The only reliable way to find these is `/proc/<pid>/fd/*`, where the kernel
suffixes the symlink target with ` (deleted)`. `hsedd` automates that scan
and exposes four more things over its socket protocol:

- **`SCAN`** — walk `/proc`, list every unlinked-but-open fd
- **`TRUNCATE <pid> <fd>`** — reopen the same inode through
  `/proc/<pid>/fd/<fd>` and `ftruncate` it to zero, freeing the disk blocks
  immediately, without signaling, pausing, or restarting the process
- **`HUP <pid>`** — the polite request: ask the process to reopen its log
  files itself, if it supports that convention
- **`KILL <pid>`** — SIGKILL, for when the process is hung/unresponsive and
  won't release the fd any other way. Immediate, uncatchable, no graceful
  shutdown; the kernel releases every fd as part of normal process
  teardown. The TUI requires typing the word `KILL` to confirm — a single
  stray keypress can't trigger it.
- **`STREAM <pid> <fd>`** — attach via `ptrace(PTRACE_SEIZE)` and stream,
  live, exactly what bytes the process is writing into that fd right now

## Run the TUI

```bash
sudo hsed
sudo hsed --min-size 10485760      # only show entries >= 10MiB
sudo hsed --pid 4821                # inspect one process only
sudo hsed --socket /run/hsed.sock  # point at a non-default daemon
```

`hsed` connects to `hsedd`'s socket — `$HSED_SOCKET`, else `/run/hsed.sock`
if you're root, else `/tmp/hsed-<uid>.sock`. If it can't connect, it tells
you exactly how to start the daemon and exits, rather than pretending to
work with no data.

## Keybindings

| Key     | Action                                            |
|---------|------------------------------------------------------|
| `r`     | Rescan immediately                                    |
| `/`     | Filter table by process name or path substring        |
| `Enter` | Open details + actions for the selected row            |
| `v`     | Hidden filesystem tree view (paths reconstructed from deleted entries) |
| `q`     | Quit                                                   |

Inside the detail panel:

| Key   | Action                                              |
|-------|--------------------------------------------------------|
| `s`   | Stream live writes to this fd (ptrace, via hsedd)      |
| `t`   | Truncate the hidden file, reclaim its space now        |
| `h`   | Send SIGHUP, asking the process to reopen its files    |
| `k`   | SIGKILL the process (type `KILL` to confirm)           |
| `Esc` | Close                                                   |

## Important operational notes

**Truncate is a low-level operation.** It is safe for the common case —
an append-only log file a process just keeps writing to sequentially. It is
**not** safe, in general, for:

- files a process has `mmap()`'d (truncating out from under a live mapping
  can raise `SIGBUS` on the process's next access to that region)
- files accessed at arbitrary offsets rather than append-only (database
  data files, WAL/redo logs, B-tree indexes, anything with a defined binary
  layout)

For a production database or anything similar, prefer `h` (SIGHUP / graceful
reopen) first. Reach for `t` (truncate) only on confirmed append-only
log-style writers, and `k` (SIGKILL) only when the process is genuinely
hung and a supervisor (systemd, container runtime, etc.) will restart it —
not as a routine way to free space. When in doubt, test the exact same
binary/version against a staging replica first.

**Streaming uses ptrace.** `hsedd` attaches with `PTRACE_SEIZE` (which,
unlike `PTRACE_ATTACH`, doesn't stop the target as a side effect) and only
interrupts it briefly on each traced `write`/`pwrite64`/`writev` syscall —
for a typical daemon this is not noticeable, but for latency-sensitive
workloads it can matter. It requires root or `CAP_SYS_PTRACE`, and depends
on `/proc/sys/kernel/yama/ptrace_scope` allowing the attach (root is
unaffected by this setting).

**Streaming scope in this version:** x86_64 only, and it traces exactly the
PID/TID you give it — it does not follow `clone()`'d threads the way
`strace -f` does. If the fd could be written by a different thread of a
multi-threaded target, find that thread's TID under `/proc/<pid>/task/` and
pass it as the pid instead. `writev()` calls are reported (syscall +
length) but the payload preview isn't captured in this version — plain
`write()`/`pwrite64()` are.

**This tool only detects and lets you act on already-open fds.** It does
not detect rootkits, hook the kernel, or hide/unhide anything itself — it's
a read side, standard `/proc` inspection (the same data `lsof -a +L1` or
`find /proc/*/fd -lname '* (deleted)'` gives you), and a write side that
uses only `ftruncate`, `kill`, and standard `ptrace(2)`. No new
kernel-level capability is introduced beyond what root/`CAP_SYS_PTRACE`
already grant — the same privileges `strace -p` and `gdb -p` already need.

## Protocol

If you want to drive `hsedd` from something other than this TUI, the wire
format is a small newline-delimited text-command / JSON-lines-response
protocol over the Unix socket — see `backend/src/protocol.h` for the exact
schema (`SCAN`, `TRUNCATE`, `HUP`, `KILL`, `STREAM`, `PING`, `QUIT`).

## Uninstall

```bash
sudo systemctl disable --now hsed   # if you enabled the service
sudo dpkg -r hsed                    # remove
sudo dpkg -P hsed                    # remove + purge runtime socket/pidfile
```

## Project layout

```
backend/                       # the C daemon (binary: hsedd)
├── src/
│   ├── proc_scan.c/.h          # /proc/*/fd walk -> hsed_entry_t list
│   ├── reclaim.c/.h            # ftruncate via /proc/pid/fd/N, kill(sig)
│   ├── tracer.c/.h             # ptrace(SEIZE/INTERRUPT) live write streamer (x86_64)
│   ├── protocol.c/.h           # JSON-lines formatting, line I/O helpers
│   ├── server.c/.h             # Unix socket accept loop, one thread per connection
│   ├── base64.c/.h             # binary-safe transport for captured write() bytes
│   ├── util.c/.h                # logging, small string helpers
│   └── hsed.c                   # main(): args, daemonization, signal handling
├── systemd/hsed.service
└── Makefile

hidden_space_explorer/         # the Python TUI (thin client, command: hsed)
├── client.py                   # HsedClient / StreamSession — talks to hsedd's socket
├── app.py                      # Textual UI: table, tree view, detail modal, stream screen
└── __main__.py                 # CLI entry point

packaging/
├── build-deb.sh                 # builds hsedd, vendors Python deps, assembles the .deb
└── hsed_1.0.0_amd64/DEBIAN/     # control, postinst, postrm
```
