package main

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
)

// requiredGameFiles must exist (case-insensitively) in a valid game folder.
var requiredGameFiles = []string{
	"RESDATA.TXT",
	"FLIGHTMODELS/SPACECRAFT0.LST",
	"MISSIONS/MISSION.LST",
	"MOVIES/PROLOGUE.SNM",
	"MOVIES/BATTLE1.SNM",
	"WAVE/FRONTEND/B1M1/N010101.WAV",
}

// findCaseInsensitive resolves a slash-separated relative path below dir,
// matching each part without regard to case. Returns "" if not found.
func findCaseInsensitive(dir, rel string) string {
	cur := dir
	for _, part := range strings.Split(rel, "/") {
		exact := filepath.Join(cur, part)
		if _, err := os.Lstat(exact); err == nil {
			cur = exact
			continue
		}
		entries, err := os.ReadDir(cur)
		if err != nil {
			return ""
		}
		found := ""
		for _, e := range entries {
			if strings.EqualFold(e.Name(), part) {
				found = filepath.Join(cur, e.Name())
				break
			}
		}
		if found == "" {
			return ""
		}
		cur = found
	}
	return cur
}

// gameCheck is the result of looking at one folder.
type gameCheck struct {
	ok       bool
	unmerged bool     // ALLIANCE/RESDATA.TXT exists: an unmerged CD copy
	missing  []string // required files that are missing
}

func checkGameDir(dir string) gameCheck {
	var c gameCheck
	if !isDir(dir) {
		c.missing = requiredGameFiles
		return c
	}
	for _, f := range requiredGameFiles {
		if findCaseInsensitive(dir, f) == "" {
			c.missing = append(c.missing, f)
		}
	}
	c.ok = len(c.missing) == 0
	if !c.ok && findCaseInsensitive(dir, "ALLIANCE/RESDATA.TXT") != "" {
		c.unmerged = true
	}
	return c
}

// findGameRoot looks for a valid game folder at dir itself or up to depth
// folders below it. It returns the valid root (or "") and, if none is valid,
// the folder of an unmerged CD copy if one was seen.
func findGameRoot(dir string, depth int) (root string, unmerged string) {
	c := checkGameDir(dir)
	if c.ok {
		return dir, ""
	}
	if c.unmerged {
		unmerged = dir
	}
	if depth <= 0 {
		return "", unmerged
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		return "", unmerged
	}
	for _, e := range entries {
		if !isDir(filepath.Join(dir, e.Name())) || strings.HasPrefix(e.Name(), ".") {
			continue
		}
		r, u := findGameRoot(filepath.Join(dir, e.Name()), depth-1)
		if r != "" {
			return r, ""
		}
		if unmerged == "" {
			unmerged = u
		}
	}
	return "", unmerged
}

// parseVDFPaths returns the library folder paths in Steam's
// libraryfolders.vdf. New files have "path" keys; old ones list the extra
// libraries as "1" "D:\\SteamLibrary".
func parseVDFPaths(data string) []string {
	toks := vdfTokens(data)
	var paths []string
	for i := 0; i+1 < len(toks); i++ {
		k, v := toks[i], toks[i+1]
		if !k.quoted || !v.quoted {
			continue
		}
		key := strings.ToLower(k.text)
		_, numErr := strconv.Atoi(key)
		if key == "path" || (numErr == nil && looksLikePath(v.text)) {
			paths = append(paths, v.text)
			i++
		}
	}
	return paths
}

func looksLikePath(s string) bool {
	return strings.Contains(s, "/") || strings.Contains(s, "\\") || (len(s) >= 2 && s[1] == ':')
}

type vdfToken struct {
	text   string
	quoted bool
}

// vdfTokens splits Valve's KeyValues text into quoted strings and braces,
// handling \\ and \" escapes and // comments.
func vdfTokens(s string) []vdfToken {
	var toks []vdfToken
	for i := 0; i < len(s); {
		c := s[i]
		switch {
		case c == '"':
			var b strings.Builder
			i++
			for i < len(s) && s[i] != '"' {
				if s[i] == '\\' && i+1 < len(s) {
					switch s[i+1] {
					case '\\', '"':
						b.WriteByte(s[i+1])
					case 'n':
						b.WriteByte('\n')
					case 't':
						b.WriteByte('\t')
					default:
						b.WriteByte('\\')
						b.WriteByte(s[i+1])
					}
					i += 2
					continue
				}
				b.WriteByte(s[i])
				i++
			}
			i++ // closing quote
			toks = append(toks, vdfToken{text: b.String(), quoted: true})
		case c == '{' || c == '}':
			toks = append(toks, vdfToken{text: string(c)})
			i++
		case c == '/' && i+1 < len(s) && s[i+1] == '/':
			for i < len(s) && s[i] != '\n' {
				i++
			}
		default:
			i++
		}
	}
	return toks
}

