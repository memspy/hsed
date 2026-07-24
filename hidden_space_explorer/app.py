from __future__ import annotations

from datetime import datetime
from typing import Optional

from textual import work
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Vertical
from textual.screen import ModalScreen, Screen
from textual.widgets import DataTable, Footer, Header, Input, Label, RichLog, Static, Tree

from . import client


def human_size(n: float) -> str:
    for unit in ("B", "K", "M", "G", "T"):
        if n < 1024 or unit == "T":
            return f"{n:.0f}{unit}" if unit == "B" else f"{n:.1f}{unit}"
        n /= 1024
    return f"{n:.1f}P"



class ConfirmScreen(ModalScreen[bool]):
    BINDINGS = [
        Binding("y", "confirm", "Yes"),
        Binding("n", "cancel", "No"),
        Binding("escape", "cancel", "Cancel"),
    ]

    def __init__(self, question: str) -> None:
        super().__init__()
        self.question = question

    def compose(self) -> ComposeResult:
        with Vertical(id="confirm-box"):
            yield Label(self.question, id="confirm-question")
            yield Label("[y] Yes    [n / Esc] Cancel", id="confirm-hint")

    def action_confirm(self) -> None:
        self.dismiss(True)

    def action_cancel(self) -> None:
        self.dismiss(False)



class DangerConfirmScreen(ModalScreen[bool]):
    BINDINGS = [Binding("escape", "cancel", "Cancel")]

    def __init__(self, question: str, confirm_word: str = "KILL") -> None:
        super().__init__()
        self.question = question
        self.confirm_word = confirm_word

    def compose(self) -> ComposeResult:
        with Vertical(id="confirm-box"):
            yield Label(self.question, id="confirm-question")
            yield Label(
                f"Type {self.confirm_word} and press Enter to confirm, or Esc to cancel.",
                id="confirm-hint",
            )
            yield Input(placeholder=self.confirm_word, id="confirm-input")

    def on_mount(self) -> None:
        self.query_one(Input).focus()

    def on_input_submitted(self, event: Input.Submitted) -> None:
        self.dismiss(event.value.strip() == self.confirm_word)

    def action_cancel(self) -> None:
        self.dismiss(False)


class StreamScreen(Screen):
    BINDINGS = [Binding("escape", "back", "Back")]

    def __init__(self, entry: client.HiddenEntry) -> None:
        super().__init__()
        self.entry = entry
        self._session: Optional[client.StreamSession] = None

    def compose(self) -> ComposeResult:
        yield Header()
        yield Static(
            f"Live write stream -> PID {self.entry.pid} ({self.entry.comm})  "
            f"fd={self.entry.fd}   was: {self.entry.path}",
            id="stream-title",
        )
        yield RichLog(id="stream-log", wrap=True, highlight=False, markup=False)
        yield Footer()

    def on_mount(self) -> None:
        log = self.query_one("#stream-log", RichLog)
        try:
            self._session = client.HsedClient().open_stream(self.entry.pid, self.entry.fd)
        except client.HsedError as e:
            log.write(f"[!] {e}")
            return
        log.write(f"[+] Attached to PID {self.entry.pid} via ptrace (hsed daemon) ...")
        log.write(f"[+] Watching write()/pwrite64()/writev() on fd {self.entry.fd} only\n")
        self.run_stream_worker()

    def on_unmount(self) -> None:
        if self._session is not None:
            self._session.close()

    def action_back(self) -> None:
        if self._session is not None:
            self._session.close()
        self.app.pop_screen()

    @work(thread=True, exclusive=True)
    def run_stream_worker(self) -> None:
        log = self.query_one("#stream-log", RichLog)
        assert self._session is not None
        try:
            for ev in self._session.events():
                ts = datetime.now().strftime("%H:%M:%S")
                preview = ev.data.decode("utf-8", errors="replace")
                kind = "writev" if ev.is_writev else "write"
                note = "" if ev.captured >= ev.ret else f" (showing first {ev.captured}B)"
                self.app.call_from_thread(
                    log.write, f"[{ts}] {kind} {ev.ret}B{note}: {preview!r}"
                )
        except client.HsedError as e:
            self.app.call_from_thread(log.write, f"[!] {e}")
        except OSError:
            pass  # session closed from the UI thread (Escape) — expected, not an error
        except Exception as e:  # noqa: BLE001 - surface any failure in the log, don't crash the TUI
            self.app.call_from_thread(log.write, f"[!] Stream error: {e}")



