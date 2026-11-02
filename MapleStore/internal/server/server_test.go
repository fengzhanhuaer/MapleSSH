package server

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"maplestore/internal/store"
)

const password = "example-password-123456"

func testServer(t *testing.T) (*Server, string) {
	t.Helper()
	directory := t.TempDir()
	application, err := New(Config{DataDir: filepath.Join(directory, "data"), ConfigDir: filepath.Join(directory, "config"), AdminUsername: "admin", AdminPassword: password, MaxUploadBytes: 1024, Logger: slog.New(slog.NewTextHandler(io.Discard, nil))})
	if err != nil {
		t.Fatal(err)
	}
	for _, username := range []string{"alice", "bob"} {
		if err := application.accounts.Create(username, password, false); err != nil {
			t.Fatal(err)
		}
	}
	return application, directory
}

func davRequest(application *Server, username, method, target string, body io.Reader, headers map[string]string) *httptest.ResponseRecorder {
	request := httptest.NewRequest(method, target, body)
	if username != "" {
		request.SetBasicAuth(username, password)
	}
	for name, value := range headers {
		request.Header.Set(name, value)
	}
	response := httptest.NewRecorder()
	application.ServeHTTP(response, request)
	return response
}

func requireStatus(t *testing.T, response *httptest.ResponseRecorder, wanted int) {
	t.Helper()
	if response.Code != wanted {
		t.Fatalf("status = %d, want %d: %s", response.Code, wanted, response.Body.String())
	}
}