// steamLibraries returns the Steam library folders below a Steam install.
func steamLibraries(steamRoot string) []string {
	libs := []string{steamRoot}
	for _, rel := range []string{"steamapps/libraryfolders.vdf", "config/libraryfolders.vdf"} {
		data, err := os.ReadFile(filepath.Join(steamRoot, filepath.FromSlash(rel)))
		if err != nil {
			continue
		}
		libs = append(libs, parseVDFPaths(string(data))...)
	}
	return libs
}

// steamGameDirs returns steamapps/common folders whose name mentions "alliance".
func steamGameDirs(steamRoots []string) []string {
	var out []string
	for _, root := range steamRoots {
		for _, lib := range steamLibraries(root) {
			out = append(out, globAlliance(filepath.Join(lib, "steamapps", "common"))...)
		}
	}
	return out
}

// globAlliance lists the sub-folders of dir whose name contains "alliance".
func globAlliance(dir string) []string {
	var out []string
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	for _, e := range entries {
		if isDir(filepath.Join(dir, e.Name())) && strings.Contains(strings.ToLower(e.Name()), "alliance") {
			out = append(out, filepath.Join(dir, e.Name()))
		}
	}
	return out
}

// subDirs lists all sub-folders of dir.
func subDirs(dir string) []string {
	var out []string
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	for _, e := range entries {
		if isDir(filepath.Join(dir, e.Name())) {
			out = append(out, filepath.Join(dir, e.Name()))
		}
	}
	return out
}

