# Hidden Space Explorer

A tool for the classic "`df -h` says 100%, `du -sh` finds nothing" incident.

**v1.1.1 note:** a speed + universality pass on top of the v1.1.0 Go
rewrite — SCAN now parallelizes across worker threads, STREAM follows
every thread of a target process (not just the one you named) including
threads created after streaming starts, `writev()` payloads are captured
alongside plain `write()`, SCAN/STATS gained a uid filter, and there's a
new lightweight STATS command for monitoring use. The backend also now
cross-builds for aarch64. See CHANGELOG.md for the full list.

**Architecture:** the privileged part — scanning `/proc`, `ftruncate`,
signals, `ptrace` — is a standalone C daemon (`hsedd`) that runs in the
background or on demand and accepts commands over a Unix socket. `hsed` is
the interactive TUI, a thin client that connects to it. It never touches
`/proc`, `ftruncate`, `kill`, or `ptrace` directly — every privileged
operation happens in `hsedd`.

```
┌──────────────┐  SCAN / STATS / TRUNCATE / HUP / KILL / STREAM   ┌───────────────────────┐
│  hsed         │ ──────────────────────────────────────────────► │  hsedd (C daemon)      │
│  (Go TUI)     │              JSON-lines responses                │  /proc, ftruncate,     │
└──────────────┘ ◄────────────────────────────────────────────── │  kill, ptrace(SEIZE)   │
                          Unix domain socket                       └───────────────────────┘
```

## Install from releases

```bash
sudo dpkg -i hsed_1.1.1_amd64.deb     # or hsed_1.1.1_arm64.deb
```

That's it — `hsed` and `hsedd` both land on `PATH`. `hsed` is a static
binary (`CGO_ENABLED=0`, no libc, no interpreter), so nothing else needs
to be installed.

Start the backend:

```bash
sudo systemctl enable --now hsed     # if systemd is available, or:
sudo hsedd                           # background daemon
sudo hsedd --foreground              # stays attached (Ctrl-C to stop)
```

Then just run:

```bash
hsed
```

### Building the package yourself

```bash
cd packaging
chmod +x ./build-deb.sh    # if permission denied
./build-deb.sh             # amd64 (default)
./build-deb.sh arm64       # cross-build for aarch64, needs gcc-aarch64-linux-gnu
sudo dpkg -i hsed_1.1.1_amd64.deb
sudo systemctl enable --now hsed
```

Requires `gcc`/`make` (for `hsedd`) and Go >= 1.24 (for `hsed`); see
`tui/go.mod` for a note about the `golang.org/x/*` → `github.com/golang/*`
replace directives some restricted-network build environments need. See
`packaging/build-deb.sh` for exactly what goes into the package.

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
(in parallel across worker threads — see below) and exposes five more
things over its socket protocol:

- **`SCAN [min_size] [pid] [uid]`** — walk `/proc`, list every
  unlinked-but-open fd; optionally restrict to one process or one uid
- **`STATS [min_size] [uid]`** — the same matching logic as SCAN, but just
  the headline `count`/`total_bytes` — skips all username/cmdline
  resolution, so it costs meaningfully less on a host with many matching
  fds. Meant for a monitoring check ("is more than 1GB hidden right now?")
  that doesn't need the full breakdown.
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
  live, exactly what bytes are being written into that fd right now, from
  *any* thread of the process — see below.

## Multi-core scanning

SCAN and STATS split the process list across worker threads (one
`/proc/<pid>/fd` walk is independent of every other process's, so this
scales close to linearly with core count on a host with many processes).
Auto-detects the online CPU count by default; override with
`hsedd --scan-threads N` (applies to both the daemon's socket commands and
`--scan-once`/`--stats-once`). A uid filter (`--uid N` on the CLI, or the
`uid` argument over the socket) is checked before opening a process's fd
directory at all, so filtering to one user on a multi-tenant host skips
the expensive per-fd work entirely for everyone else.

## Multi-threaded STREAM

