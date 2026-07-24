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

chmod +x ./build-deb.sh                  # if permission denied
sudo ./build-deb.sh
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

<p align="center"><img src="https://github.com/memspy/hsed/blob/main/scan.png" width="80%"/></p>

<p align="center"><img src="https://github.com/memspy/hsed/blob/main/truncated.png" width="80%"/></p>

<p align="center"><img src="https://github.com/memspy/hsed/blob/main/hup.png" width="80%"/></p>

<p align="center"><img src="https://github.com/memspy/hsed/blob/main/killing.png" width="80%"/></p>

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

# Hidden Space Explorer (Russian README)

Инструмент для классического инцидента: «`df -h` показывает 100%, `du -sh` не находит ничего».

**Архитектура:** привилегированная часть — сканирование `/proc`, `ftruncate`,
сигналы, `ptrace` — представляет собой автономный демон на C (`hsedd`), который
работает в фоне или по требованию и принимает команды через Unix-сокет. `hsed` —
это интерактивный TUI (Python/Textual), тонкий клиент, который подключается к
нему. Python никогда не обращается к `/proc`, `ftruncate`, `kill` или `ptrace`
напрямую — все привилегированные операции выполняются в `hsedd`.

```
┌──────────────┐  SCAN / TRUNCATE / HUP / KILL / STREAM   ┌───────────────────────┐
│  hsed         │ ────────────────────────────────────────► │  hsedd (демон на C)    │
│  (Python TUI) │            ответы в JSON-lines             │  /proc, ftruncate,     │
└──────────────┘ ◄──────────────────────────────────────── │  kill, ptrace(SEIZE)   │
                          Unix domain socket                 └───────────────────────┘
```

## Установка

```bash
sudo dpkg -i hsed_1.0.0_amd64.deb
```

Вот и всё — `hsed` и `hsedd` оказываются в `PATH`. Python TUI работает с
вендорной копией своих зависимостей, входящей в состав пакета, поэтому
ничего дополнительно устанавливать не нужно (ни pip, ни доступ в интернет).

Запуск бэкенда:

```bash
sudo systemctl enable --now hsed     # если доступен systemd, или:
sudo hsedd                           # фоновый демон
sudo hsedd --foreground              # остаётся прикреплённым к терминалу (Ctrl-C для остановки)
```

Затем просто запустите:

```bash
sudo hsed
```

### Самостоятельная сборка пакета

```bash
cd backend && make                       # собираем hsed
cd ../packaging && ./build-deb.sh        # компоновка + сборка

chmod +x ./build-deb.sh                  # если ошибка доступ запрещен
sudo ./build-deb.sh
```

Подробности того, что попадает в пакет, смотрите в `packaging/build-deb.sh`
(те же шаги, что и выше: скомпилировать `hsedd`, вендорить зависимости Python,
разложить дерево `usr/`/`lib/`, `dpkg-deb --build`).

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
и предоставляет ещё четыре возможности через свой сокетный протокол:

- **`SCAN`** — обойти `/proc`, перечислить каждый удалённый-но-открытый fd
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
  транслировать в реальном времени, какие именно байты процесс записывает
  в этот fd прямо сейчас

## Запуск TUI

```bash
sudo hsed
sudo hsed --min-size 10485760      # показать только записи размером >= 10 МиБ
sudo hsed --pid 4821                # проверить только один процесс
sudo hsed --socket /run/hsed.sock  # указать нестандартный сокет демона
```

`hsed` подключается к сокету `hsedd` — `$HSED_SOCKET`, иначе `/run/hsed.sock`,
если вы root, иначе `/tmp/hsed-<uid>.sock`. Если подключиться не удаётся,
он сообщает, как именно запустить демон, и завершает работу, а не делает вид,
что работает без данных.

## Горячие клавиши

| Клавиша | Действие                                                     |
|---------|--------------------------------------------------------------|
| `r`     | Немедленно пересканировать                                    |
| `/`     | Фильтровать таблицу по подстроке имени процесса или пути      |
| `Enter` | Открыть детали + действия для выбранной строки                |
| `v`     | Скрытое дерево файловой системы (пути, восстановленные из удалённых записей) |
| `q`     | Выйти                                                         |

Внутри панели деталей:

| Клавиша | Действие                                                   |
|---------|------------------------------------------------------------|
| `s`     | Транслировать живую запись в этот fd (ptrace, через hsedd) |
| `t`     | Обрезать скрытый файл, немедленно вернуть его место        |
| `h`     | Отправить SIGHUP, попросив процесс переоткрыть свои файлы  |
| `k`     | SIGKILL процессу (введите `KILL` для подтверждения)         |
| `Esc`   | Закрыть                                                     |

<p align="center"><img src="https://github.com/memspy/hsed/blob/main/scan.png" width="80%"/></p>

<p align="center"><img src="https://github.com/memspy/hsed/blob/main/truncated.png" width="80%"/></p>

<p align="center"><img src="https://github.com/memspy/hsed/blob/main/hup.png" width="80%"/></p>

<p align="center"><img src="https://github.com/memspy/hsed/blob/main/killing.png" width="80%"/></p>

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
кратковременно прерывает её на каждом трассируемом системном вызове
`write`/`pwrite64`/`writev` — для типичного демона это незаметно, но для
чувствительных к задержкам рабочих нагрузок может иметь значение. Требуется
root или `CAP_SYS_PTRACE`, а также `/proc/sys/kernel/yama/ptrace_scope`,
разрешающая подключение (на root эта настройка не влияет).

**Область стриминга в этой версии:** только x86_64, и трассируется ровно тот
PID/TID, который вы указали — он не следует за потоками, созданными `clone()`,
в отличие от `strace -f`. Если fd может быть записан другим потоком
многопоточной цели, найдите TID этого потока в `/proc/<pid>/task/` и передайте
его как pid. Вызовы `writev()` сообщаются (системный вызов + длина), но
предпросмотр полезной нагрузки не захватывается в этой версии — обычные
`write()`/`pwrite64()` захватываются.

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
`backend/src/protocol.h` (`SCAN`, `TRUNCATE`, `HUP`, `KILL`, `STREAM`, `PING`,
`QUIT`).

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
│   ├── proc_scan.c/.h          # обход /proc/*/fd -> список hsed_entry_t
│   ├── reclaim.c/.h            # ftruncate через /proc/pid/fd/N, kill(sig)
│   ├── tracer.c/.h             # ptrace(SEIZE/INTERRUPT) стример живой записи (x86_64)
│   ├── protocol.c/.h           # форматирование JSON-lines, помощники строкового ввода-вывода
│   ├── server.c/.h             # цикл приёма Unix-сокета, по одному потоку на соединение
│   ├── base64.c/.h             # бинарно-безопасная передача захваченных байтов write()
│   ├── util.c/.h                # логирование, небольшие строковые помощники
│   └── hsed.c                   # main(): аргументы, демонизация, обработка сигналов
├── systemd/hsed.service
└── Makefile

hidden_space_explorer/         # Python TUI (тонкий клиент, команда: hsed)
├── client.py                   # HsedClient / StreamSession — общается с сокетом hsedd
├── app.py                      # Textual UI: таблица, вид дерева, модальное окно деталей, экран стрима
└── __main__.py                 # точка входа CLI

packaging/
├── build-deb.sh                 # сборка hsedd, вендоринг зависимостей Python, компоновка .deb
└── hsed_1.0.0_amd64/DEBIAN/     # control, postinst, postrm
```
