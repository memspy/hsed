package ui

import (
	"fmt"
	"os"
	"os/exec"
	"syscall"
	"testing"
	"time"

	tea "github.com/charmbracelet/bubbletea"

	"hsed/tui/client"
)

const testSocket = "/tmp/hsed-go-test.sock"


func driveScreen(s Screen, msg tea.Msg) Screen {
	newS, cmd := s.Update(msg)
	s = newS
	if cmd == nil {
		return s
	}
	result := cmd()
	if result == nil {
		return s
	}
	return driveScreen(s, result)
}

func TestTableScreenScanAndPushDetail(t *testing.T) {
	c := client.New(testSocket)

	tmp, err := os.CreateTemp("", "hsed_go_ui_*.log")
	if err != nil {
		t.Fatal(err)
	}
	defer tmp.Close()
	tmp.Write(make([]byte, 1234))
	tmp.Sync()
	os.Remove(tmp.Name())

	table := NewTableScreen(c, 0, os.Getpid(), client.UIDAny, time.Hour)
	table.Resize(120, 40)

	
	msg := table.scanCmd()()
	newScreen, _ := table.Update(msg)
	table = newScreen.(*TableScreen)

	if len(table.entries) == 0 {
		t.Fatalf("expected at least one entry after scan, got 0")
	}
	if table.filteredEntries[0].Size != 1234 {
		t.Fatalf("expected our 1234-byte file first (sorted by size desc), got %+v", table.filteredEntries[0])
	}

	var s Screen = table
	s, cmd := s.Update(tea.KeyMsg{Type: tea.KeyEnter})
	if cmd == nil {
		t.Fatalf("expected a push command after Enter")
	}
	pushed := cmd()
	pm, ok := pushed.(pushMsg)
	if !ok {
		t.Fatalf("expected pushMsg, got %T", pushed)
	}
	detail, ok := pm.screen.(*DetailScreen)
	if !ok {
		t.Fatalf("expected *DetailScreen, got %T", pm.screen)
	}
	if detail.entry.Size != 1234 {
		t.Fatalf("detail screen has wrong entry: %+v", detail.entry)
	}
}

func TestDetailScreenTruncateFlow(t *testing.T) {
	c := client.New(testSocket)

	tmp, err := os.CreateTemp("", "hsed_go_ui_trunc_*.log")
	if err != nil {
		t.Fatal(err)
	}
	defer tmp.Close()
	tmp.Write(make([]byte, 9999))
	tmp.Sync()
	os.Remove(tmp.Name())

	entries, _, err := c.Scan(0, os.Getpid(), client.UIDAny)
	if err != nil {
		t.Fatal(err)
	}
	var entry client.HiddenEntry
	for _, e := range entries {
		if e.Size == 9999 {
			entry = e
		}
	}
	if entry.PID == 0 {
		t.Fatalf("did not find our test entry")
	}

	detail := NewDetailScreen(c, entry)
	detail.Resize(120, 40)

	var s Screen = detail
	s, cmd := s.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune("t")})
	detail = s.(*DetailScreen)
	if detail.pending != "truncate" {
		t.Fatalf("expected pending=truncate, got %q", detail.pending)
	}
	pushed := cmd()
	pm := pushed.(pushMsg)
	if _, ok := pm.screen.(*ConfirmScreen); !ok {
		t.Fatalf("expected *ConfirmScreen, got %T", pm.screen)
	}

	
	final := driveScreen(detail, resultMsg{value: true})
	fd := final.(*DetailScreen)
	if fd.status != "" {
		t.Fatalf("unexpected error status after truncate: %s", fd.status)
	}

	st, err := tmp.Stat()
	if err != nil {
		t.Fatal(err)
	}
	if st.Size() != 0 {
		t.Fatalf("file size after truncate-via-UI = %d, want 0", st.Size())
	}
}

func TestKillConfirmScreenRequiresExactWord(t *testing.T) {
	var s Screen = NewKillConfirmScreen("SIGKILL PID 1234 (test)?")
	s.Resize(120, 40)
	s.Init()() 

	for _, ch := range "nope" {
		s, _ = s.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune{ch}})
	}
	s, cmd := s.Update(tea.KeyMsg{Type: tea.KeyEnter})
	popped := cmd().(popMsg)
	if confirmed, _ := popped.result.(bool); confirmed {
		t.Fatalf("wrong word should not confirm")
	}

	s = NewKillConfirmScreen("SIGKILL PID 1234 (test)?")
	s.Resize(120, 40)
	s.Init()()
	for _, ch := range killConfirmWord {
		s, _ = s.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune{ch}})
	}
	s, cmd = s.Update(tea.KeyMsg{Type: tea.KeyEnter})
	popped = cmd().(popMsg)
	if confirmed, _ := popped.result.(bool); !confirmed {
		t.Fatalf("exact word KILL should confirm")
	}
}

