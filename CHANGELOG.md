# Changelog

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
