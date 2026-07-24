"""CLI entry point: `python -m hidden_space_explorer` or the installed script."""
from __future__ import annotations

import argparse

from .app import run


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="hsed",
        description=(
            "TUI explorer for invisible disk usage: finds files that were "
            "deleted (unlinked) but are still held open by a process fd, "
            "which is why `df` shows the space used while `du` can't find it."
        ),
    )
    parser.add_argument(
        "--min-size",
        type=int,
        default=0,
        metavar="BYTES",
        help="Only show entries at least this many bytes (default: 0, show everything)",
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=5.0,
        metavar="SECONDS",
        help="Auto-rescan interval in seconds (default: 5)",
    )
    parser.add_argument(
        "--pid",
        type=int,
        default=None,
        help="Restrict scanning to a single PID",
    )
    parser.add_argument(
        "--socket",
        type=str,
        default=None,
        metavar="PATH",
        help=(
            "Path to the hsed daemon's Unix socket. Defaults to $HSED_SOCKET, "
            "or /run/hsed.sock as root, or /tmp/hsed-<uid>.sock otherwise — "
            "matching hsed's own default (see hsed --help)."
        ),
    )
    args = parser.parse_args()
    run(min_size=args.min_size, interval=args.interval, only_pid=args.pid, socket_path=args.socket)


if __name__ == "__main__":
    main()
