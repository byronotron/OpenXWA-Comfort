package main

import (
	"archive/zip"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func TestCompareVersions(t *testing.T) {
	cases := []struct {
		a, b string
		want int
	}{
		{"1.0.0", "1.0.0", 0},
		{"v1.0.1", "1.0.0", 1},
		{"1.0.0", "1.0.0-rc3", 1},
		{"1.0.0-rc3", "1.0.0-rc2", 1},
		{"1.0.0-rc10", "1.0.0-rc9", 1},
		{"1.0.0-rc3", "1.0.0", -1},
		{"1.1", "1.0.9", 1},
		{"2.0.0-rc1", "1.9.9", 1},
		{"dev", "0.0.1", -1},
		{"dev", "0.0.0", 0},
		{"1.0.0", "dev", 1},
		{"1.0.0+build5", "1.0.0", 0},
	}
	for _, c := range cases {
		if got := compareVersions(c.a, c.b); got != c.want {
			t.Errorf("compareVersions(%q, %q) = %d, want %d", c.a, c.b, got, c.want)
		}
	}
}

func TestParseVDFPaths(t *testing.T) {
	newFmt := `"libraryfolders"
{
	"0"
	{
		"path"		"C:\\Program Files (x86)\\Steam"
		"label"		""
		"apps" { "228980" "1" }
	}
	"1"
	{
		"path"		"D:\\SteamLibrary"   // comment
	}
}`
	got := parseVDFPaths(newFmt)
	want := []string{`C:\Program Files (x86)\Steam`, `D:\SteamLibrary`}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("new format: got %q want %q", got, want)
	}
	oldFmt := `"LibraryFolders"
{
	"TimeNextStatsReport"		"1561832478"
	"ContentStatsID"		"-158337411836"
	"1"		"E:\\Games\\Steam"
}`
	got = parseVDFPaths(oldFmt)
	want = []string{`E:\Games\Steam`}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("old format: got %q want %q", got, want)
	}
}

func TestSkipFileName(t *testing.T) {
	skip := []string{"XWINGALLIANCE.EXE", "ddraw.dll", "Luke0.plt", "game.LNK", "goggame-1234.info", "unins000.dat",
		"steam_api.dll", "steam_api64.dll", "desktop.ini", "Thumbs.db", ".DS_Store", "x.tmp", "icon.ico", "site.url"}
	keep := []string{"RESDATA.TXT", "SPACECRAFT0.LST", "N010101.WAV", "readme.txt", "exefile.dat", "plt.txt"}
	for _, n := range skip {
		if !skipFileName(n) {
			t.Errorf("%q should be skipped", n)
		}
	}
	for _, n := range keep {
		if skipFileName(n) {
			t.Errorf("%q should be kept", n)
		}
	}
}

func TestParseRemoteList(t *testing.T) {
	out := "12|./RESDATA.TXT\n" +
		"0|./MISSIONS/empty file.txt\n" +
		"345|./My Mods/it's here|odd.txt\n" +
		"stat: './x': Permission denied\n" +
		"garbage line\n" +
		listEndMarker + "\n"
	files, ok := parseRemoteList(out)
	if !ok {
		t.Fatal("marker not seen")
	}
	want := map[string]int64{"resdata.txt": 12, "missions/empty file.txt": 0, "my mods/it's here|odd.txt": 345}
	if !reflect.DeepEqual(files, want) {
		t.Errorf("got %v", files)
	}
	if _, ok := parseRemoteList("12|./a\n"); ok {
		t.Error("listing without marker must be incomplete")
	}
	local := []gameFile{{Rel: "RESDATA.TXT", Size: 12}, {Rel: "Missions/Empty File.txt", Size: 1}, {Rel: "NEW.TXT", Size: 1}}
	todo := filesToCopy(local, files)
	if len(todo) != 2 || todo[0].Rel != "Missions/Empty File.txt" || todo[1].Rel != "NEW.TXT" {
		t.Errorf("filesToCopy = %v", todo)
	}
}