STREAM follows every thread of the target process, including ones created
*after* streaming starts — equivalent to `strace -f`. Since threads created
via pthreads share one fd table (`CLONE_FILES`), any thread can be the one
actually calling `write()` on the fd you asked about; a single-threaded
tracer (v1.1.0 and earlier) would silently miss writes from any thread
other than the one whose PID/TID you happened to pass. Pass the process's
main PID (what `/proc/<pid>/task` lists as the thread-group leader) to
follow the whole process; pass a specific thread's TID (found the same
way) to narrow tracing to just that one thread.

`writev()` calls are also captured now, not just plain `write()`/
`pwrite64()` — the payload preview is reassembled from up to the first 16
iovec segments, concatenated in order, capped at the usual preview size
limit.

## Run the TUI

```bash
hsed
hsed -min-size 10485760      # only show entries >= 10MiB
hsed -pid 4821                 # inspect one process only
hsed -uid 1000                  # only that uid's hidden files
hsed -socket /run/hsed.sock  # point at a non-default daemon
```

`hsed` connects to `hsedd`'s socket — `$HSED_SOCKET`, else `/run/hsed.sock`
if you're root, else `/tmp/hsed-<uid>.sock`. If it can't connect, it tells
you exactly how to start the daemon and exits, rather than pretending to
work with no data.

## Keybindings

| Key     | Action                                                                 |
| ------- | ------------------------------------------------------------------------ |
| `r`     | Rescan immediately                                                     |
| `/`     | Filter table by process name or path substring                         |
| `Enter` | Open details + actions for the selected row                            |
| `v`     | Hidden filesystem tree view (paths reconstructed from deleted entries) |
| `q`     | Quit                                                                   |

Inside the detail panel:

| Key   | Action                                              |
| ----- | ----------------------------------------------------- |
| `s`   | Stream live writes to this fd (ptrace, via hsedd)   |
| `t`   | Truncate the hidden file, reclaim its space now     |
| `h`   | Send SIGHUP, asking the process to reopen its files |
| `k`   | SIGKILL the process (type `KILL` to confirm)        |
| `Esc` | Close                                               |

> Screenshots (`scan.png`, `truncated.png`, `hup.png`, `killing.png`) are
> from the v1.0.0 Python TUI. The Go TUI looks and behaves the same way
> (same layout, same keybindings) but the images haven't been retaken yet
> — happy to swap them in once fresh ones exist.

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
interrupts each thread briefly on its traced `write`/`pwrite64`/`writev`
syscalls — for a typical daemon this is not noticeable, but for
latency-sensitive workloads it can matter. It requires root or
`CAP_SYS_PTRACE`, and depends on `/proc/sys/kernel/yama/ptrace_scope`
allowing the attach (root is unaffected by this setting).

**Architecture support:** x86_64 is implemented and verified on real
hardware, including under real load (repeated rapid attach/detach cycles,
multi-threaded targets, dynamic thread creation). aarch64 has its own
register-access code path (see `backend/src/tracer.c`) and cross-builds
cleanly; SCAN/TRUNCATE/HUP/KILL/STATS have been exercised under QEMU
user-mode emulation against real processes and work correctly there, but
QEMU user-mode's ptrace emulation is known to be unreliable for exactly
the kind of syscall-stepping STREAM does, so the STREAM path specifically
has **not** been confirmed on real ARM64 hardware yet — if you try it on
actual aarch64 hardware, reports (either way) are genuinely useful.

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
schema (`SCAN`, `STATS`, `TRUNCATE`, `HUP`, `KILL`, `STREAM`, `PING`,
`QUIT`). `tui/client` is a small, self-contained reference implementation
if you'd rather read Go than C.

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
│   ├── proc_scan.c/.h          # /proc/*/fd walk -> hsed_entry_t list, parallel + uid-filtered
│   ├── reclaim.c/.h            # ftruncate via /proc/pid/fd/N, kill(sig)
│   ├── tracer.c/.h             # ptrace(SEIZE/INTERRUPT) live write streamer,
│   │                            # multi-thread following, writev capture (x86_64 + aarch64)
│   ├── protocol.c/.h           # JSON-lines formatting, line I/O helpers
│   ├── server.c/.h             # Unix socket accept loop, one thread per connection
│   ├── base64.c/.h             # binary-safe transport for captured write() bytes
│   ├── util.c/.h                # logging, small string helpers
│   └── hsed.c                   # main(): args, daemonization, signal handling
├── systemd/hsed.service
└── Makefile                     # `make` (native) / `make arm64` (cross-build)

