package store

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"io"
	"os"
	"path"
	"path/filepath"
	"strings"

	"golang.org/x/net/webdav"
)

const temporaryPrefix = ".maplestore-upload-"

type Filesystem struct{ root *os.Root }

func OpenFilesystem(directory, username string) (*Filesystem, error) {
	if err := ValidateUsername(username); err != nil {
		return nil, err
	}
	if err := os.MkdirAll(directory, 0700); err != nil {
		return nil, err
	}
	parent, err := os.OpenRoot(directory)
	if err != nil {
		return nil, err
	}
	defer parent.Close()
	if err := parent.Mkdir(username, 0700); err != nil && !os.IsExist(err) {
		return nil, err
	}
	info, err := parent.Lstat(username)
	if err != nil {
		return nil, err
	}
	if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return nil, os.ErrPermission
	}
	root, err := parent.OpenRoot(username)
	if err != nil {
		return nil, err
	}
	return &Filesystem{root: root}, nil
}

func (filesystem *Filesystem) Close() error { return filesystem.root.Close() }

func SafePath(name string) (string, error) {
	if strings.ContainsAny(name, "\\:\x00") {
		return "", os.ErrPermission
	}
	for _, component := range strings.Split(name, "/") {
		if component == ".." || strings.HasPrefix(component, temporaryPrefix) {
			return "", os.ErrPermission
		}
	}
	return filepath.FromSlash(strings.TrimPrefix(path.Clean("/"+name), "/")), nil
}

func relativePath(name string) (string, error) {
	cleaned, err := SafePath(name)
	if cleaned == "" && err == nil {
		cleaned = "."
	}
	return cleaned, err
}

func (filesystem *Filesystem) Mkdir(ctx context.Context, name string, perm os.FileMode) error {
	name, err := relativePath(name)
	if err != nil {
		return err
	}
	return filesystem.root.Mkdir(name, 0700)
}

func (filesystem *Filesystem) OpenFile(ctx context.Context, name string, flag int, perm os.FileMode) (webdav.File, error) {
	name, err := relativePath(name)
	if err != nil {
		return nil, err
	}
	state, _ := ctx.Value(uploadKey{}).(*uploadState)
	if state != nil && flag&os.O_TRUNC != 0 {
		if name == "." {
			return nil, os.ErrPermission
		}
		if info, statErr := filesystem.root.Stat(name); statErr == nil && !info.Mode().IsRegular() {
			return nil, os.ErrPermission
		}
		parent, err := filesystem.root.OpenRoot(filepath.Dir(name))
		if err != nil {
			return nil, err
		}
		random := make([]byte, 16)
		if _, err := rand.Read(random); err != nil {
			parent.Close()
			return nil, err
		}
		temporary := temporaryPrefix + hex.EncodeToString(random)
		file, err := parent.OpenFile(temporary, os.O_RDWR|os.O_CREATE|os.O_EXCL, 0600)
		if err != nil {
			parent.Close()
			return nil, err
		}
		return &atomicFile{File: file, parent: parent, temporary: temporary, destination: filepath.Base(name), state: state, ctx: ctx}, nil
	}
	file, err := filesystem.root.OpenFile(name, flag, 0600)
	if err != nil {
		return nil, err
	}
	return &directoryFile{File: file}, nil
}

func (filesystem *Filesystem) RemoveAll(ctx context.Context, name string) error {
	name, err := relativePath(name)
	if err != nil {
		return err
	}
	if name == "." {
		return os.ErrPermission
	}
	return filesystem.root.RemoveAll(name)
}

func (filesystem *Filesystem) Rename(ctx context.Context, oldName, newName string) error {
	oldName, err := relativePath(oldName)
	if err != nil {
		return err
	}
	newName, err = relativePath(newName)
	if err != nil {
		return err
	}
	if oldName == "." || newName == "." {
		return os.ErrPermission
	}
	return filesystem.root.Rename(oldName, newName)
}

func (filesystem *Filesystem) Stat(ctx context.Context, name string) (os.FileInfo, error) {
	name, err := relativePath(name)
	if err != nil {
		return nil, err
	}
	return filesystem.root.Stat(name)
}

type directoryFile struct{ *os.File }

func (file *directoryFile) Readdir(count int) ([]os.FileInfo, error) {
	if count <= 0 {
		entries, err := file.File.Readdir(count)
		visible := entries[:0]
		for _, entry := range entries {
			if !strings.HasPrefix(entry.Name(), temporaryPrefix) {
				visible = append(visible, entry)
			}
		}
		return visible, err
	}
	entries := make([]os.FileInfo, 0, count)
	for len(entries) < count {
		batch, err := file.File.Readdir(count - len(entries))
		for _, entry := range batch {
			if !strings.HasPrefix(entry.Name(), temporaryPrefix) {
				entries = append(entries, entry)
			}
		}
		if err != nil {
			return entries, err
		}
	}
	return entries, nil
}

type uploadKey struct{}
type uploadState struct {
	complete bool
	err      error
}
type trackedBody struct {
	io.ReadCloser
	state *uploadState
}

func TrackUpload(ctx context.Context, body io.ReadCloser) (context.Context, io.ReadCloser) {
	state := &uploadState{}
	return context.WithValue(ctx, uploadKey{}, state), &trackedBody{ReadCloser: body, state: state}
}

func (body *trackedBody) Read(buffer []byte) (int, error) {
	count, err := body.ReadCloser.Read(buffer)
	if errors.Is(err, io.EOF) {
		body.state.complete = true
	} else if err != nil {
		body.state.err = err
	}
	return count, err
}

type atomicFile struct {
	*os.File
	parent      *os.Root
	temporary   string
	destination string
	state       *uploadState
	ctx         context.Context
	closed      bool
	result      error
}

func (file *atomicFile) Write(content []byte) (int, error) {
	count, err := file.File.Write(content)
	if err != nil {
		file.state.err = err
	}
	return count, err
}

func (file *atomicFile) ReadFrom(reader io.Reader) (int64, error) {
	count, err := io.Copy(struct{ io.Writer }{file}, reader)
	if err != nil {
		file.state.err = err
	}
	return count, err
}

func (file *atomicFile) Close() error {
	if file.closed {
		return file.result
	}
	file.closed = true
	defer file.parent.Close()
	defer file.parent.Remove(file.temporary)
	file.result = file.state.err
	if file.result == nil {
		file.result = file.ctx.Err()
	}
	if file.result == nil && !file.state.complete {
		file.result = io.ErrUnexpectedEOF
	}
	if file.result == nil {
		file.result = file.File.Sync()
	}
	closeErr := file.File.Close()
	if file.result == nil {
		file.result = closeErr
	}
	if file.result == nil {
		file.result = file.parent.Rename(file.temporary, file.destination)
	}
	return file.result
}
