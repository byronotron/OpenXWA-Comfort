//go:build !windows

package main

import (
	"errors"
	"io/fs"
)

func isHiddenSystemFile(fs.FileInfo) bool { return false }

func windowsSteamRoots() []string       { return nil }
func windowsRegistryGameDirs() []string { return nil }

// startLauncher is only supported on Windows (the release contains a Windows exe).
func startLauncher(string) error { return errors.New("not supported on this system") }