tui/                            # the Go TUI (single static binary, command: hsed)
├── go.mod / go.sum
├── main.go                       # CLI entry point
├── client/                        # Client / StreamSession — talks to hsedd's socket
└── ui/                             # bubbletea screens (table, detail, tree, stream, confirms)

packaging/
├── build-deb.sh                 # builds hsedd + hsed, assembles the .deb (amd64 or arm64)
└── pkg-static/DEBIAN/            # control, postinst, postrm
```

---

# Hidden Space Explorer (Russian README)

Инструмент для классического инцидента: «`df -h` показывает 100%, `du -sh` не находит ничего».

**Заметка к v1.1.1:** проход на ускорение и универсальность поверх Go-версии
v1.1.0 — `SCAN` теперь распараллеливается по рабочим потокам, `STREAM`
следит за **всеми** потоками целевого процесса (не только тем, что вы
указали), включая потоки, созданные уже после начала трансляции, полезная
нагрузка `writev()` захватывается наравне с обычным `write()`, `SCAN`/`STATS`
получили фильтр по uid, добавлена лёгкая команда `STATS` для мониторинга.
Бэкенд также теперь кросс-собирается под aarch64. Полный список — в
CHANGELOG.md.

**Архитектура:** привилегированная часть — сканирование `/proc`, `ftruncate`,
сигналы, `ptrace` — представляет собой автономный демон на C (`hsedd`), который
работает в фоне или по требованию и принимает команды через Unix-сокет. `hsed` —
это интерактивный TUI, тонкий клиент, который подключается к нему. Он никогда
не обращается к `/proc`, `ftruncate`, `kill` или `ptrace` напрямую — все
привилегированные операции выполняются в `hsedd`.

```
┌──────────────┐  SCAN / STATS / TRUNCATE / HUP / KILL / STREAM   ┌───────────────────────┐
│  hsed         │ ──────────────────────────────────────────────► │  hsedd (демон на C)    │
│  (Go TUI)     │              ответы в JSON-lines                 │  /proc, ftruncate,     │
└──────────────┘ ◄────────────────────────────────────────────── │  kill, ptrace(SEIZE)   │
                          Unix domain socket                       └───────────────────────┘
