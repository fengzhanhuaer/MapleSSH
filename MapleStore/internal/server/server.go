package server

import (
	"embed"
	"io/fs"
	"log/slog"
	"mime"
	"net/http"
	"net/url"
	"os"
	"path"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"golang.org/x/net/webdav"
	"maplestore/internal/store"
)

//go:embed web/*
var webFiles embed.FS

type Config struct {
	DataDir        string
	ConfigDir      string
	AdminUsername  string
	AdminPassword  string
	SecureCookies  bool
	MaxUploadBytes int64
	Logger         *slog.Logger
}

type Server struct {
	config    Config
	accounts  *store.Accounts
	static    http.Handler
	mu        sync.Mutex
	sessions  map[string]session
	failures  map[string]failure
	locks     map[string]webdav.LockSystem
	authSlots chan struct{}
}

func New(config Config) (*Server, error) {
	if config.DataDir == "" {
		config.DataDir = "./data"
	}
	if config.ConfigDir == "" {
		config.ConfigDir = "./config"
	}
	if config.Logger == nil {
		config.Logger = slog.Default()
	}
	for _, directory := range []string{config.DataDir, config.ConfigDir} {
		if err := os.MkdirAll(directory, 0700); err != nil {
			return nil, err
		}
	}
	dataPath, err := filepath.EvalSymlinks(config.DataDir)
	if err != nil {
		return nil, err
	}
	configPath, err := filepath.EvalSymlinks(config.ConfigDir)
	if err != nil {
		return nil, err
	}
	if nestedPath(dataPath, configPath) || nestedPath(configPath, dataPath) {
		return nil, fs.ErrPermission
	}
	accounts, err := store.OpenAccounts(config.ConfigDir, config.AdminUsername, config.AdminPassword)
	if err != nil {
		return nil, err
	}
	root, err := store.OpenFilesystem(config.DataDir, config.AdminUsername)
	if err != nil {
		return nil, err
	}
	root.Close()
	staticFiles, err := fs.Sub(webFiles, "web")
	if err != nil {
		return nil, err
	}
	return &Server{config: config, accounts: accounts, static: http.FileServerFS(staticFiles), sessions: make(map[string]session), failures: make(map[string]failure), locks: make(map[string]webdav.LockSystem), authSlots: make(chan struct{}, 4)}, nil
}

func nestedPath(parent, child string) bool {
	relative, err := filepath.Rel(parent, child)
	return err == nil && relative != ".." && !strings.HasPrefix(relative, ".."+string(filepath.Separator))
}

func (application *Server) ServeHTTP(writer http.ResponseWriter, request *http.Request) {
	started := time.Now()
	recorded := &responseWriter{ResponseWriter: writer}
	defer func() {
		application.config.Logger.Info("http", "method", request.Method, "path", request.URL.EscapedPath(), "status", recorded.status, "duration", time.Since(started).String())
	}()
	recorded.Header().Set("X-Content-Type-Options", "nosniff")
	recorded.Header().Set("Referrer-Policy", "no-referrer")
	recorded.Header().Set("X-Frame-Options", "DENY")
	recorded.Header().Set("Cache-Control", "no-store")
	recorded.Header().Set("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'")
	if !readMethod(request.Method) && !sameOrigin(request, application.config.SecureCookies) {
		writeError(recorded, 403, "不允许跨站操作")
		return
	}
	switch {
	case request.URL.Path == "/healthz":
		if request.Method != "GET" && request.Method != "HEAD" {
			recorded.WriteHeader(405)
			return
		}
		writeJSON(recorded, 200, map[string]string{"status": "ok"})
	case strings.HasPrefix(request.URL.Path, "/api/"):
		application.serveAPI(recorded, request)
	case strings.HasPrefix(request.URL.Path, "/dav/"):
		application.serveDAV(recorded, request)
	case request.URL.Path == "/dav":
		http.Redirect(recorded, request, "/dav/", http.StatusPermanentRedirect)
	case request.URL.Path == "/" || request.URL.Path == "/app.js" || request.URL.Path == "/style.css" || request.URL.Path == "/favicon.svg":
		if request.Method != "GET" && request.Method != "HEAD" {
			recorded.WriteHeader(405)
			return
		}
		application.static.ServeHTTP(recorded, request)
	default:
		http.NotFound(recorded, request)
	}
}

