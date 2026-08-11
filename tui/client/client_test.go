package client

import (
	"bufio"
	"fmt"
	"os"
	"os/exec"
	"syscall"
	"testing"
	"time"
)

const testSocket = "/tmp/hsed-go-test.sock"

func TestFullClientLifecycle(t *testing.T) {
	c := New(testSocket)

	if err := c.Ping(); err != nil {
		t.Fatalf("Ping: %v", err)
	}

	tmp, err := os.CreateTemp("", "hsed_go_client_*.log")
	if err != nil {
		t.Fatal(err)
	}
	defer tmp.Close()
	if _, err := tmp.Write(make([]byte, 4321)); err != nil {
		t.Fatal(err)
	}
	tmp.Sync()
	path := tmp.Name()
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}

	entries, _, err := c.Scan(0, os.Getpid(), UIDAny)
	if err != nil {
		t.Fatalf("Scan: %v", err)
	}
	var found *HiddenEntry
	for i := range entries {
		if entries[i].Size == 4321 {
			found = &entries[i]
		}
	}
	if found == nil {
		t.Fatalf("did not find the unlinked test file in scan results: %+v", entries)
	}
	if found.PID != os.Getpid() {
		t.Fatalf("wrong pid: got %d want %d", found.PID, os.Getpid())
	}

	freed, err := c.Truncate(found.PID, found.FD)
	if err != nil {
		t.Fatalf("Truncate: %v", err)
	}
	if freed != 4321 {
		t.Fatalf("freed = %d, want 4321", freed)
	}
	st, err := tmp.Stat()
	if err != nil {
		t.Fatal(err)
	}
	if st.Size() != 0 {
		t.Fatalf("file size after truncate = %d, want 0", st.Size())
	}

	// HUP a well-behaved child that installs a handler and must survive.
	victim := exec.Command("python3", "-u", "-c", `
import signal, time
signal.signal(signal.SIGHUP, lambda *a: None)
print("READY", flush=True)
time.sleep(10)
`)
	stdout, _ := victim.StdoutPipe()
	if err := victim.Start(); err != nil {
		t.Fatal(err)
	}
	buf := make([]byte, 16)
	stdout.Read(buf)

	if err := c.Hup(victim.Process.Pid); err != nil {
		t.Fatalf("Hup: %v", err)
	}
	time.Sleep(200 * time.Millisecond)
	if victim.ProcessState != nil {
		t.Fatalf("victim exited after HUP, expected it to survive")
	}

	if err := c.Kill(victim.Process.Pid); err != nil {
		t.Fatalf("Kill: %v", err)
	}
	err = victim.Wait()
	if exitErr, ok := err.(*exec.ExitError); ok {
		if status, ok := exitErr.Sys().(syscall.WaitStatus); ok {
			if !status.Signaled() || status.Signal() != syscall.SIGKILL {
				t.Fatalf("expected SIGKILL, got status %v", status)
			}
		} else {
			t.Fatalf("could not read wait status")
		}
	} else {
		t.Fatalf("expected victim to die from KILL, Wait() returned: %v", err)
	}

	if err := c.Ping(); err != nil {
		t.Fatalf("daemon unhealthy after full command sequence: %v", err)
	}
}

