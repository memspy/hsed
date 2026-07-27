package ui

import (
	"github.com/charmbracelet/bubbles/textinput"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

const killConfirmWord = "KILL"

type KillConfirmScreen struct {
	question      string
	input         textinput.Model
	width, height int
}

func NewKillConfirmScreen(question string) *KillConfirmScreen {
	ti := textinput.New()
	ti.Placeholder = killConfirmWord
	ti.CharLimit = 32
	return &KillConfirmScreen{question: question, input: ti}
}

func (s *KillConfirmScreen) Init() tea.Cmd   { return s.input.Focus() }
func (s *KillConfirmScreen) Resize(w, h int) { s.width, s.height = w, h }

func (s *KillConfirmScreen) Update(msg tea.Msg) (Screen, tea.Cmd) {
	if km, ok := msg.(tea.KeyMsg); ok {
		switch km.String() {
		case "esc":
			return s, PopScreenWithResult(false)
		case "enter":
			return s, PopScreenWithResult(s.input.Value() == killConfirmWord)
		}
	}
	var cmd tea.Cmd
	s.input, cmd = s.input.Update(msg)
	return s, cmd
}

func (s *KillConfirmScreen) View() string {
	w := s.width - 8
	if w > 76 {
		w = 76
	}
	content := StyleDanger.Render(s.question) + "\n\n" +
		StyleHelp.Render("Type "+killConfirmWord+" and press Enter to confirm, or Esc to cancel.") +
		"\n\n" + s.input.View()
	box := StyleDangerBox.Width(w).Render(content)
	return lipgloss.Place(s.width, s.height, lipgloss.Center, lipgloss.Center, box)
}
