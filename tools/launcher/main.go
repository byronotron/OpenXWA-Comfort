// OpenXWA Launcher: puts the OpenXWA VR app and the user's own X-Wing
// Alliance game files on a standalone VR headset over USB, using adb.
//
// The design is recorded in PLAN.md. Standard library only.
package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// version is set at build time: -ldflags "-X main.version=1.0.0".
var version = "dev"

const (
	appPackage  = "org.openxwa.quest"
	appActivity = "org.openxwa.quest/org.openxwa.quest.OpenXwaActivity"
	// remoteBase is the app's own folder on the headset.
	remoteBase     = "/sdcard/Android/data/org.openxwa.quest/files"
	remoteGame     = remoteBase + "/XWA"
	remotePilots   = remoteBase + "/pilot-import"
	manifestName   = "openxwa-install-files.txt"
	manifestHeader = "# OpenXWA install file list v1"
)

type options struct {
	help        bool
	showVersion bool
	logsOnly    bool
	noUpdate    bool
	gameArg     string
}

// userError carries a message meant for the user (already friendly).
type userError struct{ msg string }

func (e *userError) Error() string { return e.msg }

func fail(format string, args ...any) error {
	return &userError{msg: fmt.Sprintf(format, args...)}
}

func usage() string {
	return `OpenXWA Launcher ` + version + `

Puts OpenXWA and your own X-Wing Alliance game files on your VR headset
(Meta Quest, Pico, Steam Frame) over a USB cable.

Usage:
  OpenXWA-Launcher [options] [game folder]

You can also drag your X-Wing Alliance folder onto OpenXWA-Launcher.exe.

Options:
  --no-update-check   do not look for a new launcher version
  --logs              only collect the OpenXWA logs from the headset
  --version           show the launcher version
  --help              show this help
`
}

func parseArgs(args []string) (options, error) {
	var o options
	for _, a := range args {
		switch strings.ToLower(a) {
		case "--help", "-help", "-h", "/?", "/h":
			o.help = true
		case "--version", "-version", "-v":
			o.showVersion = true
		case "--logs", "-logs":
			o.logsOnly = true
		case "--no-update-check", "-no-update-check":
			o.noUpdate = true
		default:
			if strings.HasPrefix(a, "-") {
				if _, err := os.Stat(a); err != nil {
					return o, fmt.Errorf("unknown option %q", a)
				}
			}
			if o.gameArg == "" {
				o.gameArg = a
			}
		}
	}
	return o, nil
}

func main() {
	opts, err := parseArgs(os.Args[1:])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		fmt.Fprint(os.Stderr, "\n"+usage())
		os.Exit(2)
	}
	if opts.help {
		fmt.Print(usage())
		return
	}
	if opts.showVersion {
		fmt.Println("OpenXWA Launcher " + version)
		return
	}

	a := newApp()
	logPath := openLog(a.exeDir)
	logf("OpenXWA Launcher %s starting; exe dir %s; args %q", version, a.exeDir, os.Args[1:])
	code := a.run(opts)
	logf("exit code %d", code)
	if !a.skipPause {
		if code != 0 && logPath != "" {
			fmt.Printf("\nDetails are in the log file: %s\n", logPath)
		}
		fmt.Print("\nPress Enter to close.")
		a.ui.readLine()
	}
	closeLog()
	os.Exit(code)
}

func exeDirectory() string {
	exe, err := os.Executable()
	if err != nil {
		wd, _ := os.Getwd()
		return wd
	}
	if r, err := filepath.EvalSymlinks(exe); err == nil {
		exe = r
	}
	return filepath.Dir(exe)
}

// app holds the state of one launcher run.
type app struct {
	exeDir     string
	ui         *UI
	adb        *ADB
	totalSteps int
	skipPause  bool // set when the closing prompt was already shown (or a new launcher took over)

	apkPath    string
	apkVersion string
	gameDir    string
}

func newApp() *app {
	return &app{exeDir: exeDirectory(), ui: newUI(os.Stdin), totalSteps: 8}
}

func (a *app) step(n int, title string) {
	say("")
	say("Step %d of %d: %s", n, a.totalSteps, title)
}

// run executes the whole flow and returns the process exit code.
func (a *app) run(opts options) int {
	say("OpenXWA Launcher %s", version)
	say("Puts OpenXWA and your own X-Wing Alliance files on your VR headset.")

	err := a.flow(opts)
	if err != nil {
		var ue *userError
		say("")
		if errors.As(err, &ue) {
			say("%s", ue.msg)
		} else {
			say("Something went wrong: %v", err)
		}
		logf("stopped with error: %v", err)
		return 1
	}
	return 0
}

func (a *app) flow(opts options) error {
	if opts.logsOnly {
		a.totalSteps = 3
		a.step(1, "Finding adb")
		if err := a.setupADB(); err != nil {
			return err
		}
		a.step(2, "Connecting to the headset")
		if err := a.connect(); err != nil {
			return err
		}
		a.step(3, "Collecting logs")
		a.collectLogs()
		return nil
	}

	if !opts.noUpdate {
		if a.checkForUpdates() {
			return nil
		}
	}

	if err := a.findAPK(); err != nil {
		return err
	}

	a.step(1, "Finding adb")
	if err := a.setupADB(); err != nil {
		return err
	}
	a.step(2, "Finding your X-Wing Alliance folder")
	if err := a.chooseGame(opts.gameArg); err != nil {
		return err
	}
	a.step(3, "Connecting to the headset")
	if err := a.connect(); err != nil {
		return err
	}
	a.step(4, "Installing the OpenXWA app")
	if err := a.installApp(); err != nil {
		return err
	}
	a.step(5, "Preparing the OpenXWA folder on the headset")
	if err := a.prepareAppFolder(); err != nil {
		return err
	}
	a.step(6, "Pilots")
	if err := a.copyPilots(); err != nil {
		return err
	}
	a.step(7, "Copying the game files")
	if err := a.copyGame(); err != nil {
		return err
	}
	a.step(8, "Done")
	a.doneMenu()
	return nil
}
