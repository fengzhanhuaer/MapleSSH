package server

import (
	"bytes"
	"encoding/json"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestOnlyAdminCanRevealMemberPasswords(t *testing.T) {
	application, directory := testServer(t)
	var logs bytes.Buffer
	application.config.Logger = slog.New(slog.NewTextHandler(&logs, nil))
	admin, csrf := login(t, application, "admin")
	alice, aliceCSRF := login(t, application, "alice")
	target := "/api/users/alice/password"
	requireStatus(t, davRequest(application, "", "POST", target, nil, nil), 401)
	requireStatus(t, davRequest(application, "admin", "POST", target, nil, nil), 401)
	requireStatus(t, sessionRequest(application, alice, aliceCSRF, "POST", target, ""), 403)
	requireStatus(t, sessionRequest(application, admin, "", "POST", target, ""), 403)
	requireStatus(t, sessionRequest(application, admin, csrf, "GET", target, ""), 405)
	requireStatus(t, sessionRequest(application, admin, csrf, "POST", "/api/users/admin/password", ""), 403)
	requireStatus(t, sessionRequest(application, admin, csrf, "POST", "/api/users/missing/password", ""), 404)
	response := sessionRequest(application, admin, csrf, "POST", target, "")
	requireStatus(t, response, http.StatusOK)
	var result map[string]string
	if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil || result["password"] != password {
		t.Fatal("member password was not returned", err)
	}
	if response.Header().Get("Cache-Control") != "no-store" {
		t.Fatal("password response can be cached")
	}
	requireStatus(t, sessionRequest(application, alice, aliceCSRF, "POST", "/api/password", `{"currentPassword":"`+password+`","password":"new-member-password-123"}`), 200)
	response = sessionRequest(application, admin, csrf, "POST", target, "")
	requireStatus(t, response, 200)
	if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil || result["password"] != "new-member-password-123" {
		t.Fatal("self-service password change not reflected", err)
	}
	list := sessionRequest(application, admin, csrf, "GET", "/api/users", "")
	requireStatus(t, list, 200)
	if strings.Contains(list.Body.String(), "password") {
		t.Fatal("account list exposes passwords")
	}
	if !strings.Contains(logs.String(), "account_password_viewed") || strings.Contains(logs.String(), password) || strings.Contains(logs.String(), "new-member-password-123") {
		t.Fatal("password view audit missing or log exposes passwords")
	}
	for _, name := range []string{"accounts.json", "password.key"} {
		requireStatus(t, davRequest(application, "bob", "GET", "/dav/"+name, nil, nil), 404)
	}
	content, err := os.ReadFile(filepath.Join(directory, "config", "accounts.json"))
	if err != nil || strings.Contains(string(content), "new-member-password-123") {
		t.Fatal("new password written as plaintext", err)
	}
}
