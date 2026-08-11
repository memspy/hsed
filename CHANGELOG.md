# Changelog

## 1.1.1

- **Parallel SCAN/STATS.** The `/proc` walk now splits across worker
  threads (auto-detects the online CPU count, override with
  `hsedd --scan-threads N`), since each process's fd walk is independent
  of every other's — scales close to linearly with core count on hosts
  with many processes. Verified for correctness across thread counts
  (1/2/4/8/16 all produce identical results) and under load (~300
  concurrent processes).
- **STREAM now follows every thread of the target process**, including
  ones created *after* streaming starts (`PTRACE_O_TRACECLONE`) —
  equivalent to `strace -f`. Previously (v1.1.0) it only traced the exact
  PID/TID given, silently missing writes from any other thread of a
  multi-threaded process even though pthreads share one fd table. Tested
  against background-thread writers and dynamically-spawned threads.
- **`writev()` payload capture.** Previously only the syscall and length
  were reported for `writev()`; now the payload preview is reassembled
  from up to the first 16 iovec segments, concatenated in order, same as
  plain `write()`/`pwrite64()`.
- **uid filtering** on `SCAN`/`STATS` (checked before opening a process's
  fd directory at all, so it's a real cost saving on multi-tenant hosts,
  not just a display filter) and a new **`STATS`** command for cheap
  monitoring-style checks (count + total bytes, no per-entry string
  resolution).
- **aarch64 cross-build** (`make arm64` / `./build-deb.sh arm64`).
  SCAN/TRUNCATE/HUP/KILL/STATS are portable syscalls, verified under QEMU
  user-mode emulation against real processes. STREAM has its own aarch64
  register-access path but has NOT been confirmed on real ARM64 hardware
  — QEMU user-mode's ptrace emulation is known to be unreliable for
  syscall-stepping, so testing under it wouldn't have meant much either
  way. Reports from real hardware welcome.
- Fixed a real race found while stress-testing the multi-thread rewrite:
  rapid back-to-back STREAM sessions against the same target could
  transiently fail to attach (`PTRACE_SEIZE` racing a just-finished
  previous session's detach, which has ~15ms of inherent latency via the
  poll-based disconnect check). Fixed with a short bounded retry; verified
  with 30 zero-delay reconnect cycles in a row.

## 1.1.0

- **TUI rewritten from Python/Textual to Go** (`charmbracelet/bubbletea` +
  `bubbles` + `lipgloss`). `hsed` is now a single statically-linked binary
  (`CGO_ENABLED=0`) with zero runtime dependencies — no Python, no
  vendored packages, nothing to install beyond the binary itself.
- Added **`KILL <pid>`** (SIGKILL) as a fourth action alongside
  `TRUNCATE`/`HUP`, for processes that are hung/unresponsive and won't
  release a hidden fd any other way. The TUI requires typing the word
  `KILL` to confirm, not a single y/n keypress, since SIGKILL can't be
  caught or undone.
- The daemon binary is now `hsedd` (previously `hsed`), following the
  `sshd`/`nginx` naming convention for a privileged background service;
  `hsed` is reserved for the interactive command.
- `hsed_1.1.0_amd64.deb` shrank from ~15MB installed (Python + vendored
  deps) to ~4MB (two native binaries, no vendoring needed).
- No changes to the daemon (`hsedd`) or the wire protocol between them —
  `backend/src/protocol.h` is unchanged, aside from documenting `KILL`.

## 1.0.0

- Initial release: C daemon (`hsedd`-equivalent) + Python/Textual TUI.
  `SCAN`, `TRUNCATE`, `HUP`, `STREAM` over a Unix socket.

---

# Список изменений

## 1.1.1

- **Параллельные `SCAN`/`STATS`.** Обход `/proc` теперь распределяется по
  рабочим потокам (автоопределение числа онлайн-CPU, переопределяется
  через `hsedd --scan-threads N`), поскольку обход fd одного процесса не
  зависит от любого другого — масштабируется почти линейно с числом ядер
  на хостах со множеством процессов. Проверена корректность на разном
  числе потоков (1/2/4/8/16 дают идентичные результаты) и под нагрузкой
  (~300 одновременных процессов).
