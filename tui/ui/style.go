package ui

import "github.com/charmbracelet/lipgloss"

var (
	colorAccent    = lipgloss.Color("14")  // bright cyan
	colorAccentDim = lipgloss.Color("6")   // cyan
	colorText      = lipgloss.Color("252") // near-white
	colorMuted     = lipgloss.Color("242") // gray
	colorDanger    = lipgloss.Color("203") // red
	colorWarn      = lipgloss.Color("221") // yellow
	colorPanelBG   = lipgloss.Color("235") // dark gray panel background
	lipglossBlack  = lipgloss.Color("0")
)

var (
	StyleTitle = lipgloss.NewStyle().Bold(true).Foreground(colorAccent)

	StyleHeader = lipgloss.NewStyle().
			Bold(true).
			Foreground(lipgloss.Color("0")).
			Background(colorAccent).
			Padding(0, 1)

	StyleSummary = lipgloss.NewStyle().
			Foreground(colorText).
			Background(colorPanelBG).
			Padding(0, 1)

	StyleMuted  = lipgloss.NewStyle().Foreground(colorMuted)
	StyleAccent = lipgloss.NewStyle().Foreground(colorAccent)
	StyleDanger = lipgloss.NewStyle().Foreground(colorDanger).Bold(true)
	StyleWarn   = lipgloss.NewStyle().Foreground(colorWarn)
	StyleError  = lipgloss.NewStyle().Foreground(colorDanger).Bold(true)

	StyleBox = lipgloss.NewStyle().
			Border(lipgloss.RoundedBorder()).
			BorderForeground(colorAccentDim).
			Padding(1, 2)

	StyleDangerBox = lipgloss.NewStyle().
			Border(lipgloss.RoundedBorder()).
			BorderForeground(colorDanger).
			Padding(1, 2)

	StyleHelp = lipgloss.NewStyle().Foreground(colorMuted)

	StyleKey = lipgloss.NewStyle().Foreground(colorAccent).Bold(true)

	StyleSelected = lipgloss.NewStyle().Foreground(lipglossBlack).Background(colorAccent).Bold(true)
)

func helpLine(pairs ...string) string {
	s := ""
	for i := 0; i+1 < len(pairs); i += 2 {
		if i > 0 {
			s += "   "
		}
		s += StyleKey.Render("["+pairs[i]+"]") + " " + StyleHelp.Render(pairs[i+1])
	}
	return s
}
