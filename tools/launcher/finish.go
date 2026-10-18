package main

import (
	"os"
	"path/filepath"
	"strings"
	"time"
)

// doneMenu is step 8. Its Enter choice doubles as the final "Press Enter to
// close", so main does not ask again.
func (a *app) doneMenu() {
	say("All done. Put on the headset and open OpenXWA (Library -> Unknown sources).")
	for {
		say("")
		say("  [Enter]  Close")
		say("  [s]      Start OpenXWA on the headset now")
		say("  [l]      Collect logs (for bug reports)")
		ans, ok := a.ui.ask("Press Enter to close, or type s or l:")
		switch strings.ToLower(ans) {
		case "s":
			out, err := a.adb.shell(30*time.Second, "am start -n "+appActivity)
			if err != nil || strings.Contains(out, "Error") {
				say("Could not start OpenXWA: %s", strings.TrimSpace(out))
			} else {
				say("OpenXWA is starting on the headset. Put it on!")
			}
		case "l":
			a.collectLogs()
		default:
			if ok && ans != "" {
				say("Please press Enter, s or l.")
				continue
			}
			a.skipPause = true
			return
		}
		if !ok {
			a.skipPause = true
			return
		}
	}
}

// collectLogs pulls the app's logs and its settings file (openxwa-quest.cfg:
// the VR options and the hand-placed HUD, console and stick positions) from
// the headset into
// <exe dir>/logs-<date-time>/ together with the launcher's own log.
func (a *app) collectLogs() {
	dir := filepath.Join(a.exeDir, "logs-"+time.Now().Format("20060102-150405"))
	if err := os.MkdirAll(dir, 0o755); err != nil {
		dir = filepath.Join(os.TempDir(), filepath.Base(dir))
		if err := os.MkdirAll(dir, 0o755); err != nil {
			say("Could not create a folder for the logs: %v", err)
			return
		}
	}
	got := 0
	for _, name := range []string{"openxwa.log", "openxwa-prev.log", "openxwa-quest.cfg"} {
		if _, err := a.adb.run(2*time.Minute, "pull", remoteBase+"/"+name, filepath.Join(dir, name)); err == nil && name != "openxwa-quest.cfg" {
			got++
		}
	}
	if lp := logPath(); lp != "" {
		if data, err := os.ReadFile(lp); err == nil {
			_ = os.WriteFile(filepath.Join(dir, logFileName), data, 0o644)
		}
	}
	if got == 0 {
		say("OpenXWA has not written a log on the headset yet (start it once first).")
	}
	say("Logs saved in: %s", dir)
	say("Please attach the files in that folder to your bug report.")
}
