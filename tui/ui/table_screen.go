package ui

import (
	"fmt"
	"strconv"
	"strings"
	"time"

	"github.com/charmbracelet/bubbles/table"
	"github.com/charmbracelet/bubbles/textinput"
	tea "github.com/charmbracelet/bubbletea"

	"hsed/tui/client"
)

type scanResultMsg struct {
	entries []client.HiddenEntry
	total   int64
}

type scanErrMsg struct{ err error }

type tickMsg struct{}

type TableScreen struct {
	client   *client.Client
	minSize  int64
	onlyPID  int
	interval time.Duration

	table       table.Model
	filterInput textinput.Model
	filtering   bool
	filterText  string

	entries         []client.HiddenEntry
	filteredEntries []client.HiddenEntry
	totalBytes      int64
	lastErr         string
	notice          string

	width, height int
}

func NewTableScreen(c *client.Client, minSize int64, onlyPID int, interval time.Duration) *TableScreen {
	cols := []table.Column{
		{Title: "PID", Width: 7},
		{Title: "Process", Width: 14},
		{Title: "FD", Width: 4},
		{Title: "Mode", Width: 4},
		{Title: "Size", Width: 8},
		{Title: "Owner", Width: 10},
		{Title: "Was at", Width: 40},
	}
	t := table.New(
		table.WithColumns(cols),
		table.WithFocused(true),
	)
	st := table.DefaultStyles()
	st.Header = st.Header.BorderForeground(colorAccentDim).Bold(true).Foreground(colorAccent)
	st.Selected = st.Selected.Foreground(lipglossBlack).Background(colorAccent).Bold(true)
	t.SetStyles(st)

	fi := textinput.New()
	fi.Placeholder = "Filter by process name / path (Enter to apply, Esc to clear)"

	return &TableScreen{
		client:      c,
		minSize:     minSize,
		onlyPID:     onlyPID,
		interval:    interval,
		table:       t,
		filterInput: fi,
	}
}

func (s *TableScreen) Resize(w, h int) {
	s.width, s.height = w, h
	if w <= 0 {
		return
	}
	fixed := 7 + 4 + 4 + 8 + 10
	remaining := w - fixed - 10 // borders/padding fudge
	if remaining < 20 {
		remaining = 20
	}
	processW := remaining * 3 / 10
	pathW := remaining - processW
	cols := []table.Column{
		{Title: "PID", Width: 7},
		{Title: "Process", Width: processW},
		{Title: "FD", Width: 4},
		{Title: "Mode", Width: 4},
		{Title: "Size", Width: 8},
		{Title: "Owner", Width: 10},
		{Title: "Was at", Width: pathW},
	}
	s.table.SetColumns(cols)
	s.table.SetWidth(w)
	tableH := h - 6
	if tableH < 3 {
		tableH = 3
	}
	s.table.SetHeight(tableH)
}

func (s *TableScreen) scanCmd() tea.Cmd {
	return func() tea.Msg {
		entries, total, err := s.client.Scan(s.minSize, s.onlyPID)
		if err != nil {
			return scanErrMsg{err: err}
		}
		return scanResultMsg{entries: entries, total: total}
	}
}

func tickCmd(d time.Duration) tea.Cmd {
	return tea.Tick(d, func(time.Time) tea.Msg { return tickMsg{} })
}

func (s *TableScreen) Init() tea.Cmd {
	return tea.Batch(s.scanCmd(), tickCmd(s.interval))
}

func (s *TableScreen) applyFilter() {
	s.filteredEntries = nil
	rows := make([]table.Row, 0, len(s.entries))
	for _, e := range s.entries {
		if s.filterText != "" &&
			!strings.Contains(strings.ToLower(e.Comm), s.filterText) &&
			!strings.Contains(strings.ToLower(e.Path), s.filterText) {
			continue
		}
		s.filteredEntries = append(s.filteredEntries, e)
		rows = append(rows, table.Row{
			strconv.Itoa(e.PID), e.Comm, strconv.Itoa(e.FD), e.Mode,
			humanSize(e.Size), e.User, e.Path,
		})
	}
	s.table.SetRows(rows)
}

func (s *TableScreen) Update(msg tea.Msg) (Screen, tea.Cmd) {
	switch m := msg.(type) {
	case scanResultMsg:
		s.entries = m.entries
		s.totalBytes = m.total
		s.lastErr = ""
		s.applyFilter()
		return s, nil

	case scanErrMsg:
		s.lastErr = m.err.Error()
		return s, nil

	case tickMsg:
		return s, tea.Batch(s.scanCmd(), tickCmd(s.interval))

	case resultMsg:
		if note, ok := m.value.(string); ok {
			s.notice = note
		}
		return s, s.scanCmd()

	case tea.KeyMsg:
		if s.filtering {
			switch m.String() {
			case "enter":
				s.filterText = strings.ToLower(strings.TrimSpace(s.filterInput.Value()))
				s.filtering = false
				s.filterInput.Blur()
				s.applyFilter()
				return s, nil
			case "esc":
				s.filterInput.SetValue("")
				s.filterText = ""
				s.filtering = false
				s.filterInput.Blur()
				s.applyFilter()
				return s, nil
			}
			var cmd tea.Cmd
			s.filterInput, cmd = s.filterInput.Update(msg)
			return s, cmd
		}

		switch m.String() {
		case "q":
			return s, tea.Quit
		case "r":
			s.notice = ""
			return s, s.scanCmd()
		case "/":
			s.filtering = true
			return s, s.filterInput.Focus()
		case "v":
			return s, PushScreen(NewTreeScreen(s.client, s.entries))
		case "enter":
			if len(s.filteredEntries) == 0 {
				return s, nil
			}
			idx := s.table.Cursor()
			if idx < 0 || idx >= len(s.filteredEntries) {
				return s, nil
			}
			return s, PushScreen(NewDetailScreen(s.client, s.filteredEntries[idx]))
		}
	}

	var cmd tea.Cmd
	s.table, cmd = s.table.Update(msg)
	return s, cmd
}

func (s *TableScreen) View() string {
	title := StyleHeader.Width(s.width).Render("Hidden Space Explorer")

	var summaryText string
	if s.lastErr != "" {
		summaryText = " [!] " + s.lastErr
	} else {
		summaryText = fmt.Sprintf(
			" Hidden files: %d (showing %d)   Reclaimable space invisible to du/df's directory walk: %s   %s",
			len(s.entries), len(s.filteredEntries), humanSize(s.totalBytes),
			helpLine("r", "rescan", "/", "filter", "Enter", "details", "v", "tree", "q", "quit"),
		)
	}
	if s.notice != "" {
		summaryText = " " + s.notice + "  |  " + strings.TrimLeft(summaryText, " ")
	}
	summary := StyleSummary.Width(s.width).Render(summaryText)

	var b strings.Builder
	b.WriteString(title)
	b.WriteString("\n")
	b.WriteString(summary)
	b.WriteString("\n")
	if s.filtering {
		b.WriteString(s.filterInput.View())
		b.WriteString("\n")
	}
	b.WriteString(s.table.View())
	return b.String()
}
