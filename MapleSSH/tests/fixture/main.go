package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"time"

	"golang.org/x/crypto/ssh"
	"golang.org/x/net/webdav"
)

func run() error {
	root := flag.String("root", "", "isolated test directory")
	flag.Parse()
	if *root == "" {
		return fmt.Errorf("missing isolated fixture directory")
	}
	if err := os.MkdirAll(filepath.Join(*root, "dav"), 0700); err != nil {
		return err
	}
	random := make([]byte, 16)
	if _, err := rand.Read(random); err != nil {
		return err
	}
	password := hex.EncodeToString(random)
	passphrase := "fixture-private-key-passphrase"
	_, hostPrivate, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return err
	}
	hostSigner, err := ssh.NewSignerFromKey(hostPrivate)
	if err != nil {
		return err
	}
	_, clientPrivate, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return err
	}
	clientSigner, err := ssh.NewSignerFromKey(clientPrivate)
	if err != nil {
		return err
	}
	rsaPrivate, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		return err
	}
	rsaSigner, err := ssh.NewSignerFromKey(rsaPrivate)
	if err != nil {
		return err
	}
	plain, err := ssh.MarshalPrivateKey(clientPrivate, "MapleSSH isolated fixture")
	if err != nil {
		return err
	}
	encrypted, err := ssh.MarshalPrivateKeyWithPassphrase(clientPrivate, "MapleSSH isolated fixture", []byte(passphrase))
	if err != nil {
		return err
	}
	var authentications atomic.Int64
	config := &ssh.ServerConfig{
		MaxAuthTries: 3,
		PasswordCallback: func(connection ssh.ConnMetadata, supplied []byte) (*ssh.Permissions, error) {
			authentications.Add(1)
			if connection.User() == "fixture" && string(supplied) == password {
				return nil, nil
			}
			return nil, fmt.Errorf("invalid fixture credentials")
		},
		PublicKeyCallback: func(connection ssh.ConnMetadata, key ssh.PublicKey) (*ssh.Permissions, error) {
			authentications.Add(1)
			if connection.User() == "fixture" && (string(key.Marshal()) == string(clientSigner.PublicKey().Marshal()) || string(key.Marshal()) == string(rsaSigner.PublicKey().Marshal())) {
				return nil, nil
			}
			return nil, fmt.Errorf("invalid fixture key")
		},
	}
	config.AddHostKey(hostSigner)
	sshListener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return err
	}
	defer sshListener.Close()
	go func() {
		for {
			connection, acceptErr := sshListener.Accept()
			if acceptErr != nil {
				return
			}
			go func() {
				server, channels, requests, handshakeErr := ssh.NewServerConn(connection, config)
				if handshakeErr != nil {
					connection.Close()
					return
				}
				defer server.Close()
				go ssh.DiscardRequests(requests)
				for incoming := range channels {
					if incoming.ChannelType() != "session" {
						incoming.Reject(ssh.UnknownChannelType, "session only")
						continue
					}
					channel, channelRequests, channelErr := incoming.Accept()
					if channelErr != nil {
						continue
					}
					go func() {
						defer channel.Close()
						shellStarted := false
						for request := range channelRequests {
							accepted := false
							var size struct{ Columns, Rows, PixelWidth, PixelHeight uint32 }
							switch request.Type {
							case "pty-req":
								var terminal struct {
									Term                                   string
									Columns, Rows, PixelWidth, PixelHeight uint32
									Modes                                  string
								}
								accepted = ssh.Unmarshal(request.Payload, &terminal) == nil && terminal.Term == "xterm-256color" && terminal.Columns > 0 && terminal.Rows > 0
							case "window-change":
								accepted = ssh.Unmarshal(request.Payload, &size) == nil && size.Columns > 0 && size.Rows > 0
							case "shell":
								accepted = !shellStarted && len(request.Payload) == 0
							}
							request.Reply(accepted, nil)
							if accepted && request.Type == "shell" {
								shellStarted = true
								go func() {
									io.WriteString(channel, "\x1b[32mfixture-ready\x1b[0m\r\n中文终端测试：正在连接，正在认证，已连接。\r\n")
									io.Copy(channel, channel)
									channel.Close()
								}()
							} else if accepted && request.Type == "window-change" {
								fmt.Fprintf(channel, "fixture-resize:%dx%d\r\n", size.Columns, size.Rows)
							}
						}
					}()
				}
			}()
		}
	}()
	dav := &webdav.Handler{Prefix: "/dav", FileSystem: webdav.Dir(filepath.Join(*root, "dav")), LockSystem: webdav.NewMemLS()}
	mux := http.NewServeMux()
	var davWrites atomic.Int64
	mux.HandleFunc("/dav-write-metrics", func(writer http.ResponseWriter, request *http.Request) { fmt.Fprint(writer, davWrites.Load()) })
	mux.HandleFunc("/webpage/", func(writer http.ResponseWriter, request *http.Request) {
		writer.Header().Set("Content-Type", "text/html")
		io.WriteString(writer, "<html><body>Login page</body></html>")
	})
	mux.HandleFunc("/fake-dav/", func(writer http.ResponseWriter, request *http.Request) {
		writer.WriteHeader(http.StatusMultiStatus)
		io.WriteString(writer, "<html><body>Not WebDAV</body></html>")
	})
	mux.HandleFunc("/dav-denied/", func(writer http.ResponseWriter, request *http.Request) {
		writer.WriteHeader(http.StatusMultiStatus)
		io.WriteString(writer, `<d:multistatus xmlns:d="DAV:"><d:response><d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype></d:prop><d:status>HTTP/1.1 403 Forbidden</d:status></d:propstat></d:response></d:multistatus>`)
	})
	mux.HandleFunc("/metrics", func(writer http.ResponseWriter, request *http.Request) { fmt.Fprint(writer, authentications.Load()) })
	var redirected atomic.Int64
	mux.HandleFunc("/redirect-metrics", func(writer http.ResponseWriter, request *http.Request) { fmt.Fprint(writer, redirected.Load()) })
	mux.HandleFunc("/forbidden/", func(writer http.ResponseWriter, request *http.Request) { writer.WriteHeader(http.StatusForbidden) })
	mux.HandleFunc("/redirect/", func(writer http.ResponseWriter, request *http.Request) {
		http.Redirect(writer, request, "/capture/", http.StatusTemporaryRedirect)
	})
	mux.HandleFunc("/capture/", func(writer http.ResponseWriter, request *http.Request) {
		redirected.Add(1)
		writer.WriteHeader(http.StatusOK)
	})
	mux.HandleFunc("/slow/", func(writer http.ResponseWriter, request *http.Request) {
		time.Sleep(800 * time.Millisecond)
		writer.WriteHeader(http.StatusNotFound)
	})
	mux.HandleFunc("/timeout/", func(writer http.ResponseWriter, request *http.Request) {
		select {
		case <-request.Context().Done():
		case <-time.After(15 * time.Second):
			writer.WriteHeader(http.StatusNotFound)
		}
	})
	if err := os.MkdirAll(filepath.Join(*root, "corrupt"), 0700); err != nil {
		return err
	}
	corrupt := &webdav.Handler{Prefix: "/corrupt", FileSystem: webdav.Dir(filepath.Join(*root, "corrupt")), LockSystem: webdav.NewMemLS()}
	mux.HandleFunc("/corrupt/", func(writer http.ResponseWriter, request *http.Request) {
		if request.Method == "GET" && strings.HasSuffix(request.URL.Path, ".msb") {
			io.WriteString(writer, "damaged upload response")
			return
		}
		corrupt.ServeHTTP(writer, request)
	})
	mux.HandleFunc("/dav/", func(writer http.ResponseWriter, request *http.Request) {
		if request.Method != "PROPFIND" && request.Method != "GET" && request.Method != "HEAD" && request.Method != "OPTIONS" {
			davWrites.Add(1)
		}
		username, supplied, valid := request.BasicAuth()
		if !valid || username != "fixture" || supplied != password {
			writer.Header().Set("WWW-Authenticate", "Basic realm=maplessh-fixture")
			writer.WriteHeader(401)
			return
		}
		dav.ServeHTTP(writer, request)
	})
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return err
	}
	defer listener.Close()
	information := map[string]any{
		"host_public_key": string(ssh.MarshalAuthorizedKey(hostSigner.PublicKey())),
		"username":        "fixture", "password": password,
		"ssh_port":              sshListener.Addr().(*net.TCPAddr).Port,
		"dav_port":              listener.Addr().(*net.TCPAddr).Port,
		"private_key":           string(pem.EncodeToMemory(plain)),
		"encrypted_private_key": string(pem.EncodeToMemory(encrypted)),
		"rsa_private_key":       string(pem.EncodeToMemory(&pem.Block{Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(rsaPrivate)})),
		"passphrase":            passphrase,
	}
	content, err := json.Marshal(information)
	if err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(*root, "ready.json"), content, 0600); err != nil {
		return err
	}
	fmt.Println("isolated SSH and WebDAV fixture ready")
	err = http.Serve(listener, mux)
	if err != nil && !strings.Contains(err.Error(), "closed network connection") {
		return err
	}
	return nil
}
func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
