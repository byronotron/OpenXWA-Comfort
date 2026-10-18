package main

import (
	"fmt"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

// gameFile is one file of the PC game folder that belongs on the headset.
type gameFile struct {
	Rel  string // relative path with forward slashes, as on the PC
	Abs  string // full local path
	Size int64
}

var (
	skipExact    = map[string]bool{"desktop.ini": true, "thumbs.db": true}
	skipExts     = map[string]bool{".exe": true, ".dll": true, ".plt": true, ".lnk": true, ".ico": true, ".url": true, ".tmp": true}
	skipPrefixes = []string{"goggame-", "unins", "steam_api"}
)

// skipFileName reports files the port does not use (programs, shortcuts,
// store/installer files) and pilots, which are handled separately.
func skipFileName(name string) bool {
	l := strings.ToLower(name)
	if strings.HasPrefix(l, ".") || skipExact[l] || skipExts[path.Ext(l)] {
		return true
	}
	for _, p := range skipPrefixes {
		if strings.HasPrefix(l, p) {
			return true
		}
	}
	return false
}

// walkGame lists the files to copy, sorted by relative path.
func walkGame(root string) (files []gameFile, skipped int, err error) {
	err = filepath.WalkDir(root, func(p string, d fs.DirEntry, werr error) error {
		if werr != nil {
			return werr
		}
		if d.IsDir() {
			if p != root && strings.HasPrefix(d.Name(), ".") {
				return filepath.SkipDir
			}
			return nil
		}
		if !d.Type().IsRegular() {
			skipped++
			return nil
		}
		info, ierr := d.Info()
		if ierr != nil {
			return ierr
		}
		if skipFileName(d.Name()) || isHiddenSystemFile(info) {
			skipped++
			return nil
		}
		rel, rerr := filepath.Rel(root, p)
		if rerr != nil {
			return rerr
		}
		files = append(files, gameFile{Rel: filepath.ToSlash(rel), Abs: p, Size: info.Size()})
		return nil
	})
	sort.Slice(files, func(i, j int) bool { return files[i].Rel < files[j].Rel })
	return files, skipped, err
}

const listEndMarker = "__OPENXWA_LIST_END__"

// remoteListCommand prints "size|./relative/path" for every file below dir,
// then an end marker so a cut-off listing is noticed. A missing dir lists
// nothing.
func remoteListCommand(dir string) string {
	q := shQuote(dir)
	return "if [ -d " + q + " ]; then cd " + q + " && find . -type f -exec stat -c '%s|%n' {} +; fi; echo " + listEndMarker
}

// parseRemoteList turns the listing into lower-case relative path -> size
// (the headset's shared storage ignores case). complete is false when the
// end marker is missing.
func parseRemoteList(out string) (files map[string]int64, complete bool) {
	files = map[string]int64{}
	for _, line := range strings.Split(out, "\n") {
		line = strings.TrimRight(line, "\r")
		if strings.TrimSpace(line) == listEndMarker {
			complete = true
			continue
		}
		i := strings.IndexByte(line, '|')
		if i <= 0 {
			continue
		}
		size, err := strconv.ParseInt(strings.TrimSpace(line[:i]), 10, 64)
		if err != nil {
			continue
		}
		rel := strings.TrimPrefix(line[i+1:], "./")
		if rel == "" {
			continue
		}
		files[strings.ToLower(rel)] = size
	}
	return files, complete
}

// filesToCopy returns the local files that are missing on the headset or
// have a different size there.
func filesToCopy(local []gameFile, remote map[string]int64) []gameFile {
	var out []gameFile
	for _, f := range local {
		if size, ok := remote[strings.ToLower(f.Rel)]; !ok || size != f.Size {
			out = append(out, f)
		}
	}
	return out
}

// mkdirCommands builds "mkdir -p 'a' 'b' ..." commands for the folders that
// the files need, leaving out folders that a deeper one already creates,
// with each command shorter than maxLen characters.
func mkdirCommands(base string, files []gameFile, maxLen int) []string {
	dirs := map[string]bool{}
	for _, f := range files {
		if d := path.Dir(f.Rel); d != "." {
			dirs[d] = true
		}
	}
	parents := map[string]bool{}
	for d := range dirs {
		for p := path.Dir(d); p != "."; p = path.Dir(p) {
			parents[p] = true
		}
	}
	var leaves []string
	for d := range dirs {
		if !parents[d] {
			leaves = append(leaves, d)
		}
	}
	sort.Strings(leaves)
	var cmds []string
	cur := ""
	for _, d := range leaves {
		arg := shQuote(base + "/" + d)
		if cur != "" && len(cur)+1+len(arg) > maxLen {
			cmds = append(cmds, cur)
			cur = ""
		}
		if cur == "" {
			cur = "mkdir -p"
		}
		cur += " " + arg
	}
	if cur != "" {
		cmds = append(cmds, cur)
	}
	return cmds
}

// pushJob is one "adb push" call: a whole top-level folder or one file.
type pushJob struct {
	label  string
	local  string
	remote string
	files  []gameFile // the files this job is expected to bring over
}

const folderPushThreshold = 200

// planPushes groups the files by top-level folder. Folders with many files
// to copy are pushed as a whole (much faster); the rest go file by file.
// Step "verify" catches anything a folder push silently skipped.
func planPushes(root, base string, files []gameFile) []pushJob {
	groups := map[string][]gameFile{}
	var tops []string
	for _, f := range files {
		top := ""
		if i := strings.IndexByte(f.Rel, '/'); i >= 0 {
			top = f.Rel[:i]
		}
		if _, ok := groups[top]; !ok {
			tops = append(tops, top)
		}
		groups[top] = append(groups[top], f)
	}
	sort.Strings(tops) // "" (loose files in the main folder) comes first
	var jobs []pushJob
	for _, top := range tops {
		g := groups[top]
		if top != "" && len(g) > folderPushThreshold {
			jobs = append(jobs, pushJob{
				label:  top,
				local:  filepath.Join(root, filepath.FromSlash(top)) + string(filepath.Separator) + ".",
				remote: base + "/" + top + "/",
				files:  g,
			})
			continue
		}
		for _, f := range g {
			jobs = append(jobs, singleFileJob(base, f))
		}
	}
	return jobs
}

func singleFileJob(base string, f gameFile) pushJob {
	label := path.Dir(f.Rel)
	if label == "." {
		label = "main folder"
	}
	return pushJob{label: label, local: f.Abs, remote: base + "/" + f.Rel, files: []gameFile{f}}
}

// plural returns "1 file" or "3 files".
func plural(n int, word string) string {
	if n == 1 {
		return "1 " + word
	}
	return fmt.Sprintf("%d %ss", n, word)
}

func totalSize(files []gameFile) int64 {
	var n int64
	for _, f := range files {
		n += f.Size
	}
	return n
}

// copyProgress draws "Copying: 412 of 1530 files (380 MB of 1.02 GB)  FOLDER".
// While a long folder push runs, a small spinner shows that it is working.
type copyProgress struct {
	mu         sync.Mutex
	line       progressLine
	verb       string
	doneFiles  int
	totalFiles int
	doneBytes  int64
	totalBytes int64
	folder     string
	spin       int
	stop       chan struct{}
}

func newCopyProgress(verb string, files []gameFile) *copyProgress {
	p := &copyProgress{verb: verb, totalFiles: len(files), totalBytes: totalSize(files), stop: make(chan struct{})}
	go func() {
		t := time.NewTicker(250 * time.Millisecond)
		defer t.Stop()
		for {
			select {
			case <-p.stop:
				return
			case <-t.C:
				p.mu.Lock()
				p.spin++
				p.mu.Unlock()
				p.draw(false)
			}
		}
	}()
	return p
}

func (p *copyProgress) text(final bool) string {
	p.mu.Lock()
	defer p.mu.Unlock()
	s := fmt.Sprintf("%s: %d of %d files (%s of %s)", p.verb, p.doneFiles, p.totalFiles, humanSize(p.doneBytes), humanSize(p.totalBytes))
	if !final && p.folder != "" {
		spinner := `|/-\`[p.spin%4 : p.spin%4+1]
		room := progressWidth - len(s) - 4
		f := p.folder
		if room < 4 {
			f = ""
		} else if len(f) > room {
			f = "..." + f[len(f)-room+3:]
		}
		s += "  " + f + " " + spinner
	}
	return s
}

func (p *copyProgress) draw(force bool) { p.line.show(p.text(false), force) }

func (p *copyProgress) setFolder(f string) {
	p.mu.Lock()
	p.folder = f
	p.mu.Unlock()
	p.draw(false)
}

func (p *copyProgress) add(files []gameFile) {
	p.mu.Lock()
	p.doneFiles += len(files)
	p.doneBytes += totalSize(files)
	p.mu.Unlock()
	p.draw(false)
}

func (p *copyProgress) finish() {
	close(p.stop)
	p.line.done(p.text(true))
}

// listRemote lists the files below a headset folder with their sizes.
func (a *app) listRemote(dir string) (map[string]int64, error) {
	out, err := a.adb.shell(10*time.Minute, remoteListCommand(dir))
	files, complete := parseRemoteList(out)
	if !complete {
		if err == nil {
			err = fmt.Errorf("the file list from the headset was incomplete")
		}
		return nil, err
	}
	return files, nil
}

func (a *app) headsetGone() bool {
	out, err := a.adb.run(20*time.Second, "get-state")
	return err != nil || strings.TrimSpace(out) != "device"
}

var errDisconnected = &userError{msg: "The headset was disconnected. Check the USB cable and run the launcher\n" +
	"again; it will continue where it stopped and only copy what is missing."}

// runPushes runs the jobs with a progress line.
func (a *app) runPushes(verb string, jobs []pushJob, files []gameFile) error {
	p := newCopyProgress(verb, files)
	for _, j := range jobs {
		p.setFolder(j.label)
		out, err := a.adb.run(0, "push", j.local, j.remote)
		if err != nil || strings.Contains(out, "adb: error") {
			logf("push of %s reported a problem", j.local)
			if a.headsetGone() {
				p.finish()
				return errDisconnected
			}
		}
		p.add(j.files)
	}
	p.finish()
	return nil
}

// freeSpaceKB parses "df -k" output and returns the available KB.
func parseDfAvailable(out string) (int64, bool) {
	lines := strings.Split(strings.TrimSpace(out), "\n")
	if len(lines) < 2 {
		return 0, false
	}
	// Skip the header; the filesystem name may sit on its own line, so look
	// for the first three numbers in a row: size, used, available.
	toks := strings.Fields(strings.Join(lines[1:], " "))
	for i := 0; i+2 < len(toks); i++ {
		var n [3]int64
		ok := true
		for k := 0; k < 3; k++ {
			v, err := strconv.ParseInt(toks[i+k], 10, 64)
			if err != nil {
				ok = false
				break
			}
			n[k] = v
		}
		if ok {
			return n[2], true
		}
	}
	return 0, false
}

// checkFreeSpace warns when the headset may be too full for need bytes.
func (a *app) checkFreeSpace(need int64) error {
	out, err := a.adb.shell(30*time.Second, "df -k "+shQuote(remoteBase))
	if err != nil {
		logf("df failed; skipping the free space check")
		return nil
	}
	availKB, ok := parseDfAvailable(out)
	if !ok {
		logf("could not parse df output; skipping the free space check")
		return nil
	}
	free := availKB * 1024
	const margin = 200 << 20
	logf("headset free space %d bytes, need %d", free, need)
	if free >= need+margin {
		return nil
	}
	say("")
	say("WARNING: the headset may not have enough free space.")
	say("  Needed: about %s    Free on the headset: %s", humanSize(need), humanSize(free))
	say("Free up space on the headset (for example uninstall games you no longer play).")
	if !a.ui.yesNo("Continue anyway? (y/N)", false) {
		return fail("Stopped so you can free up space on the headset. Run the launcher again\nafterwards; it only copies what is missing.")
	}
	return nil
}

// copyGame is step 7.
func (a *app) copyGame() error {
	say("Checking the game folder...")
	files, skipped, err := walkGame(a.gameDir)
	if err != nil {
		return fail("Could not read the game folder:\n  %v", err)
	}
	logf("%d game files, %d skipped", len(files), skipped)
	say("%d game files (%s).", len(files), humanSize(totalSize(files)))

	say("Checking what is already on the headset...")
	remote, err := a.listRemote(remoteGame)
	if err != nil {
		return fail("Could not read the files on the headset (%v).\nCheck the USB cable and try again.", err)
	}
	todo := filesToCopy(files, remote)
	if len(todo) == 0 {
		say("All game files are already on the headset. Nothing to copy.")
	} else {
		if len(todo) == len(files) {
			say("%s to copy (%s).", plural(len(todo), "file"), humanSize(totalSize(todo)))
		} else {
			say("%s to copy (new or changed, %s).", plural(len(todo), "file"), humanSize(totalSize(todo)))
		}
		if err := a.checkFreeSpace(totalSize(todo)); err != nil {
			return err
		}
		// Create every folder first: pushing into missing folders is where
		// files went missing before.
		for _, c := range mkdirCommands(remoteGame, todo, 4000) {
			if out, err := a.adb.shell(2*time.Minute, c); err != nil {
				return fail("Could not create the folders on the headset:\n  %s", strings.TrimSpace(out))
			}
		}
		if err := a.runPushes("Copying", planPushes(a.gameDir, remoteGame, todo), todo); err != nil {
			return err
		}
		if err := a.verifyGame(files); err != nil {
			return err
		}
	}
	return a.writeManifest(files)
}

// verifyGame lists the headset again and re-copies missing or wrong-size
// files one by one, up to 3 times.
func (a *app) verifyGame(files []gameFile) error {
	say("Checking the copied files...")
	const rounds = 3
	for round := 1; ; round++ {
		remote, err := a.listRemote(remoteGame)
		if err != nil {
			if a.headsetGone() {
				return errDisconnected
			}
			return fail("Could not read the files on the headset (%v).\nCheck the USB cable and try again.", err)
		}
		bad := filesToCopy(files, remote)
		if len(bad) == 0 {
			say("All %d game files are on the headset and checked.", len(files))
			return nil
		}
		logf("verify round %d: %d files missing or wrong size", round, len(bad))
		if round > rounds {
			var b strings.Builder
			b.WriteString("These files could not be copied to the headset:\n")
			for i, f := range bad {
				if i == 20 {
					fmt.Fprintf(&b, "  ... and %d more\n", len(bad)-20)
					break
				}
				fmt.Fprintf(&b, "  %s\n", f.Rel)
			}
			b.WriteString("\nPlease check the USB cable (try another cable or USB port) and that the\n" +
				"headset has enough free space, then run the launcher again. It will only\n" +
				"copy what is missing.")
			return &userError{msg: b.String()}
		}
		say("%s did not arrive correctly. Copying again (try %d of %d)...", plural(len(bad), "file"), round, rounds)
		var jobs []pushJob
		for _, f := range bad {
			jobs = append(jobs, singleFileJob(remoteGame, f))
		}
		if err := a.runPushes("Copying again", jobs, bad); err != nil {
			return err
		}
	}
}

// manifestText is the install file list the app reads at start to name
// missing files: a header line, then "<size>\t<relative/path>" lines.
func manifestText(files []gameFile) string {
	var b strings.Builder
	b.WriteString(manifestHeader + "\n")
	for _, f := range files {
		fmt.Fprintf(&b, "%d\t%s\n", f.Size, f.Rel)
	}
	return b.String()
}

func (a *app) writeManifest(files []gameFile) error {
	text := manifestText(files)
	tmp, err := os.CreateTemp("", "openxwa-install-files-*.txt")
	if err != nil {
		return fail("Could not write a temporary file: %v", err)
	}
	tmpPath := tmp.Name()
	defer os.Remove(tmpPath)
	if _, err := tmp.WriteString(text); err != nil {
		tmp.Close()
		return fail("Could not write a temporary file: %v", err)
	}
	tmp.Close()
	remote := remoteGame + "/" + manifestName
	for try := 1; try <= 3; try++ {
		a.adb.run(2*time.Minute, "push", tmpPath, remote)
		out, _ := a.adb.shell(30*time.Second, "stat -c %s "+shQuote(remote))
		if n, err := strconv.ParseInt(firstLine(out), 10, 64); err == nil && n == int64(len(text)) {
			logf("install file list written (%d files)", len(files))
			return nil
		}
	}
	return fail("Could not write the install file list to the headset.\nCheck the USB cable and try again.")
}
