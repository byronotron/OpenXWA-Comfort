package main

import (
	"archive/zip"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

var errNotWritable = errors.New("folder is not writable")

func userAgent() string { return "OpenXWA-Launcher/" + version }

// downloadClient has no overall time limit (downloads can be slow) but gives
// up when the server does not answer at all. Proxies come from the
// environment (the default transport's ProxyFromEnvironment).
var downloadClient = func() *http.Client {
	t := http.DefaultTransport.(*http.Transport).Clone()
	t.ResponseHeaderTimeout = 30 * time.Second
	return &http.Client{Transport: t}
}()

// downloadFile saves url to dest, drawing "label: 3.2 MB of 7.0 MB (45%)".
// It writes to dest+".part" first and renames when complete.
func downloadFile(url, dest, label string) error {
	logf("download %s -> %s", url, dest)
	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return err
	}
	req.Header.Set("User-Agent", userAgent())
	resp, err := downloadClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("the server answered %s", resp.Status)
	}
	part := dest + ".part"
	f, err := os.Create(part)
	if err != nil {
		return fmt.Errorf("%w: %v", errNotWritable, err)
	}
	var p progressLine
	total := resp.ContentLength
	var got int64
	buf := make([]byte, 64<<10)
	draw := func(force bool) {
		if total > 0 {
			p.show(fmt.Sprintf("%s: %s of %s (%d%%)", label, humanSize(got), humanSize(total), got*100/total), force)
		} else {
			p.show(fmt.Sprintf("%s: %s", label, humanSize(got)), force)
		}
	}
	for {
		n, rerr := resp.Body.Read(buf)
		if n > 0 {
			if _, werr := f.Write(buf[:n]); werr != nil {
				f.Close()
				os.Remove(part)
				fmt.Println()
				return werr
			}
			got += int64(n)
			draw(false)
		}
		if rerr == io.EOF {
			break
		}
		if rerr != nil {
			f.Close()
			os.Remove(part)
			fmt.Println()
			return rerr
		}
	}
	draw(true)
	fmt.Println()
	if err := f.Close(); err != nil {
		os.Remove(part)
		return err
	}
	if total > 0 && got != total {
		os.Remove(part)
		return fmt.Errorf("the download stopped early (%d of %d bytes)", got, total)
	}
	os.Remove(dest)
	if err := os.Rename(part, dest); err != nil {
		os.Remove(part)
		return err
	}
	logf("downloaded %d bytes", got)
	return nil
}

// safeZipPath returns where a zip entry goes inside destDir, or an error for
// entries that would land outside it ("zip slip": ../, absolute paths, drive
// letters).
func safeZipPath(destDir, name string) (string, error) {
	n := strings.ReplaceAll(name, "\\", "/")
	if n == "" || strings.HasPrefix(n, "/") || strings.Contains(n, ":") {
		return "", fmt.Errorf("unsafe path in zip: %q", name)
	}
	for _, part := range strings.Split(n, "/") {
		if part == ".." {
			return "", fmt.Errorf("unsafe path in zip: %q", name)
		}
	}
	clean := filepath.Clean(destDir)
	p := filepath.Join(clean, filepath.FromSlash(n))
	if p != clean && !strings.HasPrefix(p, clean+string(filepath.Separator)) {
		return "", fmt.Errorf("unsafe path in zip: %q", name)
	}
	return p, nil
}

// extractZip unpacks zipPath into destDir and returns the files written.
// Executable bits are kept on Unix (adb must stay runnable).
func extractZip(zipPath, destDir string) ([]string, error) {
	r, err := zip.OpenReader(zipPath)
	if err != nil {
		return nil, fmt.Errorf("could not open the zip: %w", err)
	}
	defer r.Close()
	// Check every name before writing anything.
	for _, f := range r.File {
		if _, err := safeZipPath(destDir, f.Name); err != nil {
			return nil, err
		}
	}
	var written []string
	for _, f := range r.File {
		p, _ := safeZipPath(destDir, f.Name)
		if f.FileInfo().IsDir() || strings.HasSuffix(f.Name, "/") {
			if err := os.MkdirAll(p, 0o755); err != nil {
				return written, err
			}
			continue
		}
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			return written, err
		}
		mode := os.FileMode(0o644)
		if runtime.GOOS != "windows" && f.Mode()&0o111 != 0 {
			mode = 0o755
		}
		if err := writeZipEntry(f, p, mode); err != nil {
			return written, err
		}
		written = append(written, p)
	}
	logf("extracted %d files from %s into %s", len(written), zipPath, destDir)
	return written, nil
}

func writeZipEntry(f *zip.File, dest string, mode os.FileMode) error {
	rc, err := f.Open()
	if err != nil {
		return err
	}
	defer rc.Close()
	out, err := os.OpenFile(dest, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, mode)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, rc); err != nil {
		out.Close()
		return err
	}
	if err := out.Close(); err != nil {
		return err
	}
	return os.Chmod(dest, mode)
}