class DetailScreen(ModalScreen[Optional[str]]):
    BINDINGS = [
        Binding("escape", "dismiss_none", "Close"),
        Binding("s", "do_stream", "Stream writes"),
        Binding("t", "do_truncate", "Truncate (reclaim space)"),
        Binding("h", "do_hup", "Send SIGHUP"),
        Binding("k", "do_kill", "SIGKILL process"),
    ]

    def __init__(self, entry: client.HiddenEntry) -> None:
        super().__init__()
        self.entry = entry

    def compose(self) -> ComposeResult:
        e = self.entry
        with Vertical(id="detail-box"):
            yield Label(f"PID {e.pid} - {e.comm}", id="detail-title")
            yield Static(
                f"cmdline:  {e.cmdline}\n"
                f"fd:       {e.fd}      mode: {e.mode}      owner: {e.user}\n"
                f"was at:   {e.path}\n"
                f"size:     {human_size(e.size)}  ({e.size} bytes)\n"
                f"inode:    {e.inode}   device: {e.dev_major}:{e.dev_minor}\n"
                f"mtime:    {datetime.fromtimestamp(e.mtime):%Y-%m-%d %H:%M:%S}\n",
                id="detail-body",
            )
            yield Static(
                "[s] Stream live writes    [t] Truncate & reclaim    "
                "[h] SIGHUP (graceful reopen)    [k] SIGKILL process    [Esc] Close",
                id="detail-hint",
            )

    def action_dismiss_none(self) -> None:
        self.dismiss(None)

    def action_do_stream(self) -> None:
        self.app.push_screen(StreamScreen(self.entry))

    @work
    async def action_do_truncate(self) -> None:
        confirmed = await self.app.push_screen_wait(
            ConfirmScreen(
                f"Truncate hidden file held by PID {self.entry.pid} fd {self.entry.fd} "
                f"({human_size(self.entry.size)})?\n\n"
                f"The process will NOT be restarted or signaled. If it accesses this "
                f"file via mmap or random-offset seeks (databases, WAL, indexes) this "
                f"can trigger SIGBUS or corruption. Prefer SIGHUP first for daemons "
                f"that support graceful log reopen."
            )
        )
        if not confirmed:
            return
        try:
            freed = client.HsedClient().truncate(self.entry.pid, self.entry.fd)
            self.dismiss(f"Freed {human_size(freed)} (PID {self.entry.pid}, fd {self.entry.fd})")
        except client.HsedError as e:
            self.dismiss(f"Error: {e}")

    @work
    async def action_do_hup(self) -> None:
        confirmed = await self.app.push_screen_wait(
            ConfirmScreen(
                f"Send SIGHUP to PID {self.entry.pid} ({self.entry.comm})?\n\n"
                f"Well-behaved daemons close and reopen their log files on this "
                f"signal, releasing the unlinked inode themselves."
            )
        )
        if not confirmed:
            return
        try:
            client.HsedClient().hup(self.entry.pid)
            self.dismiss(f"SIGHUP sent to PID {self.entry.pid}")
        except client.HsedError as e:
            self.dismiss(f"Error: {e}")

    @work
    async def action_do_kill(self) -> None:
        confirmed = await self.app.push_screen_wait(
            DangerConfirmScreen(
                f"SIGKILL PID {self.entry.pid} ({self.entry.comm})?\n\n"
                f"This terminates the process IMMEDIATELY — it cannot be caught, "
                f"blocked, or handled, so there is no graceful shutdown and no "
                f"chance to flush buffers or finish in-flight work. Only the "
                f"kernel's normal process teardown runs (all fds close, this "
                f"hidden file's space is freed as a side effect). Use this when "
                f"the process is hung or unresponsive to SIGHUP and a supervisor "
                f"(systemd, container runtime, etc.) will restart it — not as a "
                f"routine way to free space."
            )
        )
        if not confirmed:
            return
        try:
            client.HsedClient().kill(self.entry.pid)
            self.dismiss(f"SIGKILL sent to PID {self.entry.pid}")
        except client.HsedError as e:
            self.dismiss(f"Error: {e}")



