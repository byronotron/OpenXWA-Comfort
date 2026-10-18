package main

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"strings"
	"sync"
	"time"
)

// UI reads the user's answers from the console. When stdin ends (for example
// a scripted test), every question gets its default answer.
type UI struct {
	r   *bufio.Reader
	eof bool
}

func newUI(in io.Reader) *UI { return &UI{r: bufio.NewReader(in)} }

// readLine returns one line without the line ending; ok is false at end of input.
func (u *UI) readLine() (string, bool) {
	if u.eof {
		return "", false
	}
	s, err := u.r.ReadString('\n')
	if err != nil {
		u.eof = true
		if s == "" {
			logf("INPUT: <end of input>")
			return "", false
		}
	}
	s = strings.TrimRight(s, "\r\n")
	logf("INPUT: %q", s)
	return s, true
}

// ask prints a question and returns the trimmed answer.
func (u *UI) ask(question string) (string, bool) {
	fmt.Print(question + " ")
	logf("ASK: %s", question)
	line, ok := u.readLine()
	if !ok {
		fmt.Println()
	}
	return strings.TrimSpace(line), ok
}

// yesNo asks until it gets y or n; an empty answer (or end of input) is def.
func (u *UI) yesNo(question string, def bool) bool {
	for {
		ans, ok := u.ask(question)
		if !ok || ans == "" {
			return def
		}
		switch strings.ToLower(ans) {
		case "y", "yes", "j", "ja":
			return true
		case "n", "no", "nein", "non":
			return false
		}
		say("Please type y (yes) or n (no).")
	}
}

// say prints a line for the user and also writes it to the log.
func say(format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	fmt.Println(msg)
	if strings.TrimSpace(msg) != "" {
		logf("SAY: %s", msg)
	}
}

// progressLine redraws a single console line in place with \r, at most every
// 250 ms. The text is kept below 80 columns so it never wraps.
type progressLine struct {
	mu      sync.Mutex
	last    time.Time
	lastLen int
}

const progressWidth = 78

func (p *progressLine) show(text string, force bool) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if !force && time.Since(p.last) < 250*time.Millisecond {
		return
	}
	p.last = time.Now()
	if len(text) > progressWidth {
		text = text[:progressWidth]
	}
	pad := ""
	if p.lastLen > len(text) {
		pad = strings.Repeat(" ", p.lastLen-len(text))
	}
	fmt.Fprint(os.Stdout, "\r"+text+pad)
	p.lastLen = len(text)
}

// done ends the progress line so the next output starts on a new line.
func (p *progressLine) done(final string) {
	p.show(final, true)
	fmt.Println()
	logf("PROGRESS: %s", final)
	p.mu.Lock()
	p.lastLen = 0
	p.mu.Unlock()
}

// humanSize formats a byte count the way Windows Explorer users expect.
func humanSize(b int64) string {
	const (
		kb = 1024
		mb = 1024 * kb
		gb = 1024 * mb
	)
	switch {
	case b < kb:
		return fmt.Sprintf("%d bytes", b)
	case b < mb:
		return fmt.Sprintf("%d KB", b/kb)
	case b < 10*mb:
		return fmt.Sprintf("%.1f MB", float64(b)/mb)
	case b < gb:
		return fmt.Sprintf("%d MB", b/mb)
	default:
		return fmt.Sprintf("%.2f GB", float64(b)/gb)
	}
}
