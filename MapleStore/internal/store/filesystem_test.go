package store

import (
	"context"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestRootedFilesystemRejectsTraversalAndRootDeletion(t *testing.T) {
	filesystem, err := OpenFilesystem(t.TempDir(), "alice")
	if err != nil {
		t.Fatal(err)
	}
	defer filesystem.Close()
	for _, name := range []string{"/../bob/secret", "/folder/../../bob", `/folder\..\secret`, "/file:stream", "/.maplestore-upload-secret"} {
		if _, err := filesystem.OpenFile(context.Background(), name, os.O_RDONLY, 0); err == nil {
			t.Fatalf("accepted unsafe path %q", name)
		}
	}
	if err := filesystem.RemoveAll(context.Background(), "/"); err == nil {
		t.Fatal("allowed account root deletion")
	}
	if err := filesystem.Rename(context.Background(), "/", "/renamed"); err == nil {
		t.Fatal("allowed account root rename")
	}
}

func TestRootedFilesystemRejectsSymlinkEscape(t *testing.T) {
	directory := t.TempDir()
	filesystem, err := OpenFilesystem(directory, "alice")
	if err != nil {
		t.Fatal(err)
	}
	defer filesystem.Close()
	if err := os.Mkdir(filepath.Join(directory, "bob"), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(directory, "bob", "secret"), []byte("private"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(filepath.Join(directory, "bob"), filepath.Join(directory, "alice", "escape")); err != nil {
		t.Skipf("symlink creation unavailable: %v", err)
	}
	if _, err := filesystem.OpenFile(context.Background(), "/escape/secret", os.O_RDONLY, 0); err == nil {
		t.Fatal("read another account through symlink")
	}
	if _, err := filesystem.OpenFile(context.Background(), "/escape/secret", os.O_WRONLY|os.O_TRUNC, 0600); err == nil {
		t.Fatal("write another account through symlink")
	}
}

func TestFilesystemAccountsAreIndependent(t *testing.T) {
	directory := t.TempDir()
	alice, err := OpenFilesystem(directory, "alice")
	if err != nil {
		t.Fatal(err)
	}
	defer alice.Close()
	bob, err := OpenFilesystem(directory, "bob")
	if err != nil {
		t.Fatal(err)
	}
	defer bob.Close()
	file, err := alice.OpenFile(context.Background(), "/note.txt", os.O_CREATE|os.O_WRONLY, 0600)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := io.WriteString(file, "alice only"); err != nil {
		t.Fatal(err)
	}
	if err := file.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err := bob.Stat(context.Background(), "/note.txt"); !os.IsNotExist(err) {
		t.Fatalf("bob can see alice file: %v", err)
	}
}

func TestAtomicFileRecordsCopyErrors(t *testing.T) {
	filename := filepath.Join(t.TempDir(), "readonly.txt")
	if err := os.WriteFile(filename, []byte("original"), 0600); err != nil {
		t.Fatal(err)
	}
	readOnly, err := os.Open(filename)
	if err != nil {
		t.Fatal(err)
	}
	defer readOnly.Close()
	file := &atomicFile{File: readOnly, state: &uploadState{complete: true}}
	if _, err := file.ReadFrom(strings.NewReader("replacement")); err == nil {
		t.Fatal("expected a write failure")
	}
	if file.state.err == nil {
		t.Fatal("copy error was not recorded; failed uploads could be committed")
	}
}
