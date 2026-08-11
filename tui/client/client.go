package client

import (
	"bufio"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net"
	"os"
	"strings"
	"sync"
	"time"
)

type HsedError struct{ msg string }

func (e *HsedError) Error() string { return e.msg }

func errf(format string, a ...any) error { return &HsedError{fmt.Sprintf(format, a...)} }


func DefaultSocketPath() string {
	if v := os.Getenv("HSED_SOCKET"); v != "" {
		return v
	}
	if os.Geteuid() == 0 {
		return "/run/hsed.sock"
	}
	return fmt.Sprintf("/tmp/hsed-%d.sock", os.Geteuid())
}

type HiddenEntry struct {
	PID      int    `json:"pid"`
	FD       int    `json:"fd"`
	Mode     string `json:"mode"`
	Size     int64  `json:"size"`
	UID      int    `json:"uid"`
	User     string `json:"user"`
	Comm     string `json:"comm"`
	Cmdline  string `json:"cmdline"`
	Path     string `json:"path"`
	Inode    uint64 `json:"inode"`
	DevMajor int    `json:"dev_major"`
	DevMinor int    `json:"dev_minor"`
	Mtime    int64  `json:"mtime"`
}


func (e HiddenEntry) Key() string { return fmt.Sprintf("%d:%d", e.PID, e.FD) }

type WriteEvent struct {
	TID      int    `json:"tid"`
	Ret      int64  `json:"ret"`
	Captured int    `json:"captured"`
	IsWritev bool   `json:"is_writev"`
	DataB64  string `json:"data_b64"`
}


func (w WriteEvent) Data() []byte {
	b, _ := base64.StdEncoding.DecodeString(w.DataB64)
	return b
}


type envelope struct {
	Type       string `json:"type"`
	Message    string `json:"message"`
	OK         bool   `json:"ok"`
	Freed      int64  `json:"freed"`
	TotalBytes int64  `json:"total_bytes"`
	Count      int    `json:"count"`
}


type Client struct {
	SocketPath string
	Timeout    time.Duration
}

func New(socketPath string) *Client {
	if socketPath == "" {
		socketPath = DefaultSocketPath()
	}
	return &Client{SocketPath: socketPath, Timeout: 5 * time.Second}
}

func (c *Client) connect() (net.Conn, error) {
	conn, err := net.DialTimeout("unix", c.SocketPath, c.Timeout)
	if err != nil {
		return nil, errf(
			"can't reach hsedd at %s (%v). Start the daemon first, e.g.: sudo hsedd  (or `systemctl start hsed`)",
			c.SocketPath, err,
		)
	}
	return conn, nil
}

func readEnvelope(r *bufio.Reader) (string, envelope, error) {
	line, err := r.ReadString('\n')
	if err != nil {
		if line == "" {
			return "", envelope{}, errf("connection closed unexpectedly")
		}
	}
	line = strings.TrimRight(line, "\r\n")
	var env envelope
	if jsonErr := json.Unmarshal([]byte(line), &env); jsonErr != nil {
		return "", envelope{}, errf("malformed response from daemon: %v", jsonErr)
	}
	return line, env, nil
}

func (c *Client) Ping() error {
	conn, err := c.connect()
	if err != nil {
		return err
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(c.Timeout))
	if _, err := fmt.Fprint(conn, "PING\n"); err != nil {
		return errf("write failed: %v", err)
	}
	_, env, err := readEnvelope(bufio.NewReader(conn))
	if err != nil {
		return err
	}
	if env.Type != "pong" {
		return errf("unexpected response to PING: %q", env.Type)
	}
	return nil
}


const UIDAny int64 = -1


func (c *Client) Scan(minSize int64, onlyPID int, uidFilter int64) ([]HiddenEntry, int64, error) {
	conn, err := c.connect()
	if err != nil {
		return nil, 0, err
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(c.Timeout))
	cmd := fmt.Sprintf("SCAN %d %d", minSize, onlyPID)
	if uidFilter != UIDAny {
		cmd += fmt.Sprintf(" %d", uidFilter)
	}
	if _, err := fmt.Fprintf(conn, "%s\n", cmd); err != nil {
		return nil, 0, errf("write failed: %v", err)
	}

	reader := bufio.NewReader(conn)
	var entries []HiddenEntry
	for {
		line, env, err := readEnvelope(reader)
		if err != nil {
			return nil, 0, errf("connection closed unexpectedly during SCAN")
		}
		switch env.Type {
		case "entry":
			var e HiddenEntry
			if err := json.Unmarshal([]byte(line), &e); err != nil {
				return nil, 0, errf("malformed entry from daemon: %v", err)
			}
			entries = append(entries, e)
		case "end":
			return entries, env.TotalBytes, nil
		case "error":
			return nil, 0, errf("%s", env.Message)
		default:
			return nil, 0, errf("unexpected response type during SCAN: %q", env.Type)
		}
	}
}


