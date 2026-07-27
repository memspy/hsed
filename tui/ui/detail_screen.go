package ui

import (
	"fmt"
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"

	"hsed/tui/client"
)

type actionResultMsg struct {
	err   error
	label string
}


type DetailScreen struct {
	client  *client.Client
	entry   client.HiddenEntry
	pending string // "" | "truncate" | "hup" | "kill"
	status  string

	width, height int
}

func NewDetailScreen(c *client.Client, e client.HiddenEntry) *DetailScreen {
	return &DetailScreen{client: c, entry: e}
}

func (s *DetailScreen) Init() tea.Cmd   { return nil }
func (s *DetailScreen) Resize(w, h int) { s.width, s.height = w, h }

func (s *DetailScreen) Update(msg tea.Msg) (Screen, tea.Cmd) {
	switch m := msg.(type) {
	case resultMsg:
		confirmed, _ := m.value.(bool)
		action := s.pending
		s.pending = ""
		if !confirmed {
			return s, nil
		}
		switch action {
		case "truncate":
			return s, s.truncateCmd()
		case "hup":
			return s, s.hupCmd()
		case "kill":
			return s, s.killCmd()
		}
		return s, nil

	case actionResultMsg:
		if m.err != nil {
			s.status = "Error: " + m.err.Error()
			return s, nil
		}
		return s, PopScreenWithResult(m.label)

	case tea.KeyMsg:
		switch m.String() {
		case "esc":
			return s, PopScreen()
		case "s":
			return s, PushScreen(NewStreamScreen(s.client, s.entry))
		case "t":
			s.pending = "truncate"
			return s, PushScreen(NewConfirmScreen(fmt.Sprintf(
				"Truncate hidden file held by PID %d fd %d (%s)?\n\n"+
					"The process will NOT be restarted or signaled. If it accesses "+
					"this file via mmap or random-offset seeks (databases, WAL, "+
					"indexes) this can trigger SIGBUS or corruption. Prefer SIGHUP "+
					"first for daemons that support graceful log reopen.",
				s.entry.PID, s.entry.FD, humanSize(s.entry.Size))))
		case "h":
			s.pending = "hup"
			return s, PushScreen(NewConfirmScreen(fmt.Sprintf(
				"Send SIGHUP to PID %d (%s)?\n\n"+
					"Well-behaved daemons close and reopen their log files on this "+
					"signal, releasing the unlinked inode themselves.",
				s.entry.PID, s.entry.Comm)))
		case "k":
			s.pending = "kill"
			return s, PushScreen(NewKillConfirmScreen(fmt.Sprintf(
				"SIGKILL PID %d (%s)?\n\n"+
					"This terminates the process IMMEDIATELY — it cannot be caught, "+
					"blocked, or handled, so there is no graceful shutdown and no "+
					"chance to flush buffers or finish in-flight work. Only the "+
					"kernel's normal process teardown runs (all fds close, this "+
					"hidden file's space is freed as a side effect). Use this when "+
					"the process is hung or unresponsive to SIGHUP and a supervisor "+
					"(systemd, container runtime, etc.) will restart it — not as a "+
					"routine way to free space.",
				s.entry.PID, s.entry.Comm)))
		}
	}
	return s, nil
}

func (s *DetailScreen) truncateCmd() tea.Cmd {
	e := s.entry
	return func() tea.Msg {
		freed, err := s.client.Truncate(e.PID, e.FD)
		if err != nil {
			return actionResultMsg{err: err}
		}
		return actionResultMsg{label: fmt.Sprintf("Freed %s (PID %d, fd %d)", humanSize(freed), e.PID, e.FD)}
	}
}

func (s *DetailScreen) hupCmd() tea.Cmd {
	e := s.entry
	return func() tea.Msg {
		if err := s.client.Hup(e.PID); err != nil {
			return actionResultMsg{err: err}
		}
		return actionResultMsg{label: fmt.Sprintf("SIGHUP sent to PID %d", e.PID)}
	}
}

func (s *DetailScreen) killCmd() tea.Cmd {
	e := s.entry
	return func() tea.Msg {
		if err := s.client.Kill(e.PID); err != nil {
			return actionResultMsg{err: err}
		}
		return actionResultMsg{label: fmt.Sprintf("SIGKILL sent to PID %d", e.PID)}
	}
}

func (s *DetailScreen) View() string {
	e := s.entry
	title := StyleTitle.Render(fmt.Sprintf("PID %d - %s", e.PID, e.Comm))
	body := fmt.Sprintf(
		"cmdline:  %s\n"+
			"fd:       %d      mode: %s      owner: %s\n"+
			"was at:   %s\n"+
			"size:     %s  (%d bytes)\n"+
			"inode:    %d   device: %d:%d\n"+
			"mtime:    %s\n",
		e.Cmdline, e.FD, e.Mode, e.User, e.Path,
		humanSize(e.Size), e.Size, e.Inode, e.DevMajor, e.DevMinor,
		time.Unix(e.Mtime, 0).Format("2006-01-02 15:04:05"),
	)
	hint := helpLine(
		"s", "Stream live writes",
		"t", "Truncate & reclaim",
		"h", "SIGHUP (graceful reopen)",
		"k", "SIGKILL process",
		"Esc", "Close",
	)
	content := title + "\n\n" + body
	if s.status != "" {
		content += "\n" + StyleError.Render(s.status) + "\n"
	}
	content += "\n" + hint

	w := s.width - 8
	if w > 90 {
		w = 90
	}
	box := StyleBox.Width(w).Render(content)
	return lipgloss.Place(s.width, s.height, lipgloss.Center, lipgloss.Center, box)
}