// gameSearchDirs collects folders that may contain the game; each one is
// checked with findGameRoot afterwards.
func gameSearchDirs() []string {
	var dirs []string
	home, _ := os.UserHomeDir()
	var steamRoots []string
	if runtime.GOOS == "windows" {
		steamRoots = append(steamRoots, windowsSteamRoots()...)
		dirs = append(dirs, windowsRegistryGameDirs()...)
		pf86 := os.Getenv("ProgramFiles(x86)")
		pf := os.Getenv("ProgramFiles")
		for _, p := range []string{pf86, pf} {
			if p == "" {
				continue
			}
			steamRoots = append(steamRoots, filepath.Join(p, "Steam"))
			dirs = append(dirs, globAlliance(filepath.Join(p, "GOG Galaxy", "Games"))...)
			dirs = append(dirs, globAlliance(filepath.Join(p, "GOG.com"))...)
			dirs = append(dirs, subDirs(filepath.Join(p, "LucasArts"))...)
		}
		for _, drive := range "CDEFGH" {
			d := string(drive) + `:\`
			dirs = append(dirs, globAlliance(filepath.Join(d, "GOG Games"))...)
			dirs = append(dirs, globAlliance(filepath.Join(d, "Games"))...)
		}
	} else if home != "" {
		steamRoots = append(steamRoots,
			filepath.Join(home, ".steam", "steam"),
			filepath.Join(home, ".local", "share", "Steam"),
			filepath.Join(home, ".var", "app", "com.valvesoftware.Steam", ".local", "share", "Steam"),
			filepath.Join(home, "Library", "Application Support", "Steam"),
		)
		dirs = append(dirs, globAlliance(filepath.Join(home, "GOG Games"))...)
		dirs = append(dirs, globAlliance(filepath.Join(home, "Games"))...)
	}
	dirs = append(dirs, steamGameDirs(steamRoots)...)
	return dirs
}

// pathKey makes paths comparable for de-duplication.
func pathKey(p string) string {
	if r, err := filepath.EvalSymlinks(p); err == nil {
		p = r
	}
	if abs, err := filepath.Abs(p); err == nil {
		p = abs
	}
	p = filepath.Clean(p)
	if runtime.GOOS == "windows" || runtime.GOOS == "darwin" {
		p = strings.ToLower(p)
	}
	return p
}

// detectGames returns the valid game folders found on this computer, plus
// unmerged CD copies (only reported when there is no valid folder).
func detectGames() (valid []string, unmerged []string) {
	seen := map[string]bool{}
	seenU := map[string]bool{}
	for _, d := range gameSearchDirs() {
		root, u := findGameRoot(d, 2)
		if root != "" {
			k := pathKey(root)
			if !seen[k] {
				seen[k] = true
				valid = append(valid, root)
			}
		} else if u != "" && !seenU[pathKey(u)] {
			seenU[pathKey(u)] = true
			unmerged = append(unmerged, u)
		}
	}
	sort.Strings(valid)
	return valid, unmerged
}

// cleanUserPath removes quotes and spaces around a typed or pasted path.
func cleanUserPath(s string) string {
	s = strings.TrimSpace(s)
	for len(s) >= 2 && ((s[0] == '"' && s[len(s)-1] == '"') || (s[0] == '\'' && s[len(s)-1] == '\'')) {
		s = strings.TrimSpace(s[1 : len(s)-1])
	}
	s = strings.Trim(s, `"`)
	if strings.HasPrefix(s, "~/") || s == "~" {
		if home, err := os.UserHomeDir(); err == nil {
			s = filepath.Join(home, s[1:])
		}
	}
	if s != "" {
		s = filepath.Clean(s)
	}
	return s
}

func unmergedMessage(dir string) error {
	return fail("This looks like a copy of the original CD that was not merged yet:\n"+
		"  %s\n"+
		"The game files are inside its ALLIANCE folder. Please move everything from\n"+
		"the ALLIANCE folder up into the main folder (so that RESDATA.TXT is directly\n"+
		"in %s), then run this launcher again.", dir, filepath.Base(dir))
}

// tryUserFolder checks a folder named by the user. It returns the valid game
// root, or a problem text saying why the folder is not usable.
func tryUserFolder(p string) (root, problem string, err error) {
	if p == "" {
		return "", "Please choose a folder.", nil
	}
	if !isDir(p) {
		return "", "That folder does not exist: " + p, nil
	}
	root, unmerged := findGameRoot(p, 2)
	if root != "" {
		return root, "", nil
	}
	if unmerged != "" {
		return "", "", unmergedMessage(unmerged)
	}
	c := checkGameDir(p)
	return "", "This is not an X-Wing Alliance game folder: " + p + "\n" +
		"These files are missing: " + strings.Join(c.missing, ", ") + "\n" +
		"Choose the folder that contains RESDATA.TXT and the FLIGHTMODELS, MISSIONS,\n" +
		"MOVIES and WAVE folders.", nil
}

func folderQuestion(text, value, problem string) Question {
	prompt := "Folder:"
	if value != "" {
		prompt = "Press Enter to use it, or type another folder path:"
	}
	return Question{
		Kind:          "text",
		Title:         "Where is X-Wing Alliance?",
		Text:          text,
		Value:         value,
		Problem:       problem,
		Placeholder:   `for example C:\GOG Games\Star Wars - X-Wing Alliance`,
		Browse:        true,
		Options:       []Option{{ID: "ok", Label: "Use this folder", Style: "primary"}},
		ConsolePrompt: prompt,
	}
}

func (a *app) setGame(root string) error {
	a.gameDir = root
	say("Game folder: %s", root)
	return nil
}

func noFolderChosen() error {
	if appCtx.Err() != nil {
		return errClosed
	}
	return fail("No game folder was chosen.")
}

// chooseGame is step 2: use the folder from the command line, or find the
// game automatically and let the user confirm or choose.
func (a *app) chooseGame(arg string) error {
	problem := ""
	if arg != "" {
		root, prob, err := tryUserFolder(cleanUserPath(arg))
		if err != nil {
			return err
		}
		if root != "" {
			return a.setGame(root)
		}
		problem = prob
	}

	text := "Choose the folder that contains RESDATA.TXT."
	value := ""
	if arg == "" {
		say("Looking for X-Wing Alliance on this computer...")
		valid, unmerged := detectGames()
		logf("detected games: %q; unmerged: %q", valid, unmerged)
		switch {
		case len(valid) == 1:
			text, value = "Found X-Wing Alliance here:", valid[0]
		case len(valid) > 1:
			var opts []Option
			for i, v := range valid {
				opts = append(opts, Option{ID: fmt.Sprint(i), Label: v, Key: fmt.Sprint(i + 1)})
			}
			opts = append(opts, Option{ID: "other", Label: "Another folder...", Key: "o"})
			for {
				ans := ui.Ask(Question{
					Kind:      "buttons",
					Title:     "Which X-Wing Alliance folder?",
					Text:      "Found X-Wing Alliance in several places. Choose the one to use:",
					Problem:   problem,
					Options:   opts,
					AllowText: true,
				})
				if ans.Closed {
					return noFolderChosen()
				}
				if ans.ID == "other" {
					break
				}
				if ans.ID == "text" {
					root, prob, err := tryUserFolder(cleanUserPath(ans.Text))
					if err != nil {
						return err
					}
					if root != "" {
						return a.setGame(root)
					}
					problem = prob
					continue
				}
				var n int
				fmt.Sscan(ans.ID, &n)
				if n >= 0 && n < len(valid) {
					return a.setGame(valid[n])
				}
			}
		default:
			if len(unmerged) > 0 {
				return unmergedMessage(unmerged[0])
			}
			text = "X-Wing Alliance was not found automatically.\n" + text
		}
	}
	if runtime.GOOS == "windows" {
		if _, console := ui.(*consoleUI); console {
			text += "\nTip: in Explorer, hold Shift, right-click the folder and choose \"Copy as path\"."
		}
	}

	for {
		ans := ui.Ask(folderQuestion(text, value, problem))
		if ans.Closed {
			return noFolderChosen()
		}
		root, prob, err := tryUserFolder(cleanUserPath(ans.Text))
		if err != nil {
			return err
		}
		if root != "" {
			return a.setGame(root)
		}
		say("%s", prob)
		problem = prob
		value = ""
		if _, console := ui.(*consoleUI); console {
			problem = "" // already printed
		}
	}
}