func TestDetailScreenKillFlowEndToEnd(t *testing.T) {
	c := client.New(testSocket)

	victim := exec.Command("python3", "-u", "-c", `
import tempfile, os, time, signal
signal.signal(signal.SIGHUP, lambda *a: None)
tmp = tempfile.NamedTemporaryFile(prefix="hsed_go_ui_kill_", suffix=".log", delete=False)
tmp.write(b"K"*555); tmp.flush()
os.unlink(tmp.name)
print("READY", flush=True)
time.sleep(15)
`)
	stdout, _ := victim.StdoutPipe()
	if err := victim.Start(); err != nil {
		t.Fatal(err)
	}
	buf := make([]byte, 16)
	stdout.Read(buf)
	defer victim.Process.Kill()

	entries, _, err := c.Scan(0, victim.Process.Pid, client.UIDAny)
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 1 {
		t.Fatalf("expected exactly one entry for victim, got %d", len(entries))
	}
	entry := entries[0]

	detail := NewDetailScreen(c, entry)
	detail.Resize(120, 40)

	var s Screen = detail
	s, cmd := s.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune("k")})
	detail = s.(*DetailScreen)
	if detail.pending != "kill" {
		t.Fatalf("expected pending=kill, got %q", detail.pending)
	}
	pushed := cmd().(pushMsg)
	if _, ok := pushed.screen.(*KillConfirmScreen); !ok {
		t.Fatalf("expected *KillConfirmScreen, got %T", pushed.screen)
	}

	// Simulate the confirm screen answering "true" (as if KILL was typed).
	final := driveScreen(detail, resultMsg{value: true})
	fd := final.(*DetailScreen)
	if fd.status != "" {
		t.Fatalf("unexpected error status after kill: %s", fd.status)
	}

	time.Sleep(300 * time.Millisecond)
	err = victim.Wait()
	exitErr, ok := err.(*exec.ExitError)
	if !ok {
		t.Fatalf("expected victim to have exited with an error (signaled), got: %v", err)
	}
	status := exitErr.Sys().(syscall.WaitStatus)
	if !status.Signaled() || status.Signal() != syscall.SIGKILL {
		t.Fatalf("expected SIGKILL, got status %v", status)
	}
}

func TestTreeScreenBuildsAndNavigates(t *testing.T) {
	c := client.New(testSocket)
	entries := []client.HiddenEntry{
		{PID: 1, FD: 3, Path: "/var/log/app.log", Size: 100, Comm: "app"},
		{PID: 1, FD: 4, Path: "/var/log/other.log", Size: 200, Comm: "app"},
	}
	tree := NewTreeScreen(c, entries)
	tree.Resize(120, 40)

	if len(tree.rows) != 1 {
		t.Fatalf("expected 1 top-level row (var/, collapsed), got %d: %+v", len(tree.rows), tree.rows)
	}
	if !tree.rows[0].isDir || tree.rows[0].label != "var" {
		t.Fatalf("expected top row to be the 'var' directory, got %+v", tree.rows[0])
	}

	var s Screen = tree
	s, _ = s.Update(tea.KeyMsg{Type: tea.KeyEnter})
	tree = s.(*TreeScreen)
	if len(tree.rows) != 2 {
		t.Fatalf("expected var/ and log/ after expanding, got %d rows", len(tree.rows))
	}

	tree.cursor = 1
	s, _ = tree.Update(tea.KeyMsg{Type: tea.KeyEnter})
	tree = s.(*TreeScreen)
	if len(tree.rows) != 4 {
		t.Fatalf("expected 4 rows (var, log, app.log, other.log), got %d: %+v", len(tree.rows), tree.rows)
	}
}

func TestStreamScreenLiveCaptureViaApp(t *testing.T) {
	c := client.New(testSocket)

	writer := exec.Command("python3", "-u", "-c", `
import tempfile, os, time
tmp = tempfile.NamedTemporaryFile(prefix="hsed_go_ui_stream_", suffix=".log", delete=False)
tmp.write(b"warmup"); tmp.flush()
os.unlink(tmp.name)
print(f"{os.getpid()} {tmp.fileno()}", flush=True)
i = 0
while True:
    tmp.write(f"uiline-{i}\n".encode())
    tmp.flush()
    i += 1
    time.sleep(0.2)
`)
	stdout, _ := writer.StdoutPipe()
	if err := writer.Start(); err != nil {
		t.Fatal(err)
	}
	defer writer.Process.Kill()

	var pid, fd int
	buf := make([]byte, 32)
	n, _ := stdout.Read(buf)
	if _, err := fmt.Sscanf(string(buf[:n]), "%d %d", &pid, &fd); err != nil {
		t.Fatalf("parsing writer announcement: %v", err)
	}

	entry := client.HiddenEntry{PID: pid, FD: fd, Comm: "python3", Path: "/tmp/warmup"}
	stream := NewStreamScreen(c, entry)
	stream.Resize(120, 40)

	attachedRaw := stream.Init()()
	newS, cmd := stream.Update(attachedRaw)
	stream = newS.(*StreamScreen)
	if stream.session == nil {
		t.Fatalf("expected session to be set after attach")
	}
	defer stream.CloseSession()

	sawContent := false
	for i := 0; i < 5 && !sawContent; i++ {
		if cmd == nil {
			t.Fatalf("expected a follow-up read command")
		}
		msg := cmd()
		var ns Screen
		ns, cmd = stream.Update(msg)
		stream = ns.(*StreamScreen)
		for _, line := range stream.lines {
			if len(line) > 0 && contains(line, "uiline-") {
				sawContent = true
			}
		}
	}
	if !sawContent {
		t.Fatalf("did not see live-captured content in stream lines: %v", stream.lines)
	}
}

func contains(s, substr string) bool {
	return len(s) >= len(substr) && (func() bool {
		for i := 0; i+len(substr) <= len(s); i++ {
			if s[i:i+len(substr)] == substr {
				return true
			}
		}
		return false
	})()
}