func TestShQuoteAndMkdir(t *testing.T) {
	if got := shQuote("it's a b"); got != `'it'\''s a b'` {
		t.Errorf("shQuote = %s", got)
	}
	files := []gameFile{
		{Rel: "A/B/C/f1"}, {Rel: "A/B/f2"}, {Rel: "A B/f3"}, {Rel: "root.txt"}, {Rel: "Q/it's/f"},
	}
	cmds := mkdirCommands("/base", files, 4000)
	want := []string{`mkdir -p '/base/A B' '/base/A/B/C' '/base/Q/it'\''s'`}
	if !reflect.DeepEqual(cmds, want) {
		t.Errorf("mkdir = %q", cmds)
	}
	// Splitting keeps every command under the limit.
	var many []gameFile
	for i := 0; i < 500; i++ {
		many = append(many, gameFile{Rel: strings.Repeat("d", 20) + "/" + string(rune('a'+i%26)) + strings.Repeat("x", i) + "/f"})
	}
	cmds = mkdirCommands("/sdcard/Android/data/org.openxwa.quest/files/XWA", many, 4000)
	count := 0
	for _, c := range cmds {
		if len(c) > 4000 && strings.Count(c, "'/sdcard") > 1 {
			t.Errorf("command too long: %d", len(c))
		}
		count += strings.Count(c, "'/sdcard")
	}
	if count != 500 {
		t.Errorf("got %d dirs, want 500", count)
	}
}

func TestPlanPushes(t *testing.T) {
	var files []gameFile
	for i := 0; i < 250; i++ {
		files = append(files, gameFile{Rel: "BIG/sub/f" + string(rune('a'+i%26)) + strings.Repeat("1", i/26), Size: 1})
	}
	files = append(files, gameFile{Rel: "SMALL/x.txt", Abs: "/g/SMALL/x.txt"}, gameFile{Rel: "RESDATA.TXT", Abs: "/g/RESDATA.TXT"})
	jobs := planPushes("/g", "/r", files)
	if len(jobs) != 3 {
		t.Fatalf("got %d jobs", len(jobs))
	}
	if jobs[0].remote != "/r/RESDATA.TXT" || jobs[1].local != filepath.Join("/g", "BIG")+string(filepath.Separator)+"." || jobs[1].remote != "/r/BIG/" || len(jobs[1].files) != 250 || jobs[2].remote != "/r/SMALL/x.txt" {
		t.Errorf("jobs: %+v %+v %+v", jobs[0], jobs[1].remote, jobs[2])
	}
}

func TestParseDf(t *testing.T) {
	toybox := "Filesystem     1K-blocks    Used Available Use% Mounted on\n/dev/fuse      110000000 9000000 101000000   9% /storage/emulated\n"
	if n, ok := parseDfAvailable(toybox); !ok || n != 101000000 {
		t.Errorf("toybox: %d %v", n, ok)
	}
	wrapped := "Filesystem 1K-blocks Used Available Use% Mounted on\n/dev/very/long/name\n 1000 400 600 40% /x\n"
	if n, ok := parseDfAvailable(wrapped); !ok || n != 600 {
		t.Errorf("wrapped: %d %v", n, ok)
	}
	if _, ok := parseDfAvailable("df: /x: No such file"); ok {
		t.Error("error output must not parse")
	}
}

func TestParseSHA256Sums(t *testing.T) {
	h := strings.Repeat("ab", 32)
	text := h + "  OpenXWA-Quest-1.0.1.zip\n" + strings.Repeat("CD", 32) + " *OpenXWA-quest-1.0.1.apk\r\nbad line\n"
	sums := parseSHA256Sums(strings.ReplaceAll(text, "\r", ""))
	if sums["OpenXWA-Quest-1.0.1.zip"] != h || sums["OpenXWA-quest-1.0.1.apk"] != strings.Repeat("cd", 32) || len(sums) != 2 {
		t.Errorf("sums = %v", sums)
	}
}