func (application *Server) serveDAV(writer http.ResponseWriter, request *http.Request) {
	user, _, ok := application.authorize(writer, request, true)
	if !ok {
		return
	}
	if user.ReadOnly && !readMethod(request.Method) {
		writeError(writer, 403, "账号为只读，不能修改文件")
		return
	}
	name := strings.TrimPrefix(request.URL.Path, "/dav/")
	cleaned, err := store.SafePath(name)
	if err != nil {
		writeError(writer, 403, "无效文件路径")
		return
	}
	if (request.Method == "DELETE" || request.Method == "MOVE" || request.Method == "COPY") && (cleaned == "" || cleaned == ".") {
		writeError(writer, 403, "不能删除、移动或复制账号根目录")
		return
	}
	if request.Method == "MOVE" || request.Method == "COPY" {
		destination, err := url.Parse(request.Header.Get("Destination"))
		if err != nil || destination.Opaque != "" || destination.User != nil || (destination.Host != "" && !strings.EqualFold(destination.Host, request.Host)) || (destination.Scheme != "" && destination.Scheme != "http" && destination.Scheme != "https") || !strings.HasPrefix(destination.Path, "/dav/") || destination.RawQuery != "" || destination.Fragment != "" {
			writeError(writer, 403, "目标必须位于当前账号目录内")
			return
		}
		target, err := store.SafePath(strings.TrimPrefix(destination.Path, "/dav/"))
		if err != nil || target == "" || target == "." {
			writeError(writer, 403, "无效目标路径")
			return
		}
	}
	if request.Header.Get("Depth") == "infinity" && request.Method == "PROPFIND" {
		writeError(writer, 403, "目录列表只支持 Depth 0 或 1")
		return
	}
	if request.Method == "PROPFIND" && request.Header.Get("Depth") == "" {
		request.Header.Set("Depth", "1")
	}
	filesystem, err := store.OpenFilesystem(application.config.DataDir, user.Username)
	if err != nil {
		writeError(writer, 500, "无法打开存储目录")
		return
	}
	defer filesystem.Close()
	if request.Method == "PUT" {
		if application.config.MaxUploadBytes > 0 {
			if request.ContentLength > application.config.MaxUploadBytes {
				writeError(writer, 413, "文件超过上传大小限制")
				return
			}
			request.Body = http.MaxBytesReader(writer, request.Body, application.config.MaxUploadBytes)
		}
		ctx, body := store.TrackUpload(request.Context(), request.Body)
		request = request.WithContext(ctx)
		request.Body = body
	}
	if request.Method == "GET" || request.Method == "HEAD" {
		writer.Header().Set("Content-Disposition", mime.FormatMediaType("attachment", map[string]string{"filename": path.Base(request.URL.Path)}))
	}
	application.mu.Lock()
	lockSystem := application.locks[user.Username]
	if lockSystem == nil {
		lockSystem = webdav.NewMemLS()
		application.locks[user.Username] = lockSystem
	}
	application.mu.Unlock()
	handler := &webdav.Handler{Prefix: "/dav", FileSystem: filesystem, LockSystem: lockSystem}
	handler.ServeHTTP(writer, request)
}

func readMethod(method string) bool {
	return method == "GET" || method == "HEAD" || method == "OPTIONS" || method == "PROPFIND"
}

func sameOrigin(request *http.Request, secure bool) bool {
	origin := request.Header.Get("Origin")
	if origin == "" {
		return true
	}
	parsed, err := url.Parse(origin)
	return err == nil && strings.EqualFold(parsed.Host, request.Host) && (parsed.Scheme == "https" || (!secure && parsed.Scheme == "http")) && parsed.User == nil && parsed.Path == "" && parsed.RawQuery == "" && parsed.Fragment == ""
}

type responseWriter struct {
	http.ResponseWriter
	status int
}

func (writer *responseWriter) WriteHeader(status int) {
	if writer.status == 0 {
		writer.status = status
		writer.ResponseWriter.WriteHeader(status)
	}
}
func (writer *responseWriter) Write(content []byte) (int, error) {
	if writer.status == 0 {
		writer.WriteHeader(200)
	}
	return writer.ResponseWriter.Write(content)
}
func (writer *responseWriter) Unwrap() http.ResponseWriter { return writer.ResponseWriter }
