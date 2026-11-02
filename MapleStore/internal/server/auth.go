package server

import (
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"net"
	"net/http"
	"time"

	"maplestore/internal/store"
)

const cookieName = "maplestore_session"

type session struct {
	Username string
	Revision uint64
	CSRF     string
	Expires  time.Time
}
type failure struct {
	Count   int
	Expires time.Time
}

func randomToken() (string, error) {
	content := make([]byte, 32)
	if _, err := rand.Read(content); err != nil {
		return "", err
	}
	return hex.EncodeToString(content), nil
}

func tokenKey(token string) string {
	digest := sha256.Sum256([]byte(token))
	return hex.EncodeToString(digest[:])
}

func (application *Server) passwordLogin(writer http.ResponseWriter, request *http.Request, username, password string) (store.User, bool) {
	address, _, err := net.SplitHostPort(request.RemoteAddr)
	if err != nil {
		address = request.RemoteAddr
	}
	now := time.Now()
	application.mu.Lock()
	for key, value := range application.failures {
		if now.After(value.Expires) {
			delete(application.failures, key)
		}
	}
	failed := application.failures[address]
	limited := failed.Count >= 10 || (len(application.failures) >= 4096 && failed.Count == 0)
	application.mu.Unlock()
	if limited {
		writer.Header().Set("Retry-After", "60")
		writeError(writer, 429, "登录尝试过多，请稍后重试")
		return store.User{}, false
	}
	select {
	case application.authSlots <- struct{}{}:
		defer func() { <-application.authSlots }()
	default:
		writer.Header().Set("Retry-After", "2")
		writeError(writer, 429, "认证繁忙，请稍后重试")
		return store.User{}, false
	}
	user, ok := application.accounts.Authenticate(username, password)
	application.mu.Lock()
	if ok {
		delete(application.failures, address)
	} else {
		failed = application.failures[address]
		if failed.Count == 0 {
			failed.Expires = now.Add(time.Minute)
		}
		failed.Count++
		application.failures[address] = failed
	}
	application.mu.Unlock()
	if !ok {
		if request.URL.Path != "/api/login" {
			writer.Header().Set("WWW-Authenticate", `Basic realm="MapleStore", charset="UTF-8"`)
		}
		writeError(writer, 401, "账号或密码错误，或账号已停用")
	}
	return user, ok
}

func (application *Server) authorize(writer http.ResponseWriter, request *http.Request, allowBasic bool) (store.User, session, bool) {
	if allowBasic {
		if username, password, present := request.BasicAuth(); present {
			user, ok := application.passwordLogin(writer, request, username, password)
			return user, session{}, ok
		}
	}
	cookie, err := request.Cookie(cookieName)
	if err != nil || len(cookie.Value) != 64 {
		application.unauthorized(writer, allowBasic)
		return store.User{}, session{}, false
	}
	key := tokenKey(cookie.Value)
	application.mu.Lock()
	current, exists := application.sessions[key]
	if exists && time.Now().After(current.Expires) {
		delete(application.sessions, key)
		exists = false
	}
	application.mu.Unlock()
	user, userExists := application.accounts.Lookup(current.Username)
	if !exists || !userExists || user.Disabled || user.Revision != current.Revision {
		application.unauthorized(writer, allowBasic)
		return store.User{}, session{}, false
	}
	if !readMethod(request.Method) && subtle.ConstantTimeCompare([]byte(request.Header.Get("X-CSRF-Token")), []byte(current.CSRF)) != 1 {
		writeError(writer, 403, "请求校验失败，请重新登录")
		return store.User{}, session{}, false
	}
	return user, current, true
}

func (application *Server) unauthorized(writer http.ResponseWriter, basic bool) {
	if basic {
		writer.Header().Set("WWW-Authenticate", `Basic realm="MapleStore", charset="UTF-8"`)
	}
	writeError(writer, 401, "请先登录")
}

func (application *Server) issueSession(writer http.ResponseWriter, user store.User) (session, error) {
	token, err := randomToken()
	if err != nil {
		return session{}, err
	}
	csrf, err := randomToken()
	if err != nil {
		return session{}, err
	}
	now := time.Now()
	current := session{Username: user.Username, Revision: user.Revision, CSRF: csrf, Expires: now.Add(12 * time.Hour)}
	application.mu.Lock()
	defer application.mu.Unlock()
	for key, value := range application.sessions {
		if now.After(value.Expires) {
			delete(application.sessions, key)
		}
	}
	if len(application.sessions) >= 1024 {
		return session{}, http.ErrServerClosed
	}
	application.sessions[tokenKey(token)] = current
	http.SetCookie(writer, &http.Cookie{Name: cookieName, Value: token, Path: "/", HttpOnly: true, Secure: application.config.SecureCookies, SameSite: http.SameSiteStrictMode, MaxAge: 12 * 60 * 60})
	return current, nil
}