- **`STREAM` теперь следит за каждым потоком целевого процесса**, включая
  созданные **после** начала трансляции (`PTRACE_O_TRACECLONE`) —
  эквивалент `strace -f`. Раньше (v1.1.0) трассировался только тот
  PID/TID, что был указан, молча пропуская записи от любого другого
  потока многопоточного процесса, хотя потоки pthreads разделяют одну
  таблицу fd. Протестировано на писателях из фонового потока и динамически
  порождаемых потоках.
- **Захват полезной нагрузки `writev()`.** Раньше для `writev()`
  сообщались только системный вызов и длина; теперь предпросмотр полезной
  нагрузки собирается из первых до 16 сегментов iovec, склеенных по
  порядку — так же, как для обычных `write()`/`pwrite64()`.
- **Фильтрация по uid** в `SCAN`/`STATS` (проверяется до открытия каталога
  fd процесса вообще, поэтому это реальная экономия на мультитенантных
  хостах, а не просто фильтр отображения) и новая команда **`STATS`** для
  дешёвых проверок в духе мониторинга (счётчик + суммарные байты, без
  разрешения строк на каждую запись).
- **Кросс-сборка под aarch64** (`make arm64` / `./build-deb.sh arm64`).
  `SCAN`/`TRUNCATE`/`HUP`/`KILL`/`STATS` — переносимые системные вызовы,
  проверены под эмуляцией QEMU user-mode на реальных процессах. `STREAM`
  имеет собственную ветку доступа к регистрам для aarch64, но **не**
  подтверждён на реальном железе ARM64 — эмуляция ptrace в QEMU user-mode
  известна своей ненадёжностью именно для пошагового отслеживания
  системных вызовов, поэтому тестирование под ней мало что доказало бы в
  любую сторону. Будем рады отчётам с реального железа.
- Исправлена реальная гонка, найденная при стресс-тестировании
  многопоточной переработки: быстрые последовательные сессии `STREAM` к
  одной и той же цели могли временно не подключиться (`PTRACE_SEIZE`
  гонялся с detach только что завершившейся предыдущей сессии, у которого
  есть ~15мс встроенной задержки через опрос отключения). Исправлено
  коротким ограниченным повтором попытки; проверено 30 циклами
  переподключения подряд без задержки.

## 1.1.0

- **TUI переписан с Python/Textual на Go** (`charmbracelet/bubbletea` +
  `bubbles` + `lipgloss`). `hsed` теперь единственный статически
  слинкованный бинарник (`CGO_ENABLED=0`) без единой рантайм-зависимости —
  ни Python, ни вендорных пакетов, ничего не нужно ставить кроме самого
  бинарника.
- Добавлена команда **`KILL <pid>`** (SIGKILL) как четвёртое действие
  наряду с `TRUNCATE`/`HUP` — для процессов, которые зависли/не отвечают
  и не освобождают скрытый fd иным способом. TUI требует ввести слово
  `KILL` целиком для подтверждения, а не одно нажатие y/n, поскольку
  SIGKILL нельзя перехватить или отменить.
- Бинарник демона теперь называется `hsedd` (раньше — `hsed`), по
  конвенции именования `sshd`/`nginx` для привилегированной фоновой
  службы; `hsed` закреплён за интерактивной командой.
- `hsed_1.1.0_amd64.deb` уменьшился с ~15MB установленного размера
  (Python + вендорные зависимости) до ~4MB (два нативных бинарника, без
  вендоринга).
- Демон (`hsedd`) и протокол передачи данных между компонентами не
  изменились — `backend/src/protocol.h` не менялся, кроме документирования
  `KILL`.

## 1.0.0

- Первый релиз: демон на C (эквивалент `hsedd`) + Python/Textual TUI.
  `SCAN`, `TRUNCATE`, `HUP`, `STREAM` через Unix-сокет.
