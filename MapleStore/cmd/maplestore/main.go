package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"syscall"
	"time"

	"maplestore/internal/server"
)

func main() {
	if err := run(); err != nil {
		slog.Error("MapleStore stopped", "error", err)
		os.Exit(1)
	}
}

func run() error {
	address := envDefault("MAPLESTORE_ADDR", ":8080")
	if len(os.Args) == 2 && os.Args[1] == "healthcheck" {
		_, port, err := net.SplitHostPort(address)
		if err != nil {
			return err
		}
		client := &http.Client{Timeout: 3 * time.Second}
		response, err := client.Get("http://127.0.0.1:" + port + "/healthz")
		if err != nil {
			return err
		}
		defer response.Body.Close()
		if response.StatusCode != 200 {
			return fmt.Errorf("health check status: %d", response.StatusCode)
		}
		return nil
	}
	if len(os.Args) > 1 {
		return errors.New("usage: maplestore [healthcheck]")
	}
	secure, err := strconv.ParseBool(envDefault("MAPLESTORE_SECURE_COOKIE", "false"))
	if err != nil {
		return fmt.Errorf("MAPLESTORE_SECURE_COOKIE: %w", err)
	}
	maximum, err := strconv.ParseInt(envDefault("MAPLESTORE_MAX_UPLOAD_BYTES", "10737418240"), 10, 64)
	if err != nil || maximum < 0 {
		return errors.New("MAPLESTORE_MAX_UPLOAD_BYTES must be a non-negative integer")
	}
	application, err := server.New(server.Config{DataDir: envDefault("MAPLESTORE_DATA_DIR", "./data"), ConfigDir: envDefault("MAPLESTORE_CONFIG_DIR", "./config"), AdminUsername: os.Getenv("MAPLESTORE_ADMIN_USERNAME"), AdminPassword: os.Getenv("MAPLESTORE_ADMIN_PASSWORD"), SecureCookies: secure, MaxUploadBytes: maximum})
	if err != nil {
		return err
	}
	httpServer := &http.Server{Addr: address, Handler: application, ReadHeaderTimeout: 10 * time.Second, IdleTimeout: 120 * time.Second, MaxHeaderBytes: 16 << 10}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	go func() {
		<-ctx.Done()
		shutdownContext, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		if err := httpServer.Shutdown(shutdownContext); err != nil {
			httpServer.Close()
		}
	}()
	slog.Info("MapleStore starting", "address", address, "data", envDefault("MAPLESTORE_DATA_DIR", "./data"), "admin", os.Getenv("MAPLESTORE_ADMIN_USERNAME"), "secure_cookie", secure)
	if err := httpServer.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		return err
	}
	return nil
}

func envDefault(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}
