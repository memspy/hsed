package ui

import (
	"fmt"
	"sort"
	"strings"

	tea "github.com/charmbracelet/bubbletea"

	"hsed/tui/client"
)

type treeRow struct {
	depth int
	label string
	node  *client.TreeNode
	isDir bool
}

type TreeScreen struct {
	client   *client.Client
	root     *client.TreeNode
	expanded map[*client.TreeNode]bool
	rows     []treeRow
	cursor   int

	width, height int
}

func NewTreeScreen(c *client.Client, entries []client.HiddenEntry) *TreeScreen {
	root := client.BuildPathTree(entries)
	s := &TreeScreen{
		client:   c,
		root:     root,
		expanded: map[*client.TreeNode]bool{root: true},
	}
	s.rebuild()
	return s
}

func (s *TreeScreen) rebuild() {
	s.rows = nil
	var walk func(n *client.TreeNode, depth int)
	walk = func(n *client.TreeNode, depth int) {
		names := make([]string, 0, len(n.Children))
		for name := range n.Children {
			names = append(names, name)
		}
		sort.Strings(names)
		for _, name := range names {
			child := n.Children[name]
			isDir := child.Children != nil
			s.rows = append(s.rows, treeRow{depth: depth, label: name, node: child, isDir: isDir})
			if isDir && s.expanded[child] {
				walk(child, depth+1)
			}
		}
	}
	walk(s.root, 0)
	if s.cursor >= len(s.rows) {
		s.cursor = len(s.rows) - 1
	}
	if s.cursor < 0 {
		s.cursor = 0
	}
}

func (s *TreeScreen) Init() tea.Cmd   { return nil }
func (s *TreeScreen) Resize(w, h int) { s.width, s.height = w, h }

func (s *TreeScreen) Update(msg tea.Msg) (Screen, tea.Cmd) {
	km, ok := msg.(tea.KeyMsg)
	if !ok {
		return s, nil
	}
	switch km.String() {
	case "esc", "q":
		return s, PopScreen()
	case "up", "k":
		if s.cursor > 0 {
			s.cursor--
		}
	case "down", "j":
		if s.cursor < len(s.rows)-1 {
			s.cursor++
		}
	case "enter", " ":
		if len(s.rows) == 0 {
			return s, nil
		}
		row := s.rows[s.cursor]
		if row.isDir {
			s.expanded[row.node] = !s.expanded[row.node]
			s.rebuild()
		} else if len(row.node.Entries) > 0 {
			best := row.node.Entries[0]
			for _, e := range row.node.Entries[1:] {
				if e.Size > best.Size {
					best = e
				}
			}
			return s, PushScreen(NewDetailScreen(s.client, best))
		}
	}
	return s, nil
}

func (s *TreeScreen) View() string {
	var b strings.Builder
	b.WriteString(StyleHeader.Width(s.width).Render("Hidden filesystem"))
	b.WriteString("\n")
	b.WriteString(StyleSummary.Width(s.width).Render(
		" Reconstructed from paths that no longer exist on disk, but are " +
			"still backing live process fds.   " +
			helpLine("Enter/Space", "expand / open", "Esc", "back")))
	b.WriteString("\n")

	if len(s.rows) == 0 {
		b.WriteString(StyleMuted.Render("  (nothing to show)"))
		return b.String()
	}

	for i, row := range s.rows {
		prefix := strings.Repeat("  ", row.depth)
		var line string
		if row.isDir {
			arrow := "▸"
			if s.expanded[row.node] {
				arrow = "▾"
			}
			line = fmt.Sprintf("%s%s %s/", prefix, arrow, row.label)
		} else {
			var total int64
			for _, e := range row.node.Entries {
				total += e.Size
			}
			note := ""
			if len(row.node.Entries) > 1 {
				note = fmt.Sprintf(", %d fds", len(row.node.Entries))
			}
			line = fmt.Sprintf("%s  %s  [%s%s]", prefix, row.label, humanSize(total), note)
		}
		if i == s.cursor {
			line = StyleSelected.Render(line)
		}
		b.WriteString(line)
		b.WriteString("\n")
	}
	return b.String()
}
