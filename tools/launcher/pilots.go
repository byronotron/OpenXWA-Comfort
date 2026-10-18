package main

import (
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// pilotName groups "Luke0.plt" and "Luke1.plt" as the pilot "Luke".
func pilotName(file string) string {
	base := strings.TrimSuffix(file, filepath.Ext(file))
	if len(base) > 1 && base[len(base)-1] >= '0' && base[len(base)-1] <= '9' {
		base = base[:len(base)-1]
	}
	return base
}

// pilotFiles lists the *.plt files at the top level of the game folder.
func pilotFiles(dir string) []gameFile {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	var out []gameFile
	for _, e := range entries {
		if !e.Type().IsRegular() || !strings.EqualFold(filepath.Ext(e.Name()), ".plt") {
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		out = append(out, gameFile{Rel: e.Name(), Abs: filepath.Join(dir, e.Name()), Size: info.Size()})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Rel < out[j].Rel })
	return out
}

func pilotNames(files []gameFile) []string {
	seen := map[string]bool{}
	var names []string
	for _, f := range files {
		n := pilotName(f.Rel)
		if !seen[strings.ToLower(n)] {
			seen[strings.ToLower(n)] = true
			names = append(names, n)
		}
	}
	return names
}

// copyPilots is step 6: optionally bring the PC pilots over. The app
// imports *.plt from pilot-import when it starts.
func (a *app) copyPilots() error {
	files := pilotFiles(a.gameDir)
	if len(files) == 0 {
		say("No pilot files in the game folder. Nothing to do.")
		return nil
	}
	say("Pilots found in your game folder: %s", strings.Join(pilotNames(files), ", "))
	if !a.ui.yesNo("Copy these pilots to the headset? (y/N)", false) {
		say("Pilots are not copied.")
		return nil
	}
	if out, err := a.adb.shell(30*time.Second, "mkdir -p "+shQuote(remotePilots)); err != nil {
		return fail("Could not create the pilot folder on the headset:\n  %s", strings.TrimSpace(out))
	}
	for try := 1; try <= 3; try++ {
		var jobs []pushJob
		remote, err := a.listRemote(remotePilots)
		if err != nil {
			remote = map[string]int64{}
		}
		bad := filesToCopy(files, remote)
		if len(bad) == 0 {
			say("Pilots copied. OpenXWA brings them in the next time it starts.")
			return nil
		}
		for _, f := range bad {
			j := singleFileJob(remotePilots, f)
			j.label = ""
			jobs = append(jobs, j)
		}
		if err := a.runPushes("Copying pilots", jobs, bad); err != nil {
			return err
		}
	}
	remote, _ := a.listRemote(remotePilots)
	if bad := filesToCopy(files, remote); len(bad) > 0 {
		var names []string
		for _, f := range bad {
			names = append(names, f.Rel)
		}
		say("WARNING: these pilot files could not be copied: %s", strings.Join(names, ", "))
		say("The game files are copied anyway; run the launcher again later to retry.")
		return nil
	}
	say("Pilots copied. OpenXWA brings them in the next time it starts.")
	return nil
}