class HiddenTreeScreen(Screen):
    BINDINGS = [Binding("escape", "app.pop_screen", "Back to table")]

    def __init__(self, entries: list[client.HiddenEntry]) -> None:
        super().__init__()
        self.entries = entries

    def compose(self) -> ComposeResult:
        yield Header()
        yield Static(
            "Hidden filesystem — reconstructed from paths that no longer "
            "exist on disk, but are still backing live process fds. "
            "Enter on a file expands to its process(es) and actions.",
            id="tree-title",
        )
        yield Tree("/ (hidden space)", id="hidden-tree")
        yield Footer()

    def on_mount(self) -> None:
        tree = self.query_one(Tree)
        tree.root.expand()
        path_tree = client.build_path_tree(self.entries)
        self._populate(tree.root, path_tree)

    def _populate(self, node, subtree: dict) -> None:
        for name, value in sorted(subtree.items()):
            if isinstance(value, dict):
                child = node.add(name, expand=False)
                self._populate(child, value)
            else:
                # `value` is a list[HiddenEntry] — usually one entry, but the
                # same deleted path can be held open by more than one fd.
                total = sum(e.size for e in value)
                count_note = "" if len(value) == 1 else f", {len(value)} fds"
                node.add_leaf(f"{name}  [{human_size(total)}{count_note}]", data=value)

    def on_tree_node_selected(self, event: Tree.NodeSelected) -> None:
        entries = event.node.data
        if not entries:
            return  # a directory node, not a file leaf
        entry = max(entries, default=None, key=lambda e: e.size)
        if entry is None:
            return
        if len(entries) > 1:
            self.app.notify(
                f"{len(entries)} file descriptors hold this path open — "
                f"showing the largest (PID {entry.pid}, fd {entry.fd})",
                timeout=5,
            )
        self.app.push_screen(DetailScreen(entry))



