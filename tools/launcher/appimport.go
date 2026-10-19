package main

import (
	"archive/zip"
	"compress/flate"
	"errors"
	"fmt"
	"io"
	"os"
	"path"
	"regexp"
	"strconv"
	"strings"
	"time"
)

// The OpenXWA app has a small helper (SetupProvider in the APK) that adb
// can call without starting the game, so the headset can stay on the desk:
//
//	content call --method mkdirs --arg 'XWA'   makes folders as the app
//	exec-in content write .../import < zip     the app writes a zip we stream
//	content call --method unzip --arg 'XWA'    the app unpacks it
//
// Everything is written by the app, so it belongs to the app. Files and
// folders made over USB belong to the "shell" user, and on Quest (Horizon
// OS 14) the app cannot use them.

const (
	helperURI       = "content://org.openxwa.quest.setup"
	importZipName   = ".openxwa-import.zip"
	importBatchSize = 256 << 20 // bytes of game files per zip
	importBatchMax  = 2500      // files per zip
)

type helperResult map[string]string

var bundleKeyRe = regexp.MustCompile(`(?:\{|, )([A-Za-z_]+)=`)

// parseBundle reads the "Result: Bundle[{key=value, ...}]" line printed by
// "content call". ok is false when there is no Bundle (no helper, refused).
func parseBundle(out string) (r helperResult, ok bool) {
	i := strings.Index(out, "Result: Bundle[{")
	if i < 0 {
		return nil, false
	}
	body := out[i+len("Result: Bundle["):]
	j := strings.LastIndex(body, "}]")
	if j < 0 {
		return nil, false
	}
	body = body[:j] // "{key=value, ..."
	locs := bundleKeyRe.FindAllStringSubmatchIndex(body, -1)
	r = helperResult{}
	for k, l := range locs {
		end := len(body)
		if k+1 < len(locs) {
			end = locs[k+1][0]
		}
		r[body[l[2]:l[3]]] = body[l[1]:end]
	}
	return r, true
}

// helperCall calls the app's helper. Errors carry the helper's own message
// ("error" in the result) or the content tool's.
func (a *app) helperCall(timeout time.Duration, method, arg string) (helperResult, error) {
	out, err := a.adb.shell(timeout, "content call --uri "+helperURI+" --method "+method+" --arg "+shQuote(arg))
	if err == errClosed {
		return nil, err
	}
	r, ok := parseBundle(out)
	if !ok {
		msg := strings.TrimSpace(out)
		switch {
		case strings.Contains(msg, "Result: null"):
			msg = "the OpenXWA app refused the request"
		case msg == "" && err != nil:
			msg = err.Error()
		case msg == "":
			msg = "no answer from the OpenXWA app"
		}
		return nil, errors.New(firstLine(msg))
	}
	if e, ok := r["error"]; ok {
		return r, errors.New(e)
	}
	return r, nil
}

// helperMkdirs makes folders (relative to the app's files folder) as the app.
func (a *app) helperMkdirs(rels ...string) error {
	r, err := a.helperCall(2*time.Minute, "mkdirs", strings.Join(rels, ":"))
	if err != nil {
		return err
	}
	if r["made"] != strconv.Itoa(len(rels)) {
		return fmt.Errorf("made %s of %d folders", r["made"], len(rels))
	}
	return nil
}

// tryHelper checks once whether the installed app has the helper, by
// letting it make the XWA folder.
func (a *app) tryHelper() bool {
	if a.helperTried {
		return a.helper
	}
	a.helperTried = true
	if err := a.helperMkdirs("XWA"); err != nil {
		logf("app helper not available: %v", err)
		return false
	}
	logf("app helper available")
	a.helper = true
	return true
}

