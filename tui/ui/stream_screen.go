package ui

import (
	"fmt"
	"strings"
	"time"

	tea "github.com/charmbracelet/bubbletea"

	"hsed/tui/client"
)

type streamAttachedMsg struct{ session *client.StreamSession }
type streamAttachErrMsg struct{ err error }
type streamEventMsg struct{ ev *client.WriteEvent } // ev == nil means clean end
type streamErrMsg struct{ err error }


type StreamScreen struct {
	client    *client.Client
	entry     client.HiddenEntry
	session   *client.StreamSession
	lines     []string
	attachErr string

	width, height int
}

func NewStreamScreen(c *client.Client, e client.HiddenEntry) *StreamScreen {
	return &StreamScreen{client: c, entry: e}
}

func (s *StreamScreen) Init() tea.Cmd {
	e := s.entry
	return func() tea.Msg {
		sess, err := s.client.OpenStream(e.PID, e.FD)
		if err != nil {
			return streamAttachErrMsg{err: err}
		}
		return streamAttachedMsg{session: sess}
	}
}

func (s *StreamScreen) Resize(w, h int) { s.width, s.height = w, h }

// CloseSession implements the App's safety-net Closer interface.
func (s *StreamScreen) CloseSession() {
	if s.session != nil {
		s.session.Close()
	}
}

func (s *StreamScreen) readNextCmd() tea.Cmd {
	sess := s.session
	return func() tea.Msg {
		ev, err := sess.Next()
		if err != nil {
			return streamErrMsg{err: err}
		}
		return streamEventMsg{ev: ev}
	}
}

func (s *StreamScreen) Update(msg tea.Msg) (Screen, tea.Cmd) {
	switch m := msg.(type) {
	case streamAttachedMsg:
		s.session = m.session
		s.lines = append(s.lines,
			fmt.Sprintf("[+] Attached to PID %d via ptrace (hsedd) ...", s.entry.PID),
			fmt.Sprintf("[+] Watching write()/pwrite64()/writev() on fd %d only", s.entry.FD),
			"",
		)
		return s, s.readNextCmd()

	case streamAttachErrMsg:
		s.attachErr = m.err.Error()
		return s, nil

	case streamEventMsg:
		if m.ev == nil {
			s.lines = append(s.lines, "[+] stream ended")
			return s, nil
		}
		ts := time.Now().Format("15:04:05")
		kind := "write"
		if m.ev.IsWritev {
			kind = "writev"
		}
		note := ""
		if int64(m.ev.Captured) < m.ev.Ret {
			note = fmt.Sprintf(" (showing first %dB)", m.ev.Captured)
		}
		s.lines = append(s.lines, fmt.Sprintf(
			"[%s] %s %dB%s: %q", ts, kind, m.ev.Ret, note, string(m.ev.Data()),
		))
		return s, s.readNextCmd()

	case streamErrMsg:
		s.lines = append(s.lines, "[!] "+m.err.Error())
		return s, nil

	case tea.KeyMsg:
		if m.String() == "esc" {
			s.CloseSession()
			return s, PopScreen()
		}
	}
	return s, nil
}

func (s *StreamScreen) View() string {
	header := StyleHeader.Width(s.width).Render(fmt.Sprintf(
		"Live write stream -> PID %d (%s)  fd=%d   was: %s",
		s.entry.PID, s.entry.Comm, s.entry.FD, s.entry.Path,
	))

	var body string
	if s.attachErr != "" {
		body = StyleError.Render("[!] " + s.attachErr)
	} else {
		maxLines := s.height - 3
		if maxLines < 1 {
			maxLines = 1
		}
		lines := s.lines
		if len(lines) > maxLines {
			lines = lines[len(lines)-maxLines:]
		}
		body = strings.Join(lines, "\n")
	}

	footer := StyleHelp.Render(helpLine("Esc", "Back"))
	return header + "\n" + body + "\n" + footer
}