func login(t *testing.T, application *Server, username string) (*http.Cookie, string) {
	t.Helper()
	body, _ := json.Marshal(map[string]string{"username": username, "password": password})
	response := davRequest(application, "", http.MethodPost, "/api/login", bytes.NewReader(body), nil)
	requireStatus(t, response, http.StatusOK)
	var result struct {
		CSRF string `json:"csrf"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	cookies := response.Result().Cookies()
	if len(cookies) != 1 || !cookies[0].HttpOnly || cookies[0].SameSite != http.SameSiteStrictMode {
		t.Fatal("missing secure cookie attributes")
	}
	return cookies[0], result.CSRF
}

func sessionRequest(application *Server, cookie *http.Cookie, csrf, method, target, body string) *httptest.ResponseRecorder {
	request := httptest.NewRequest(method, target, strings.NewReader(body))
	request.AddCookie(cookie)
	if csrf != "" {
		request.Header.Set("X-CSRF-Token", csrf)
	}
	response := httptest.NewRecorder()
	application.ServeHTTP(response, request)
	return response
}

func TestWebDAVFileLifecycleAndAccountIsolation(t *testing.T) {
	application, directory := testServer(t)
	requireStatus(t, davRequest(application, "", "GET", "/dav/", nil, nil), 401)
	requireStatus(t, davRequest(application, "alice", "MKCOL", "/dav/docs", nil, nil), 201)
	requireStatus(t, davRequest(application, "alice", "PUT", "/dav/docs/note.txt", strings.NewReader("private"), nil), 201)
	if data, err := os.ReadFile(filepath.Join(directory, "data", "alice", "docs", "note.txt")); err != nil || string(data) != "private" {
		t.Fatalf("wrong default layout: %q, %v", data, err)
	}
	response := davRequest(application, "alice", "GET", "/dav/docs/note.txt", nil, nil)
	requireStatus(t, response, 200)
	if response.Body.String() != "private" || !strings.Contains(response.Header().Get("Content-Disposition"), "attachment") {
		t.Fatal("download content or attachment protection missing")
	}
	requireStatus(t, davRequest(application, "bob", "GET", "/dav/docs/note.txt", nil, nil), 404)
	response = davRequest(application, "alice", "PROPFIND", "/dav/docs/", nil, map[string]string{"Depth": "1"})
	requireStatus(t, response, 207)
	if !strings.Contains(response.Body.String(), "note.txt") {
		t.Fatal("directory listing missing file")
	}
	requireStatus(t, davRequest(application, "alice", "COPY", "/dav/docs/note.txt", nil, map[string]string{"Destination": "http://example.com/dav/docs/copy.txt", "Overwrite": "F"}), 201)
	requireStatus(t, davRequest(application, "alice", "MOVE", "/dav/docs/copy.txt", nil, map[string]string{"Destination": "/dav/docs/moved.txt", "Overwrite": "F"}), 201)
	requireStatus(t, davRequest(application, "alice", "DELETE", "/dav/docs/moved.txt", nil, nil), 204)
	requireStatus(t, davRequest(application, "alice", "GET", "/dav/docs/moved.txt", nil, nil), 404)
	requireStatus(t, davRequest(application, "alice", "DELETE", "/dav/", nil, nil), 403)
	requireStatus(t, davRequest(application, "alice", "GET", "/dav/../bob/secret", nil, nil), 403)
	requireStatus(t, davRequest(application, "alice", "COPY", "/dav/docs/note.txt", nil, map[string]string{"Destination": "/dav/../bob/stolen.txt"}), 403)
	requireStatus(t, davRequest(application, "alice", "COPY", "/dav/docs/note.txt", nil, map[string]string{"Destination": "http://otherhost/dav/stolen.txt"}), 403)
}

type brokenReader struct{ sent bool }

func (reader *brokenReader) Read(content []byte) (int, error) {
	if !reader.sent {
		reader.sent = true
		return copy(content, "partial"), nil
	}
	return 0, errors.New("simulated connection failure")
}

func TestInterruptedAndOversizedUploadPreserveExistingFile(t *testing.T) {
	application, directory := testServer(t)
	requireStatus(t, davRequest(application, "alice", "PUT", "/dav/file.txt", strings.NewReader("original"), nil), 201)
	response := davRequest(application, "alice", "PUT", "/dav/file.txt", &brokenReader{}, nil)
	if response.Code < 400 {
		t.Fatal("interrupted upload succeeded")
	}
	requireStatus(t, davRequest(application, "alice", "PUT", "/dav/file.txt", strings.NewReader(strings.Repeat("x", 1025)), nil), 413)
	response = davRequest(application, "alice", "GET", "/dav/file.txt", nil, nil)
	if response.Body.String() != "original" {
		t.Fatal("failed upload destroyed old data")
	}
	entries, err := os.ReadDir(filepath.Join(directory, "data", "alice"))
	if err != nil || len(entries) != 1 {
		t.Fatalf("temporary upload file not removed: %v, %v", entries, err)
	}
}

func TestReadOnlyAndAccountRevocation(t *testing.T) {
	application, _ := testServer(t)
	cookie, csrf := login(t, application, "alice")
	readOnly := true
	if err := application.accounts.Update("alice", store.AccountPatch{ReadOnly: &readOnly}); err != nil {
		t.Fatal(err)
	}
	requireStatus(t, sessionRequest(application, cookie, csrf, "GET", "/api/me", ""), 401)
	requireStatus(t, davRequest(application, "alice", "PUT", "/dav/forbidden", strings.NewReader("x"), nil), 403)
	requireStatus(t, davRequest(application, "alice", "PROPFIND", "/dav/", nil, map[string]string{"Depth": "1"}), 207)
	disabled := true
	if err := application.accounts.Update("alice", store.AccountPatch{Disabled: &disabled}); err != nil {
		t.Fatal(err)
	}
	requireStatus(t, davRequest(application, "alice", "GET", "/dav/", nil, nil), 401)
}

func TestSessionCSRFAndAccountManagement(t *testing.T) {
	application, _ := testServer(t)
	admin, csrf := login(t, application, "admin")
	requireStatus(t, sessionRequest(application, admin, "", "POST", "/api/users", `{"username":"carol","password":"new-password-123456"}`), 403)
	requireStatus(t, sessionRequest(application, admin, csrf, "POST", "/api/users", `{"username":"carol","password":"new-password-123456","readOnly":true}`), 201)
	requireStatus(t, sessionRequest(application, admin, csrf, "PATCH", "/api/users/carol", `{"disabled":true}`), 200)
	response := sessionRequest(application, admin, csrf, "GET", "/api/users", "")
	requireStatus(t, response, 200)
	if strings.Contains(response.Body.String(), "passwordHash") {
		t.Fatal("API exposes password hashes")
	}
	alice, aliceCSRF := login(t, application, "alice")
	requireStatus(t, sessionRequest(application, alice, aliceCSRF, "GET", "/api/users", ""), 403)
	requireStatus(t, sessionRequest(application, alice, aliceCSRF, "PUT", "/dav/note", "ok"), 201)
	requireStatus(t, sessionRequest(application, alice, "", "DELETE", "/dav/note", ""), 403)
	request := httptest.NewRequest("DELETE", "/dav/note", nil)
	request.AddCookie(alice)
	request.Header.Set("Origin", "https://evil.example")
	request.Header.Set("X-CSRF-Token", aliceCSRF)
	response = httptest.NewRecorder()
	application.ServeHTTP(response, request)
	requireStatus(t, response, 403)
	requireStatus(t, sessionRequest(application, alice, aliceCSRF, "POST", "/api/logout", ""), 200)
	requireStatus(t, sessionRequest(application, alice, aliceCSRF, "GET", "/api/me", ""), 401)
}

func TestLoginRateLimiting(t *testing.T) {
	application, _ := testServer(t)
	for attempt := 0; attempt < 10; attempt++ {
		response := davRequest(application, "", "POST", "/api/login", strings.NewReader(`{"username":"alice","password":"bad"}`), nil)
		requireStatus(t, response, 401)
	}
	requireStatus(t, davRequest(application, "", "POST", "/api/login", strings.NewReader(`{"username":"alice","password":"bad"}`), nil), 429)
}

func TestPasswordResetInvalidatesSessionAndBasicCredentials(t *testing.T) {
	application, _ := testServer(t)
	cookie, csrf := login(t, application, "alice")
	admin, adminCSRF := login(t, application, "admin")
	requireStatus(t, sessionRequest(application, admin, adminCSRF, "PATCH", "/api/users/alice", `{"password":"replacement-password-123456"}`), 200)
	requireStatus(t, sessionRequest(application, cookie, csrf, "GET", "/api/me", ""), 401)
	requireStatus(t, davRequest(application, "alice", "GET", "/dav/file", nil, nil), 401)
	request := httptest.NewRequest("PROPFIND", "/dav/", nil)
	request.SetBasicAuth("alice", "replacement-password-123456")
	response := httptest.NewRecorder()
	application.ServeHTTP(response, request)
	requireStatus(t, response, 207)
}

func TestAdministratorCannotAccessOtherAccountThroughFileEndpoint(t *testing.T) {
	application, _ := testServer(t)
	requireStatus(t, davRequest(application, "alice", "PUT", "/dav/private.txt", strings.NewReader("private"), nil), 201)
	requireStatus(t, davRequest(application, "admin", "GET", "/dav/alice/private.txt", nil, nil), 404)
	requireStatus(t, davRequest(application, "admin", "GET", "/dav/../alice/private.txt", nil, nil), 403)
}

func TestStaticResourcesAndCookieSecurity(t *testing.T) {
	application, _ := testServer(t)
	application.config.SecureCookies = true
	cookie, _ := login(t, application, "admin")
	if !cookie.Secure {
		t.Fatal("HTTPS configuration did not protect cookie")
	}
	for _, target := range []string{"/", "/app.js", "/style.css", "/favicon.svg"} {
		response := davRequest(application, "", "GET", target, nil, nil)
		requireStatus(t, response, 200)
		if response.Header().Get("Content-Security-Policy") == "" {
			t.Fatal("missing CSP")
		}
	}
	requireStatus(t, davRequest(application, "", "GET", "/config/accounts.json", nil, nil), 404)
	requireStatus(t, davRequest(application, "", "GET", "/web/", nil, nil), 404)
}

func TestConfigCannotBePlacedUnderDataDirectory(t *testing.T) {
	directory := t.TempDir()
	_, err := New(Config{DataDir: directory, ConfigDir: filepath.Join(directory, "alice"), AdminUsername: "admin", AdminPassword: password})
	if err == nil {
		t.Fatal("configuration can be exposed under a user data directory")
	}
}