// removeShellLeftovers deletes what earlier launcher versions copied over
// USB below XWA and pilot-import: it belongs to shell, so the app can
// neither read nor replace it.
func (a *app) removeShellLeftovers() error {
	dirs := shQuote(a.remoteGame()) + " " + shQuote(a.remotePilots())
	find := "find " + dirs + " -mindepth 1 -user " + strconv.Itoa(shellUID) + " -prune"
	out, err := a.adb.shell(5*time.Minute, find+" -print 2>/dev/null | wc -l")
	if err == errClosed {
		return err
	}
	n, perr := strconv.Atoi(strings.TrimSpace(firstLine(out)))
	if perr != nil || n == 0 {
		return nil
	}
	say("Removing what an earlier launcher copied over USB (%d files and folders):\nOpenXWA cannot read it. It is copied again.", n)
	if out, err := a.adb.shell(10*time.Minute, find+" -exec rm -rf {} + 2>&1"); err != nil {
		if err == errClosed {
			return err
		}
		return fail("Could not remove the unusable files on the headset:\n  %s", strings.TrimSpace(out))
	}
	return nil
}

// importItem is one file to put in the zip: a PC file, or data in memory.
type importItem struct {
	Rel  string
	Abs  string // "" when Data is used
	Data []byte
	Size int64
}

func itemsOf(files []gameFile) []importItem {
	out := make([]importItem, len(files))
	for i, f := range files {
		out[i] = importItem{Rel: f.Rel, Abs: f.Abs, Size: f.Size}
	}
	return out
}

// importBatches splits the items into zips of limited size and count.
func importBatches(items []importItem) [][]importItem {
	var out [][]importItem
	var cur []importItem
	var size int64
	for _, it := range items {
		if len(cur) > 0 && (size+it.Size > importBatchSize || len(cur) >= importBatchMax) {
			out = append(out, cur)
			cur, size = nil, 0
		}
		cur = append(cur, it)
		size += it.Size
	}
	if len(cur) > 0 {
		out = append(out, cur)
	}
	return out
}

// importFiles puts the items into dest (relative to the app's files folder,
// e.g. "XWA") through the app: zipped while sending, unpacked by the app.
// verb labels the progress display ("" shows none).
func (a *app) importFiles(verb string, items []importItem, dest string) error {
	var files []gameFile
	for _, it := range items {
		files = append(files, gameFile{Rel: it.Rel, Size: it.Size})
	}
	var p *copyProgress
	if verb == "" { // small and quick: no progress display
		p = &copyProgress{stop: make(chan struct{})}
	} else {
		p = newCopyProgress(verb, files)
	}
	defer p.finish()
	batches := importBatches(items)
	for bi, b := range batches {
		for try := 1; ; try++ {
			mark := p.snapshot()
			err := a.importBatch(p, b, dest)
			if err == nil {
				break
			}
			if err == errClosed || appCtx.Err() != nil {
				return errClosed
			}
			logf("import %d of %d failed (try %d): %v", bi+1, len(batches), try, err)
			if a.headsetGone() {
				return errDisconnected
			}
			if try == 3 {
				return fail("Could not copy the files to the headset:\n  %v\n\n"+
					"Check the USB cable (try another cable or USB port) and that the headset\n"+
					"has enough free space, then try again. Only what is missing is copied.", err)
			}
			p.restore(mark)
		}
	}
	return nil
}

type countWriter struct {
	w io.Writer
	n int64
}

func (c *countWriter) Write(b []byte) (int, error) {
	n, err := c.w.Write(b)
	c.n += int64(n)
	return n, err
}

type progressReader struct {
	r io.Reader
	p *copyProgress
}

func (r *progressReader) Read(b []byte) (int, error) {
	n, err := r.r.Read(b)
	r.p.addBytes(int64(n))
	return n, err
}

