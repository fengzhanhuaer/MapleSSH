package server

import (
	"encoding/json"
	"io"
	"net/http"
	"strings"

	"maplestore/internal/store"
)

func writeJSON(writer http.ResponseWriter, status int, value any) {
	writer.Header().Set("Content-Type", "application/json; charset=utf-8")
	writer.WriteHeader(status)
	json.NewEncoder(writer).Encode(value)
}

func writeError(writer http.ResponseWriter, status int, message string) {
	writeJSON(writer, status, map[string]string{"error": message})
}

func decodeJSON(writer http.ResponseWriter, request *http.Request, value any) bool {
	decoder := json.NewDecoder(http.MaxBytesReader(writer, request.Body, 8192))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(value); err != nil {
		writeError(writer, 400, "请求格式无效或包含不支持的字段")
		return false
	}
	if err := decoder.Decode(new(any)); err != io.EOF {
		writeError(writer, 400, "请求只能包含一个 JSON 对象")
		return false
	}
	return true
}

func (application *Server) serveAPI(writer http.ResponseWriter, request *http.Request) {
	if request.URL.Path == "/api/login" {
		if request.Method != "POST" {
			writer.WriteHeader(405)
			return
		}
		var credentials struct {
			Username string `json:"username"`
			Password string `json:"password"`
		}
		if !decodeJSON(writer, request, &credentials) {
			return
		}
		user, ok := application.passwordLogin(writer, request, credentials.Username, credentials.Password)
		if !ok {
			return
		}
		current, err := application.issueSession(writer, user)
		if err != nil {
			writeError(writer, 503, "无法创建登录会话")
			return
		}
		writeJSON(writer, 200, map[string]any{"user": user, "csrf": current.CSRF})
		return
	}
	user, current, ok := application.authorize(writer, request, false)
	if !ok {
		return
	}
	switch request.URL.Path {
	case "/api/me":
		if request.Method != "GET" {
			writer.WriteHeader(405)
			return
		}
		writeJSON(writer, 200, map[string]any{"user": user, "csrf": current.CSRF})
		return
	case "/api/logout":
		if request.Method != "POST" {
			writer.WriteHeader(405)
			return
		}
		cookie, _ := request.Cookie(cookieName)
		application.mu.Lock()
		delete(application.sessions, tokenKey(cookie.Value))
		application.mu.Unlock()
		http.SetCookie(writer, &http.Cookie{Name: cookieName, Path: "/", HttpOnly: true, Secure: application.config.SecureCookies, SameSite: http.SameSiteStrictMode, MaxAge: -1})
		writeJSON(writer, 200, map[string]bool{"ok": true})
		return
	case "/api/password":
		if request.Method != "POST" {
			writer.WriteHeader(405)
			return
		}
		var credentials struct {
			CurrentPassword string `json:"currentPassword"`
			Password        string `json:"password"`
		}
		if !decodeJSON(writer, request, &credentials) {
			return
		}
		if _, ok := application.passwordLogin(writer, request, user.Username, credentials.CurrentPassword); !ok {
			return
		}
		if err := application.accounts.Update(user.Username, store.AccountPatch{Password: credentials.Password}); err != nil {
			writeError(writer, 400, err.Error())
			return
		}
		writeJSON(writer, 200, map[string]bool{"ok": true})
		return
	}
	if !user.Admin {
		writeError(writer, 403, "需要管理员权限")
		return
	}
	if request.URL.Path == "/api/users" {
		switch request.Method {
		case "GET":
			writeJSON(writer, 200, application.accounts.List())
		case "POST":
			var account struct {
				Username string `json:"username"`
				Password string `json:"password"`
				ReadOnly bool   `json:"readOnly"`
			}
			if !decodeJSON(writer, request, &account) {
				return
			}
			if err := store.ValidateUsername(account.Username); err != nil {
				writeError(writer, 400, err.Error())
				return
			}
			root, err := store.OpenFilesystem(application.config.DataDir, account.Username)
			if err != nil {
				writeError(writer, 500, "无法创建账号目录")
				return
			}
			root.Close()
			if err := application.accounts.Create(account.Username, account.Password, account.ReadOnly); err != nil {
				writeError(writer, 400, err.Error())
				return
			}
			application.config.Logger.Info("account_created", "actor", user.Username, "username", account.Username)
			writeJSON(writer, 201, map[string]bool{"ok": true})
		default:
			writer.WriteHeader(405)
		}
		return
	}
	if strings.HasPrefix(request.URL.Path, "/api/users/") {
		if request.Method != "PATCH" {
			writer.WriteHeader(405)
			return
		}
		username := strings.TrimPrefix(request.URL.Path, "/api/users/")
		var patch store.AccountPatch
		if !decodeJSON(writer, request, &patch) {
			return
		}
		if err := application.accounts.Update(username, patch); err != nil {
			writeError(writer, 400, err.Error())
			return
		}
		application.config.Logger.Info("account_updated", "actor", user.Username, "username", username)
		writeJSON(writer, 200, map[string]bool{"ok": true})
		return
	}
	http.NotFound(writer, request)
}
