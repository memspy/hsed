package main

import (
	"flag"
	"fmt"
	"os"
	"time"

	tea "github.com/charmbracelet/bubbletea"

	"hsed/tui/client"
	"hsed/tui/ui"
)

func main() {
	var (
		minSize    = flag.Int64("min-size", 0, "Only show entries at least this many bytes")
		interval   = flag.Duration("interval", 5*time.Second, "Auto-rescan interval")
		onlyPID    = flag.Int("pid", 0, "Restrict scanning to a single PID")
		uidFilter  = flag.Int64("uid", client.UIDAny, "Only show entries owned by this uid (default: every user)")
		socketPath = flag.String("socket", "", "Path to the hsedd daemon's Unix socket "+
			"(default: $HSED_SOCKET, else /run/hsed.sock as root, else /tmp/hsed-<uid>.sock)")
	)
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "hsed — TUI explorer for invisible disk usage: finds files that were\n"+
			"deleted (unlinked) but are still held open by a process fd, which is why\n"+
			"`df` shows the space used while `du` can't find it.\n\nUsage:\n")
		flag.PrintDefaults()
	}
	flag.Parse()

	c := client.New(*socketPath)
	if err := c.Ping(); err != nil {
		fmt.Fprintf(os.Stderr, "[!] %v\n\n", err)
		fmt.Fprintln(os.Stderr, "The hsedd daemon isn't reachable. Start it first:")
		fmt.Fprintln(os.Stderr, "    sudo hsedd                    (background daemon)")
		fmt.Fprintln(os.Stderr, "    sudo hsedd --foreground        (stays attached, e.g. under systemd)")
		fmt.Fprintln(os.Stderr, "    sudo systemctl start hsed      (if installed via the .deb)")
		os.Exit(1)
	}

	root := ui.NewTableScreen(c, *minSize, *onlyPID, *uidFilter, *interval)
	app := ui.NewApp(c, root)

	p := tea.NewProgram(app, tea.WithAltScreen())
	_, err := p.Run()

	
	app.CloseAllSessions()

	if err != nil {
		fmt.Fprintln(os.Stderr, "hsed:", err)
		os.Exit(1)
	}
}