func TestStatsAndUIDFilter(t *testing.T) {
	c := New(testSocket)

	tmp, err := os.CreateTemp("", "hsed_go_stats_*.log")
	if err != nil {
		t.Fatal(err)
	}
	defer tmp.Close()
	if _, err := tmp.Write(make([]byte, 8080)); err != nil {
		t.Fatal(err)
	}
	tmp.Sync()
	if err := os.Remove(tmp.Name()); err != nil {
		t.Fatal(err)
	}

	count, total, err := c.Stats(0, 0) // uid 0 (root) — this test process's own uid
	if err != nil {
		t.Fatalf("Stats: %v", err)
	}
	if count < 1 || total < 8080 {
		t.Fatalf("Stats undercounted: count=%d total=%d, expected at least our own 8080-byte entry", count, total)
	}

	excluded, _, err := c.Scan(0, os.Getpid(), 65534)
	if err != nil {
		t.Fatalf("Scan with uid filter: %v", err)
	}
	for _, e := range excluded {
		if e.Size == 8080 {
			t.Fatalf("uid filter 65534 should have excluded our (uid 0) entry, but found it: %+v", e)
		}
	}

	included, _, err := c.Scan(0, os.Getpid(), 0)
	if err != nil {
		t.Fatalf("Scan with matching uid filter: %v", err)
	}
	found := false
	for _, e := range included {
		if e.Size == 8080 {
			found = true
		}
	}
	if !found {
		t.Fatalf("uid filter 0 should have included our entry, got: %+v", included)
	}
}

func TestBuildPathTree(t *testing.T) {
	entries := []HiddenEntry{
		{PID: 1, FD: 3, Path: "/var/log/app.log", Size: 100},
		{PID: 2, FD: 4, Path: "/var/log/app.log", Size: 200},
		{PID: 1, FD: 5, Path: "/tmp/other.log", Size: 50},
	}
	root := BuildPathTree(entries)

	varNode, ok := root.Children["var"]
	if !ok || varNode.Children == nil {
		t.Fatalf("expected /var directory node")
	}
	logNode, ok := varNode.Children["log"]
	if !ok || logNode.Children == nil {
		t.Fatalf("expected /var/log directory node")
	}
	appLog, ok := logNode.Children["app.log"]
	if !ok {
		t.Fatalf("expected /var/log/app.log leaf")
	}
	if len(appLog.Entries) != 2 {
		t.Fatalf("expected 2 entries sharing app.log path, got %d", len(appLog.Entries))
	}

	tmpNode, ok := root.Children["tmp"]
	if !ok {
		t.Fatalf("expected /tmp directory node")
	}
	otherLog, ok := tmpNode.Children["other.log"]
	if !ok || len(otherLog.Entries) != 1 {
		t.Fatalf("expected /tmp/other.log leaf with 1 entry")
	}
}

func TestStreamSessionLiveCapture(t *testing.T) {
	c := New(testSocket)

	writer := exec.Command("python3", "-u", "-c", `
import tempfile, os, time
tmp = tempfile.NamedTemporaryFile(prefix="hsed_go_stream_", suffix=".log", delete=False)
tmp.write(b"warmup"); tmp.flush()
os.unlink(tmp.name)
print(f"{os.getpid()} {tmp.fileno()}", flush=True)
i = 0
while True:
    tmp.write(f"line-{i}\n".encode())
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
	{
		reader := bufio.NewReader(stdout)
		line, err := reader.ReadString('\n')
		if err != nil {
			t.Fatalf("reading writer's announcement: %v", err)
		}
		if _, err := fmt.Sscanf(line, "%d %d", &pid, &fd); err != nil {
			t.Fatalf("parsing writer's announcement %q: %v", line, err)
		}
	}

	sess, err := c.OpenStream(pid, fd)
	if err != nil {
		t.Fatalf("OpenStream: %v", err)
	}
	defer sess.Close()

	seen := 0
	deadline := time.After(5 * time.Second)
	events := make(chan *WriteEvent, 1)
	errs := make(chan error, 1)
	go func() {
		for {
			ev, err := sess.Next()
			if err != nil {
				errs <- err
				return
			}
			if ev == nil {
				return
			}
			events <- ev
		}
	}()
	for seen < 2 {
		select {
		case ev := <-events:
			seen++
			t.Logf("captured: %q", string(ev.Data()))
		case err := <-errs:
			t.Fatalf("stream error: %v", err)
		case <-deadline:
			t.Fatalf("timed out waiting for write events, saw %d", seen)
		}
	}
	sess.Close()
}
