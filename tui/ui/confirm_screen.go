package ui

import (
	"github.com/charmbracelet/lipgloss"

	tea "github.com/charmbracelet/bubbletea"
)

type ConfirmScreen struct {
	question      string
	width, height int
}

func NewConfirmScreen(question string) *ConfirmScreen {
	return &ConfirmScreen{question: question}
}

func (s *ConfirmScreen) Init() tea.Cmd   { return nil }
func (s *ConfirmScreen) Resize(w, h int) { s.width, s.height = w, h }

func (s *ConfirmScreen) Update(msg tea.Msg) (Screen, tea.Cmd) {
	if km, ok := msg.(tea.KeyMsg); ok {
		switch km.String() {
		case "y":
			return s, PopScreenWithResult(true)
		case "n", "esc":
			return s, PopScreenWithResult(false)
		}
	}
	return s, nil
}

func (s *ConfirmScreen) View() string {
	w := s.width - 8
	if w > 76 {
		w = 76
	}
	box := StyleBox.Width(w).Render(s.question + "\n\n" + StyleHelp.Render("[y] Yes    [n / Esc] Cancel"))
	return lipgloss.Place(s.width, s.height, lipgloss.Center, lipgloss.Center, box)
}