func TestDevicesAndNames(t *testing.T) {
	out := "List of devices attached\n" +
		"1WMHH000000000\tdevice usb:1-1 product:eureka model:Quest_3 device:eureka transport_id:3\n" +
		"PA7L10MGF\tunauthorized usb:1-2 transport_id:4\n" +
		"0123\tno permissions (user in plugdev group); see [http://developer.android.com/tools/device.html]\n\n"
	devs := parseDevices(out)
	if len(devs) != 3 || devs[0].model != "Quest 3" || devs[1].state != "unauthorized" || devs[2].state != "no permissions" {
		t.Errorf("devices = %+v", devs)
	}
	if !strings.Contains(waitMessage(devs[1:2]), "Allow") || !strings.Contains(waitMessage(nil), "USB-C") {
		t.Error("wait messages")
	}
	names := map[[2]string]string{
		{"Oculus", "Quest 3"}:  "Meta Quest 3",
		{"Meta", "Quest 3S"}:   "Meta Quest 3S",
		{"Oculus", "Quest"}:    "Meta Quest",
		{"Pico", "PICO 4"}:     "PICO 4",
		{"Valve", "Frame"}:     "Valve Frame",
		{"", ""}:               "headset",
		{"Oculus", "Oculus X"}: "Oculus X",
	}
	for in, want := range names {
		if got := deviceDisplayName(in[0], in[1]); got != want {
			t.Errorf("deviceDisplayName(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestInstalledVersionAndPilots(t *testing.T) {
	dump := "Activity Resolver Table:\n  versionName=9.9 (other)\nPackages:\n  Package [org.openxwa.quest] (c0ffee):\n    versionCode=7 minSdk=29\n    versionName=1.0.0-rc3\n"
	if v, ok := installedVersion(dump); !ok || v != "1.0.0-rc3" {
		t.Errorf("installedVersion = %q %v", v, ok)
	}
	if _, ok := installedVersion("Unable to find package: org.openxwa.quest"); ok {
		t.Error("not installed expected")
	}
	if apkVersionFromName("OpenXWA-quest-1.0.0-rc3.apk") != "1.0.0-rc3" || apkVersionFromName("other.apk") != "" {
		t.Error("apkVersionFromName")
	}
	names := pilotNames([]gameFile{{Rel: "Luke0.plt"}, {Rel: "Luke1.plt"}, {Rel: "Wedge0.plt"}, {Rel: "X.plt"}})
	if !reflect.DeepEqual(names, []string{"Luke", "Wedge", "X"}) {
		t.Errorf("pilotNames = %q", names)
	}
}

func TestGameDetection(t *testing.T) {
	dir := t.TempDir()
	game := filepath.Join(dir, "Star Wars X-Wing Alliance", "game")
	for _, f := range []string{"resdata.txt", "FlightModels/Spacecraft0.lst", "MISSIONS/MISSION.LST", "MOVIES/PROLOGUE.SNM", "MOVIES/battle1.snm", "WAVE/FRONTEND/B1M1/N010101.WAV"} {
		p := filepath.Join(game, filepath.FromSlash(f))
		os.MkdirAll(filepath.Dir(p), 0o755)
		os.WriteFile(p, []byte("x"), 0o644)
	}
	root, _ := findGameRoot(filepath.Join(dir, "Star Wars X-Wing Alliance"), 2)
	if root != game {
		t.Errorf("findGameRoot = %q", root)
	}
	cd := filepath.Join(dir, "cd")
	os.MkdirAll(filepath.Join(cd, "ALLIANCE"), 0o755)
	os.WriteFile(filepath.Join(cd, "ALLIANCE", "RESDATA.TXT"), nil, 0o644)
	if r, u := findGameRoot(cd, 2); r != "" || u != cd {
		t.Errorf("unmerged: %q %q", r, u)
	}
	// Steam library with libraryfolders.vdf pointing to a second library.
	steam := filepath.Join(dir, "steam")
	lib := filepath.Join(dir, "lib2")
	os.MkdirAll(filepath.Join(steam, "steamapps"), 0o755)
	os.MkdirAll(filepath.Join(lib, "steamapps", "common"), 0o755)
	os.Rename(filepath.Join(dir, "Star Wars X-Wing Alliance"), filepath.Join(lib, "steamapps", "common", "Star Wars X-Wing Alliance"))
	vdf := `"libraryfolders" { "1" { "path" "` + strings.ReplaceAll(lib, `\`, `\\`) + `" } }`
	os.WriteFile(filepath.Join(steam, "steamapps", "libraryfolders.vdf"), []byte(vdf), 0o644)
	dirs := steamGameDirs([]string{steam})
	if len(dirs) != 1 || !strings.HasSuffix(dirs[0], "Star Wars X-Wing Alliance") {
		t.Errorf("steamGameDirs = %q", dirs)
	}
	if got := cleanUserPath(`  "C:\Games\XWA"  `); got != `C:\Games\XWA` {
		t.Errorf("cleanUserPath = %q", got)
	}
}

func TestSafeZipPath(t *testing.T) {
	dest := t.TempDir()
	for _, bad := range []string{"../x", "a/../../x", "/etc/passwd", `C:\x`, `..\x`} {
		if _, err := safeZipPath(dest, bad); err == nil {
			t.Errorf("%q must be rejected", bad)
		}
	}
	if p, err := safeZipPath(dest, "platform-tools/adb"); err != nil || p != filepath.Join(dest, "platform-tools", "adb") {
		t.Errorf("good path: %q %v", p, err)
	}
	// A zip with a bad entry is rejected before anything is written.
	zp := filepath.Join(t.TempDir(), "bad.zip")
	f, _ := os.Create(zp)
	w := zip.NewWriter(f)
	ww, _ := w.Create("ok.txt")
	ww.Write([]byte("ok"))
	w.Create("../evil.txt")
	w.Close()
	f.Close()
	if _, err := extractZip(zp, dest); err == nil {
		t.Error("zip slip not rejected")
	}
	if _, err := os.Stat(filepath.Join(dest, "ok.txt")); err == nil {
		t.Error("files written before rejecting")
	}
}

func TestSettings(t *testing.T) {
	p := filepath.Join(t.TempDir(), "launcher.ini")
	s := loadSettings(p)
	if !s.getBool("check_updates", false) || s.getBool("include_test_versions", true) || s.get("skip_version", "x") != "" {
		t.Error("defaults")
	}
	if err := s.set("skip_version", "1.0.1"); err != nil {
		t.Fatal(err)
	}
	s2 := loadSettings(p)
	if s2.get("skip_version", "") != "1.0.1" {
		t.Error("skip_version not saved")
	}
	data, _ := os.ReadFile(p)
	if !strings.Contains(string(data), "# include_test_versions") {
		t.Error("comments lost")
	}
}

func TestFetchNewestRelease(t *testing.T) {
	releases := []ghRelease{
		{TagName: "v1.0.0", PublishedAt: "2026-10-01T10:00:00Z"},
		{TagName: "v1.0.2-rc1", Prerelease: true},
		{TagName: "v9.0.0", Draft: true},
		{TagName: "v1.0.1"},
	}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("User-Agent") == "" || r.Header.Get("Accept") != "application/vnd.github+json" {
			w.WriteHeader(400)
			return
		}
		switch r.URL.Path {
		case releasesPath + "/latest":
			json.NewEncoder(w).Encode(releases[3])
		case releasesPath:
			json.NewEncoder(w).Encode(releases)
		default:
			w.WriteHeader(404)
		}
	}))
	defer srv.Close()
	r, err := fetchNewestRelease(srv.URL, false)
	if err != nil || r.version() != "1.0.1" {
		t.Errorf("stable: %v %v", r, err)
	}
	r, err = fetchNewestRelease(srv.URL, true)
	if err != nil || r.version() != "1.0.2-rc1" {
		t.Errorf("test versions: %v %v", r, err)
	}
	if _, err := fetchNewestRelease("http://127.0.0.1:1", false); err == nil {
		t.Error("offline must fail")
	}
	if d := (&ghRelease{PublishedAt: "2026-10-01T10:00:00Z"}).releaseDate(); d != "1 Oct 2026" {
		t.Errorf("date %q", d)
	}
}