```

## Установка с релизов

```bash
sudo dpkg -i hsed_1.1.1_amd64.deb     # или hsed_1.1.1_arm64.deb
```

Вот и всё — `hsed` и `hsedd` оказываются в `PATH`. `hsed` — статический
бинарник (`CGO_ENABLED=0`, без libc, без интерпретатора), поэтому ничего
дополнительно устанавливать не нужно.

Запуск бэкенда:

```bash
sudo systemctl enable --now hsed     # если доступен systemd, или:
sudo hsedd                           # фоновый демон
sudo hsedd --foreground              # остаётся прикреплённым к терминалу (Ctrl-C для остановки)
```

Затем просто запустите:

```bash
hsed
```

### Самостоятельная сборка пакета

```bash
cd packaging
chmod +x ./build-deb.sh    # если ошибка "доступ запрещён"
./build-deb.sh              # amd64 (по умолчанию)
./build-deb.sh arm64        # кросс-сборка под aarch64, нужен gcc-aarch64-linux-gnu
sudo dpkg -i hsed_1.1.1_amd64.deb
sudo systemctl enable --now hsed
```

Нужны `gcc`/`make` (для `hsedd`) и Go >= 1.24 (для `hsed`); про `replace`-
директивы `golang.org/x/*` → `github.com/golang/*`, нужные в окружениях с
ограниченным доступом к сети при сборке, см. `tui/go.mod`. Подробности того,
что попадает в пакет, смотрите в `packaging/build-deb.sh`.

## Какую проблему решает инструмент

В Linux удаление файла (`rm`, `unlink()`) убирает только его запись в каталоге.
Если процесс всё ещё держит открытый файловый дескриптор на этот inode, ядро
сохраняет inode — и каждый занятый им блок — живым до тех пор, пока последний
дескриптор не будет закрыт. `du` обходит дерево каталогов, поэтому никогда
не видит эти блоки. `df` сообщает реальное использование блоков файловой
системы, поэтому видит. Разница между ними — это именно оно: удалённые,
но открытые файлы.

Распространённые причины:

- сервис пишет логи в файл, который был удалён наивным кроном очистки или
  ротацией логов, не отправившей процессу `HUP`/`USR1` после этого — он
  продолжает писать в пустоту, бесконечно увеличиваясь в размере
- упавший/перезапущенный процесс утёк fd на большой временный файл
- скомпрометированный процесс или контейнер намеренно делает `open()` файла,
  затем сразу `unlink()`, чтобы скрыть полезную нагрузку/данные эксфильтрации
  от `find`, `du`, антивирусного сканирования каталогов и обычного `ls`

Единственный надёжный способ найти их — `/proc/<pid>/fd/*`, где ядро добавляет
к цели симлинка суффикс ` (deleted)`. `hsedd` автоматизирует это сканирование
(параллельно по рабочим потокам — см. ниже) и предоставляет ещё пять
возможностей через свой сокетный протокол:

- **`SCAN [min_size] [pid] [uid]`** — обойти `/proc`, перечислить каждый
  удалённый-но-открытый fd; опционально ограничить одним процессом или uid
- **`STATS [min_size] [uid]`** — та же логика сопоставления, что у `SCAN`, но
  только сводные `count`/`total_bytes` — пропускает разрешение
  имени пользователя/cmdline, поэтому стоит заметно дешевле на хосте со
  множеством подходящих fd. Предназначена для проверки мониторингом («скрыто
  ли сейчас больше 1ГБ?»), которой не нужна полная разбивка.
- **`TRUNCATE <pid> <fd>`** — переоткрыть тот же inode через
  `/proc/<pid>/fd/<fd>` и обрезать его до нуля с помощью `ftruncate`,
  немедленно освобождая дисковые блоки, без сигналов, паузы или перезапуска
  процесса
- **`HUP <pid>`** — вежливый запрос: попросить процесс самостоятельно
  переоткрыть свои файлы логов, если он поддерживает это соглашение
- **`KILL <pid>`** — SIGKILL, для случаев, когда процесс завис/не отвечает и
  не освобождает fd иным способом. Мгновенный, неперехватываемый, без
  корректного завершения; ядро освобождает каждый fd в рамках обычной
  процедуры завершения процесса. TUI требует ввода слова `KILL` для
  подтверждения — случайное нажатие клавиши не может активировать его.
- **`STREAM <pid> <fd>`** — подключиться через `ptrace(PTRACE_SEIZE)` и
  транслировать в реальном времени, какие именно байты записываются
  в этот fd прямо сейчас, из **любого** потока процесса — см. ниже.

## Многоядерное сканирование

`SCAN` и `STATS` делят список процессов между рабочими потоками (обход
`/proc/<pid>/fd` одного процесса не зависит от любого другого, поэтому это
масштабируется почти линейно с числом ядер на хосте со множеством
процессов). По умолчанию автоопределяет число онлайн-CPU; переопределяется
через `hsedd --scan-threads N` (применяется и к сокетным командам демона, и
к `--scan-once`/`--stats-once`). Фильтр по uid (`--uid N` в CLI, или
аргумент `uid` через сокет) проверяется до открытия каталога fd процесса
вообще, поэтому фильтрация по одному пользователю на мультитенантном хосте
полностью пропускает дорогую работу по каждому fd для всех остальных.

## Многопоточный STREAM

`STREAM` теперь следит за каждым потоком целевого процесса, включая
созданные **после** начала трансляции — эквивалент `strace -f`. Поскольку
потоки, созданные через pthreads, разделяют одну таблицу fd (`CLONE_FILES`),
записывать в интересующий вас fd может любой поток; однопоточный трейсер
(v1.1.0 и раньше) молча пропускал бы записи от любого потока, кроме того,
чей PID/TID вы случайно указали. Передайте основной PID процесса (то, что
`/proc/<pid>/task` перечисляет как лидера группы потоков), чтобы следить за
всем процессом; передайте TID конкретного потока (найденный тем же
способом), чтобы сузить трассировку до одного потока.

Вызовы `writev()` теперь тоже захватываются, а не только обычные
`write()`/`pwrite64()` — предпросмотр полезной нагрузки собирается из первых
до 16 сегментов iovec, склеенных по порядку, с тем же ограничением на размер
предпросмотра.

## Запуск TUI

```bash
hsed
hsed -min-size 10485760      # показать только записи размером >= 10 МиБ
hsed -pid 4821                 # проверить только один процесс
hsed -uid 1000                  # только скрытые файлы этого uid
hsed -socket /run/hsed.sock  # указать нестандартный сокет демона
```

`hsed` подключается к сокету `hsedd` — `$HSED_SOCKET`, иначе `/run/hsed.sock`,
если вы root, иначе `/tmp/hsed-<uid>.sock`. Если подключиться не удаётся,
он сообщает, как именно запустить демон, и завершает работу, а не делает вид,
что работает без данных.

## Горячие клавиши

| Клавиша | Действие                                                                     |
| ------- | ------------------------------------------------------------------------------ |
| `r`     | Немедленно пересканировать                                                   |
| `/`     | Фильтровать таблицу по подстроке имени процесса или пути                     |
| `Enter` | Открыть детали + действия для выбранной строки                               |
| `v`     | Скрытое дерево файловой системы (пути, восстановленные из удалённых записей) |
| `q`     | Выйти                                                                        |

Внутри панели деталей:

| Клавиша | Действие                                                   |
| ------- | -------------------------------------------------------------- |
| `s`     | Транслировать живую запись в этот fd (ptrace, через hsedd) |
| `t`     | Обрезать скрытый файл, немедленно вернуть его место        |
| `h`     | Отправить SIGHUP, попросив процесс переоткрыть свои файлы  |
| `k`     | SIGKILL процессу (введите `KILL` для подтверждения)        |
| `Esc`   | Закрыть                                                    |

> Скриншоты (`scan.png`, `truncated.png`, `hup.png`, `killing.png`) — от
> Python TUI версии v1.0.0. Go TUI выглядит и ведёт себя так же (тот же
> макет, те же горячие клавиши), но новые скриншоты ещё не переснимались —
> заменю с радостью, как только появятся свежие.

## Важные эксплуатационные замечания

**Truncate — это низкоуровневая операция.** Она безопасна для частого случая —
файла лога, дописываемого только в конец, в который процесс просто продолжает
писать последовательно. Она **не** безопасна, в общем, для:

- файлов, которые процесс отобразил в память через `mmap()` (обрезание под
  живым отображением может вызвать `SIGBUS` при следующем обращении процесса
  к этой области)
- файлов с произвольным доступом, а не только с дописыванием в конец (файлы
  данных баз данных, WAL/redo-логи, индексы B-tree, что-либо с определённой
  бинарной структурой)

Для промышленной базы данных или чего-то подобного предпочтите `h` (SIGHUP /
корректное переоткрытие) в первую очередь. Используйте `t` (truncate) только
на подтверждённо дописываемых в конец лог-файлах, и `k` (SIGKILL) только когда
процесс действительно завис и супервизор (systemd, среда выполнения контейнеров
и т.п.) перезапустит его — не как рутинный способ освободить место. Если
сомневаетесь, сначала протестируйте на идентичном бинарнике/версии на
промежуточной реплике.

**Стриминг использует ptrace.** `hsedd` подключается с `PTRACE_SEIZE` (который,
в отличие от `PTRACE_ATTACH`, не останавливает цель как побочный эффект) и лишь
кратковременно прерывает каждый поток на его трассируемых системных вызовах
`write`/`pwrite64`/`writev` — для типичного демона это незаметно, но для
чувствительных к задержкам рабочих нагрузок может иметь значение. Требуется
root или `CAP_SYS_PTRACE`, а также `/proc/sys/kernel/yama/ptrace_scope`,
разрешающая подключение (на root эта настройка не влияет).

**Поддержка архитектур:** x86_64 реализован и проверен на реальном железе,
включая под реальной нагрузкой (повторяющиеся быстрые циклы подключения/
отключения, многопоточные цели, динамическое создание потоков). aarch64
имеет собственную ветку доступа к регистрам (см. `backend/src/tracer.c`) и
чисто кросс-компилируется; `SCAN`/`TRUNCATE`/`HUP`/`KILL`/`STATS` проверены
под эмуляцией QEMU user-mode на реальных процессах и работают там корректно,
но эмуляция ptrace в QEMU user-mode известна своей ненадёжностью именно для
такого пошагового отслеживания системных вызовов, которым занимается
`STREAM`, поэтому именно путь `STREAM` **не** подтверждён на реальном железе
ARM64 — если попробуете на настоящем aarch64-железе, отчёт (в любую сторону)
будет по-настоящему полезен.

**Этот инструмент только обнаруживает и позволяет действовать на уже открытые
fd.** Он не обнаруживает руткиты, не перехватывает ядро и не скрывает/показывает
что-либо сам — это сторона чтения, стандартная инспекция `/proc` (те же данные,
что дают `lsof -a +L1` или `find /proc/*/fd -lname '* (deleted)'`), и сторона
записи, использующая только `ftruncate`, `kill` и стандартный `ptrace(2)`.
Никаких новых возможностей уровня ядра не вводится сверх того, что уже дают
root/`CAP_SYS_PTRACE` — те же привилегии, которые уже нужны для `strace -p` и
`gdb -p`.

## Протокол

Если вы хотите управлять `hsedd` не из этого TUI, формат передачи данных —
это небольшой протокол с текстовыми командами, разделяемыми переводом строки, и
ответами в JSON-lines через Unix-сокет — точную схему смотрите в
`backend/src/protocol.h` (`SCAN`, `STATS`, `TRUNCATE`, `HUP`, `KILL`, `STREAM`,
`PING`, `QUIT`). `tui/client` — небольшая самодостаточная референсная
реализация, если Go читать удобнее, чем C.

## Удаление

```bash
sudo systemctl disable --now hsed   # если вы включили службу
sudo dpkg -r hsed                    # удалить
sudo dpkg -P hsed                    # удалить + очистить сокет/pid-файл времени выполнения
```

## Структура проекта

```
backend/                       # демон на C (бинарник: hsedd)
├── src/
│   ├── proc_scan.c/.h          # обход /proc/*/fd -> список hsed_entry_t, параллельно + фильтр по uid
│   ├── reclaim.c/.h            # ftruncate через /proc/pid/fd/N, kill(sig)
│   ├── tracer.c/.h             # ptrace(SEIZE/INTERRUPT) стример живой записи,
│   │                            # слежение за потоками, захват writev (x86_64 + aarch64)
│   ├── protocol.c/.h           # форматирование JSON-lines, помощники строкового ввода-вывода
│   ├── server.c/.h             # цикл приёма Unix-сокета, по одному потоку на соединение
│   ├── base64.c/.h             # бинарно-безопасная передача захваченных байтов write()
│   ├── util.c/.h                # логирование, небольшие строковые помощники
│   └── hsed.c                   # main(): аргументы, демонизация, обработка сигналов
├── systemd/hsed.service
└── Makefile                     # `make` (нативно) / `make arm64` (кросс-сборка)

tui/                            # Go TUI (единственный статический бинарник, команда: hsed)
├── go.mod / go.sum
├── main.go                       # точка входа CLI
├── client/                        # Client / StreamSession — общается с сокетом hsedd
└── ui/                             # экраны bubbletea (таблица, детали, дерево, стрим, подтверждения)

packaging/
├── build-deb.sh                 # сборка hsedd + hsed, компоновка .deb (amd64 или arm64)
└── pkg-static/DEBIAN/            # control, postinst, postrm
```
