package ui

import (
	tea "github.com/charmbracelet/bubbletea"

	"hsed/tui/client"
)


type Screen interface {
	Init() tea.Cmd
	Update(msg tea.Msg) (Screen, tea.Cmd)
	View() string
	Resize(width, height int)
}

type pushMsg struct{ screen Screen }

type popMsg struct{ result any }


type resultMsg struct{ value any }

func PushScreen(s Screen) tea.Cmd {
	return func() tea.Msg { return pushMsg{screen: s} }
}

func PopScreen() tea.Cmd {
	return func() tea.Msg { return popMsg{} }
}

func PopScreenWithResult(v any) tea.Cmd {
	return func() tea.Msg { return popMsg{result: v} }
}


type App struct {
	Client *client.Client
	stack  []Screen
	width  int
	height int
}

func NewApp(c *client.Client, root Screen) *App {
	return &App{Client: c, stack: []Screen{root}}
}

func (a *App) Init() tea.Cmd {
	return a.top().Init()
}

func (a *App) top() Screen { return a.stack[len(a.stack)-1] }

func (a *App) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch m := msg.(type) {
	case tea.WindowSizeMsg:
		a.width, a.height = m.Width, m.Height
		for _, s := range a.stack {
			s.Resize(a.width, a.height)
		}
		return a, nil

	case tea.KeyMsg:
		if m.String() == "ctrl+c" {
			return a, tea.Quit
		}

	case pushMsg:
		m.screen.Resize(a.width, a.height)
		a.stack = append(a.stack, m.screen)
		return a, m.screen.Init()

	case popMsg:
		if len(a.stack) > 1 {
			a.stack = a.stack[:len(a.stack)-1]
		}
		if m.result != nil {
			newTop, cmd := a.top().Update(resultMsg{value: m.result})
			a.stack[len(a.stack)-1] = newTop
			return a, cmd
		}
		return a, nil
	}

	newTop, cmd := a.top().Update(msg)
	a.stack[len(a.stack)-1] = newTop
	return a, cmd
}

func (a *App) View() string {
	return a.top().View()
}


type Closer interface{ CloseSession() }

func (a *App) CloseAllSessions() {
	for _, s := range a.stack {
		if c, ok := s.(Closer); ok {
			c.CloseSession()
		}
	}
}