func (c *Client) Stats(minSize int64, uidFilter int64) (count int, totalBytes int64, err error) {
	conn, err := c.connect()
	if err != nil {
		return 0, 0, err
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(c.Timeout))
	cmdStr := fmt.Sprintf("STATS %d", minSize)
	if uidFilter != UIDAny {
		cmdStr += fmt.Sprintf(" %d", uidFilter)
	}
	if _, err := fmt.Fprintf(conn, "%s\n", cmdStr); err != nil {
		return 0, 0, errf("write failed: %v", err)
	}
	_, env, err := readEnvelope(bufio.NewReader(conn))
	if err != nil {
		return 0, 0, err
	}
	if env.Type == "error" {
		return 0, 0, errf("%s", env.Message)
	}
	if env.Type != "stats" {
		return 0, 0, errf("unexpected response type for STATS: %q", env.Type)
	}
	return env.Count, env.TotalBytes, nil
}

func (c *Client) simpleCommand(format string, a ...any) (envelope, error) {
	conn, err := c.connect()
	if err != nil {
		return envelope{}, err
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(c.Timeout))
	if _, err := fmt.Fprintf(conn, format+"\n", a...); err != nil {
		return envelope{}, errf("write failed: %v", err)
	}
	_, env, err := readEnvelope(bufio.NewReader(conn))
	if err != nil {
		return envelope{}, err
	}
	if env.Type == "error" {
		return envelope{}, errf("%s", env.Message)
	}
	return env, nil
}

func (c *Client) Truncate(pid, fd int) (int64, error) {
	env, err := c.simpleCommand("TRUNCATE %d %d", pid, fd)
	if err != nil {
		return 0, err
	}
	return env.Freed, nil
}


func (c *Client) Hup(pid int) error {
	_, err := c.simpleCommand("HUP %d", pid)
	return err
}


func (c *Client) Kill(pid int) error {
	_, err := c.simpleCommand("KILL %d", pid)
	return err
}


type StreamSession struct {
	conn   net.Conn
	reader *bufio.Reader

	mu     sync.Mutex
	closed bool
}


func (c *Client) OpenStream(pid, fd int) (*StreamSession, error) {
	conn, err := c.connect()
	if err != nil {
		return nil, err
	}
	
	if _, err := fmt.Fprintf(conn, "STREAM %d %d\n", pid, fd); err != nil {
		conn.Close()
		return nil, errf("could not start STREAM: %v", err)
	}
	return &StreamSession{conn: conn, reader: bufio.NewReader(conn)}, nil
}


func (s *StreamSession) Next() (*WriteEvent, error) {
	for {
		line, env, err := readEnvelope(s.reader)
		if err != nil {
			return nil, nil
		}
		switch env.Type {
		case "attached":
			continue
		case "write":
			var w WriteEvent
			if jsonErr := json.Unmarshal([]byte(line), &w); jsonErr != nil {
				return nil, errf("malformed write event from daemon: %v", jsonErr)
			}
			return &w, nil
		case "stream_end":
			return nil, nil
		case "error":
			return nil, errf("%s", env.Message)
		default:
			continue
		}
	}
}

func (s *StreamSession) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return nil
	}
	s.closed = true
	return s.conn.Close()
}


type TreeNode struct {
	Name     string
	Children map[string]*TreeNode
	Entries  []HiddenEntry
}

func BuildPathTree(entries []HiddenEntry) *TreeNode {
	root := &TreeNode{Children: map[string]*TreeNode{}}
	for _, e := range entries {
		parts := strings.Split(strings.Trim(e.Path, "/"), "/")
		node := root
		for i, part := range parts {
			if part == "" {
				continue
			}
			if i == len(parts)-1 {
				child, ok := node.Children[part]
				if !ok {
					child = &TreeNode{Name: part}
					node.Children[part] = child
				}
				child.Entries = append(child.Entries, e)
			} else {
				child, ok := node.Children[part]
				if !ok {
					child = &TreeNode{Name: part, Children: map[string]*TreeNode{}}
					node.Children[part] = child
				} else if child.Children == nil {
					child.Children = map[string]*TreeNode{}
				}
				node = child
			}
		}
	}
	return root
}