// writeZip writes the items as a zip (fast deflate; ZipInputStream needs
// deflated entries when sizes follow the data).
func writeZip(w io.Writer, items []importItem, p *copyProgress) error {
	zw := zip.NewWriter(w)
	zw.RegisterCompressor(zip.Deflate, func(out io.Writer) (io.WriteCloser, error) {
		return flate.NewWriter(out, flate.BestSpeed)
	})
	for _, it := range items {
		if p != nil {
			label := path.Dir(it.Rel)
			if label == "." {
				label = "main folder"
			}
			p.setFolder(label)
		}
		zf, err := zw.CreateHeader(&zip.FileHeader{Name: it.Rel, Method: zip.Deflate, Modified: time.Now()})
		if err != nil {
			return err
		}
		var src io.Reader
		var f *os.File
		if it.Abs != "" {
			if f, err = os.Open(it.Abs); err != nil {
				return fmt.Errorf("cannot read %s: %v", it.Rel, err)
			}
			src = f
		} else {
			src = strings.NewReader(string(it.Data))
		}
		if p != nil {
			src = &progressReader{r: src, p: p}
		}
		n, err := io.Copy(zf, src)
		if f != nil {
			f.Close()
		}
		if err != nil {
			return err
		}
		if n != it.Size {
			return fmt.Errorf("%s changed while it was copied (%d bytes, expected %d)", it.Rel, n, it.Size)
		}
		if p != nil {
			p.addFiles(1)
		}
	}
	return zw.Close()
}

// importBatch streams one zip into the app and lets it unpack it.
func (a *app) importBatch(p *copyProgress, items []importItem, dest string) error {
	pr, pw := io.Pipe()
	type result struct {
		n   int64
		err error
	}
	done := make(chan result, 1)
	go func() {
		cw := &countWriter{w: pw}
		err := writeZip(cw, items, p)
		pw.CloseWithError(err)
		done <- result{cw.n, err}
	}()
	out, err := a.adb.stream(0, pr, "exec-in", "content", "write", "--uri", helperURI+"/import")
	pr.CloseWithError(io.ErrClosedPipe) // adb stopped early: unblock the zip writer
	res := <-done
	if err != nil {
		return fmt.Errorf("sending failed: %v %s", err, strings.TrimSpace(out))
	}
	if res.err != nil {
		return fmt.Errorf("sending failed: %v", res.err)
	}
	if msg := strings.TrimSpace(out); msg != "" {
		logf("content write said: %s", msg)
	}
	if err := a.waitImportSize(res.n); err != nil {
		return err
	}
	p.unpacking()
	r, err := a.helperCall(0, "unzip", dest)
	if err != nil {
		return fmt.Errorf("unpacking on the headset failed: %v", err)
	}
	logf("unpacked: %v", r)
	if r["files"] != strconv.Itoa(len(items)) || (r["skipped"] != "" && r["skipped"] != "0") {
		return fmt.Errorf("the headset unpacked %s of %d files (%s skipped)", r["files"], len(items), r["skipped"])
	}
	return nil
}

// waitImportSize waits until the zip on the headset has all n bytes (the
// app may still be writing the end when adb returns).
func (a *app) waitImportSize(n int64) error {
	remote := a.remoteBase() + "/" + importZipName
	var last int64 = -1
	stable := 0
	for i := 0; i < 120; i++ {
		out, err := a.adb.runQuiet(30*time.Second, "shell", "stat -c %s "+shQuote(remote))
		if err == errClosed {
			return err
		}
		size, perr := strconv.ParseInt(firstLine(strings.TrimSpace(out)), 10, 64)
		if perr != nil {
			if i >= 4 {
				logf("cannot read the size of the zip on the headset (%q); the app checks it", strings.TrimSpace(out))
				return nil
			}
		} else {
			if size == n {
				logf("zip on the headset: %d bytes", n)
				return nil
			}
			if size == last {
				stable++
				if stable >= 10 {
					return fmt.Errorf("only %s of %s arrived", humanSize(size), humanSize(n))
				}
			} else {
				stable = 0
			}
			last = size
		}
		if !sleepCtx(500 * time.Millisecond) {
			return errClosed
		}
	}
	return fmt.Errorf("the headset did not finish writing the files")
}
