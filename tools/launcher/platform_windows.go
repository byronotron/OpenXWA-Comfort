//go:build windows

package main

import (
	"io/fs"
	"os/exec"
	"path/filepath"
	"syscall"
)

// isHiddenSystemFile reports Windows files marked both hidden and system
// (desktop.ini, Thumbs.db and the like).
func isHiddenSystemFile(info fs.FileInfo) bool {
	d, ok := info.Sys().(*syscall.Win32FileAttributeData)
	if !ok {
		return false
	}
	const both = syscall.FILE_ATTRIBUTE_HIDDEN | syscall.FILE_ATTRIBUTE_SYSTEM
	return d.FileAttributes&both == both
}

// startLauncher opens exe in a new console window (cmd /c start) with its own
// folder as working directory. The current launcher then exits.
func startLauncher(exe string) error {
	dir := filepath.Dir(exe)
	cmd := exec.Command("cmd.exe")
	cmd.Dir = dir
	// Set the command line by hand: cmd.exe has its own quoting rules and
	// start's first quoted argument is the window title.
	cmd.SysProcAttr = &syscall.SysProcAttr{
		CmdLine: `cmd.exe /c start "" /D "` + dir + `" "` + exe + `"`,
	}
	return cmd.Run()
}