class HiddenSpaceExplorer(App):
    """Main screen: table of every unlinked-but-open file, live-refreshed."""

    CSS = """
    #summary { height: 1; background: $panel; color: $text; padding: 0 1; }
    #detail-box, #confirm-box {
        background: $panel; border: thick $accent; width: 74%; padding: 1 2;
    }
    #confirm-box { width: 60%; }
    #detail-title { text-style: bold; color: $accent; }
    #detail-hint { color: $text-muted; margin-top: 1; }
    DataTable { height: 1fr; }
    """

    BINDINGS = [
        Binding("q", "quit", "Quit"),
        Binding("r", "rescan", "Rescan"),
        Binding("/", "focus_filter", "Filter"),
        Binding("v", "toggle_tree", "Tree view"),
    ]

    def __init__(
        self,
        min_size: int = 0,
        interval: float = 5.0,
        only_pid: Optional[int] = None,
        socket_path: Optional[str] = None,
    ) -> None:
        super().__init__()
        self.min_size = min_size
        self.interval = interval
        self.only_pid = only_pid
        self.client = client.HsedClient(socket_path=socket_path)
        self.entries: list[client.HiddenEntry] = []
        self._filter = ""
        self._last_error: Optional[str] = None

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        yield Static("Connecting to hsed ...", id="summary")
        yield Input(
            placeholder="Filter by process name / path (Enter to apply, Esc to clear)",
            id="filter",
        )
        yield DataTable(id="table", zebra_stripes=True, cursor_type="row")
        yield Footer()

    def on_mount(self) -> None:
        table = self.query_one(DataTable)
        table.add_columns("PID", "Process", "FD", "Mode", "Size", "Owner", "Was at")
        self.query_one("#filter", Input).display = False
        table.focus()
        self.action_rescan()
        self.set_interval(self.interval, self.action_rescan)

    def action_toggle_tree(self) -> None:
        self.push_screen(HiddenTreeScreen(self.entries))

    def action_focus_filter(self) -> None:
        f = self.query_one("#filter", Input)
        f.display = True
        f.focus()

    def on_input_submitted(self, event: Input.Submitted) -> None:
        if event.input.id == "filter":
            self._filter = event.value.strip().lower()
            self._render_table()
            event.input.display = False
            self.query_one(DataTable).focus()

    @work(exclusive=True, thread=True)
    def action_rescan(self) -> None:
        try:
            entries, _total = self.client.scan(min_size=self.min_size, only_pid=self.only_pid or 0)
        except client.HsedError as e:
            self.call_from_thread(self._on_scan_error, str(e))
            return
        self.call_from_thread(self._on_scanned, entries)

    def _on_scan_error(self, message: str) -> None:
        self._last_error = message
        summary = self.query_one("#summary", Static)
        summary.update(f" [!] {message}")

    def _on_scanned(self, entries: list[client.HiddenEntry]) -> None:
        self._last_error = None
        self.entries = entries
        self._render_table()

    def _render_table(self) -> None:
        table = self.query_one(DataTable)
        table.clear()
        total = sum(e.size for e in self.entries)
        shown = 0
        for e in self.entries:
            if self._filter and (
                self._filter not in e.comm.lower()
                and self._filter not in e.path.lower()
            ):
                continue
            shown += 1
            table.add_row(
                str(e.pid), e.comm, str(e.fd), e.mode,
                human_size(e.size), e.user, e.path,
                key=e.key,
            )
        summary = self.query_one("#summary", Static)
        summary.update(
            f" Hidden files: {len(self.entries)} (showing {shown})   "
            f"Reclaimable space invisible to du/df's directory walk: {human_size(total)}   "
            f"[r] rescan  [/] filter  [Enter] details  [v] tree view  [q] quit"
        )

    def on_data_table_row_selected(self, event: DataTable.RowSelected) -> None:
        self._open_detail_for_key(event.row_key)

    def _open_detail_for_key(self, row_key) -> None:
        if row_key is None or row_key.value is None or not self.entries:
            return
        pid_s, fd_s = str(row_key.value).split(":")
        pid, fd = int(pid_s), int(fd_s)
        entry = next((e for e in self.entries if e.pid == pid and e.fd == fd), None)
        if entry is None:
            return

        def _after(result: Optional[str]) -> None:
            if result:
                self.notify(result, timeout=6)
                self.action_rescan()

        self.push_screen(DetailScreen(entry), _after)


def run(
    min_size: int = 0,
    interval: float = 5.0,
    only_pid: Optional[int] = None,
    socket_path: Optional[str] = None,
) -> None:
    c = client.HsedClient(socket_path=socket_path)
    try:
        c.ping()
    except client.HsedError as e:
        print(f"[!] {e}")
        print(
            "\nThe hsed daemon isn't reachable. Start it first:\n"
            "    sudo hsedd                          (background daemon)\n"
            "    sudo hsedd --foreground              (stays attached, e.g. under systemd)\n"
            "    sudo systemctl start hsed            (if installed via the .deb)\n"
        )
        raise SystemExit(1)
    HiddenSpaceExplorer(
        min_size=min_size, interval=interval, only_pid=only_pid, socket_path=socket_path
    ).run()
