package store

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestMemberPasswordsAreEncryptedAndPersisted(t *testing.T) {
	directory := t.TempDir()
	accounts, err := OpenAccounts(directory, "admin", testPassword)
	if err != nil {
		t.Fatal(err)
	}
	if err := accounts.Create("alice", testPassword, false); err != nil {
		t.Fatal(err)
	}
	if err := accounts.Create("bob", testPassword, false); err != nil {
		t.Fatal(err)
	}
	if accounts.users["alice"].PasswordEncrypted == "" || accounts.users["alice"].PasswordEncrypted == accounts.users["bob"].PasswordEncrypted {
		t.Fatal("missing or reused password ciphertext")
	}
	content, err := os.ReadFile(filepath.Join(directory, "accounts.json"))
	if err != nil || strings.Contains(string(content), testPassword) {
		t.Fatal("password stored in plaintext", err)
	}
	key, err := os.ReadFile(filepath.Join(directory, "password.key"))
	if err != nil || len(key) != 32 {
		t.Fatal("missing AES-256 key", err)
	}
	if runtime.GOOS != "windows" {
		for _, name := range []string{"accounts.json", "password.key"} {
			info, err := os.Stat(filepath.Join(directory, name))
			if err != nil || info.Mode().Perm() != 0600 {
				t.Fatal("password material is not owner-only", err)
			}
		}
	}
	accounts, err = OpenAccounts(directory, "admin", "replacement-admin-password")
	if err != nil {
		t.Fatal(err)
	}
	actual, err := accounts.RevealPassword("alice")
	if err != nil || actual != testPassword {
		t.Fatalf("persisted member password = %q, %v", actual, err)
	}
	if _, err := accounts.RevealPassword("admin"); err == nil {
		t.Fatal("administrator password can be revealed")
	}
	if _, err := accounts.RevealPassword("missing"); !errors.Is(err, os.ErrNotExist) {
		t.Fatal("missing account not reported", err)
	}
	if err := accounts.Update("alice", AccountPatch{Password: "replacement-member-password"}); err != nil {
		t.Fatal(err)
	}
	readOnly := true
	if err := accounts.Update("alice", AccountPatch{ReadOnly: &readOnly}); err != nil {
		t.Fatal(err)
	}
	accounts, err = OpenAccounts(directory, "admin", "replacement-admin-password")
	if err != nil {
		t.Fatal(err)
	}
	actual, err = accounts.RevealPassword("alice")
	if err != nil || actual != "replacement-member-password" {
		t.Fatal("password update not persisted", err)
	}
	if _, ok := accounts.Authenticate("alice", actual); !ok {
		t.Fatal("encrypted password and authentication hash disagree")
	}
}

func TestPasswordKeyAndCiphertextIntegrity(t *testing.T) {
	for _, damage := range []string{"missing-key", "invalid-key", "wrong-key", "swapped-password", "truncated-password", "missing-password"} {
		t.Run(damage, func(t *testing.T) {
			directory := t.TempDir()
			accounts, err := OpenAccounts(directory, "admin", testPassword)
			if err != nil {
				t.Fatal(err)
			}
			for _, username := range []string{"alice", "bob"} {
				if err := accounts.Create(username, testPassword, false); err != nil {
					t.Fatal(err)
				}
			}
			keyPath := filepath.Join(directory, "password.key")
			switch damage {
			case "missing-key":
				err = os.Remove(keyPath)
			case "invalid-key":
				err = os.WriteFile(keyPath, []byte("bad-key"), 0600)
			case "wrong-key":
				err = os.WriteFile(keyPath, make([]byte, 32), 0600)
			default:
				record := accounts.users["alice"]
				record.PasswordEncrypted = accounts.users["bob"].PasswordEncrypted
				if damage == "truncated-password" {
					record.PasswordEncrypted = "AA=="
				}
				if damage == "missing-password" {
					record.PasswordEncrypted = ""
				}
				accounts.users["alice"] = record
				err = accounts.save(accounts.users)
			}
			if err != nil {
				t.Fatal(err)
			}
			before, err := os.ReadFile(filepath.Join(directory, "accounts.json"))
			if err != nil {
				t.Fatal(err)
			}
			if _, err := OpenAccounts(directory, "admin", testPassword); err == nil {
				t.Fatal("damaged key or ciphertext accepted")
			}
			after, err := os.ReadFile(filepath.Join(directory, "accounts.json"))
			if err != nil || string(before) != string(after) {
				t.Fatal("failed startup modified account data", err)
			}
			if damage == "missing-key" {
				if _, err := os.Stat(keyPath); !os.IsNotExist(err) {
					t.Fatal("missing key silently replaced")
				}
			}
		})
	}
}

func TestAccountListDoesNotExposePasswordMaterial(t *testing.T) {
	accounts, err := OpenAccounts(t.TempDir(), "admin", testPassword)
	if err != nil {
		t.Fatal(err)
	}
	if err := accounts.Create("alice", testPassword, false); err != nil {
		t.Fatal(err)
	}
	content, err := json.Marshal(accounts.List())
	if err != nil || strings.Contains(string(content), "password") {
		t.Fatal("account list exposes password material", err)
	}
}

func TestFailedPasswordSavePreservesHashAndCiphertext(t *testing.T) {
	directory := t.TempDir()
	accounts, err := OpenAccounts(directory, "admin", testPassword)
	if err != nil {
		t.Fatal(err)
	}
	if err := accounts.Create("alice", testPassword, false); err != nil {
		t.Fatal(err)
	}
	before := accounts.users["alice"]
	path := filepath.Join(directory, "accounts.json")
	if err := os.Rename(path, path+".saved"); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(path, 0700); err != nil {
		t.Fatal(err)
	}
	if err := accounts.Update("alice", AccountPatch{Password: "must-not-be-committed"}); err == nil {
		t.Fatal("save unexpectedly succeeded")
	}
	if accounts.users["alice"] != before {
		t.Fatal("failed save changed hash, ciphertext or user revision")
	}
	if actual, err := accounts.RevealPassword("alice"); err != nil || actual != testPassword {
		t.Fatal("failed save changed visible password", err)
	}
}
