package store

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const testPassword = "test-password-123456"

func TestAccountsPersistAndAuthenticate(t *testing.T) {
	directory := t.TempDir()
	accounts, err := OpenAccounts(directory, "admin", testPassword)
	if err != nil {
		t.Fatal(err)
	}
	if err := accounts.Create("alice", testPassword, false); err != nil {
		t.Fatal(err)
	}
	if _, ok := accounts.Authenticate("alice", "incorrect"); ok {
		t.Fatal("accepted incorrect password")
	}
	if _, ok := accounts.Authenticate("missing", testPassword); ok {
		t.Fatal("accepted unknown user")
	}
	data, err := os.ReadFile(filepath.Join(directory, "accounts.json"))
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(data), testPassword) {
		t.Fatal("stored plaintext password")
	}
	if !strings.Contains(string(data), "$argon2id$") {
		t.Fatal("missing Argon2id hash")
	}
	reloaded, err := OpenAccounts(directory, "admin", testPassword)
	if err != nil {
		t.Fatal(err)
	}
	if user, ok := reloaded.Authenticate("alice", testPassword); !ok || user.Admin {
		t.Fatal("user was not preserved")
	}
}

func TestAdminCredentialsAreControlledByEnvironment(t *testing.T) {
	directory := t.TempDir()
	accounts, err := OpenAccounts(directory, "admin", testPassword)
	if err != nil {
		t.Fatal(err)
	}
	if err := accounts.Update("admin", AccountPatch{Password: "new-password-123456"}); err == nil {
		t.Fatal("web API can change configured administrator")
	}
	accounts, err = OpenAccounts(directory, "admin", "new-password-123456")
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := accounts.Authenticate("admin", testPassword); ok {
		t.Fatal("old admin password remains valid")
	}
	if _, ok := accounts.Authenticate("admin", "new-password-123456"); !ok {
		t.Fatal("new admin password not applied")
	}
	if _, err := OpenAccounts(directory, "otheradmin", testPassword); err == nil {
		t.Fatal("silently changed admin identity")
	}
	if _, err := OpenAccounts(t.TempDir(), "admin", ""); err == nil {
		t.Fatal("accepted empty administrator password")
	}
}

func TestAccountUpdatesRevokeCredentialsAndValidateNames(t *testing.T) {
	accounts, err := OpenAccounts(t.TempDir(), "admin", testPassword)
	if err != nil {
		t.Fatal(err)
	}
	for _, username := range []string{"../bob", "Alice", "a/b", "admin", "con", "a:b"} {
		if err := accounts.Create(username, testPassword, false); err == nil {
			t.Fatalf("accepted invalid or duplicate username %q", username)
		}
	}
	if err := accounts.Create("alice", testPassword, false); err != nil {
		t.Fatal(err)
	}
	before, _ := accounts.Lookup("alice")
	disabled := true
	if err := accounts.Update("alice", AccountPatch{Disabled: &disabled}); err != nil {
		t.Fatal(err)
	}
	after, _ := accounts.Lookup("alice")
	if after.Revision <= before.Revision {
		t.Fatal("account revision did not change")
	}
	if _, ok := accounts.Authenticate("alice", testPassword); ok {
		t.Fatal("disabled user can log in")
	}
	disabled = false
	if err := accounts.Update("alice", AccountPatch{Disabled: &disabled, Password: "replacement-password-123"}); err != nil {
		t.Fatal(err)
	}
	if _, ok := accounts.Authenticate("alice", testPassword); ok {
		t.Fatal("old password accepted")
	}
	if _, ok := accounts.Authenticate("alice", "replacement-password-123"); !ok {
		t.Fatal("new password rejected")
	}
}
