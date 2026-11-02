# MapleStore Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** 创建一个默认使用 Compose 同级 data/config 目录、支持账号隔离和网页管理的轻量 NAS 存储服务。

**Architecture:** 单个 Go HTTP 服务同时承载 WebDAV、账号 API 和内嵌静态网页。账号元数据使用受锁保护的 JSON 原子更新；文件以账号为根目录隔离。

**Tech Stack:** Go 1.26、golang.org/x/net/webdav、golang.org/x/crypto/argon2、原生 HTML/CSS/JavaScript、Docker Compose。

**Spec:** MapleStore/docs/design.md

## Global Constraints

- 只在 MapleStore 子工程内创建文件，不修改父目录或提交 Git。
- 默认数据路径 ./data/<账号名>/；配置 ./config 不得通过 WebDAV 暴露。
- 不引入数据库、前端构建链、共享目录、回收站或历史版本。

## Task 1: 可验证的账号与文件接口

**Files:** internal/store/accounts.go, internal/store/accounts_test.go, internal/store/filesystem.go, internal/store/filesystem_test.go

**Interfaces:** 持久化账号存储和实现 webdav.FileSystem 的受限文件系统。

- [ ] 先编写密码、持久化、账号名和路径隔离失败测试并运行。
- [ ] 实现密码哈希、账号 JSON 原子保存和安全文件根。
- [ ] 运行针对性测试并确保上传错误不损坏旧文件。

## Task 2: HTTP 服务与管理页面

**Files:** internal/server/server.go, internal/server/server_test.go, internal/server/web/*, cmd/maplestore/main.go

**Interfaces:** WebDAV /dav/、会话 API /api/、静态页面 /。

- [ ] 先编写未认证、文件操作、权限、会话失效和 CSRF 集成测试并运行。
- [ ] 实现 HTTP 服务、限速、会话、用户管理和日志。
- [ ] 创建轻量文件管理及账号管理页面，测试操作、错误与移动端布局。

## Task 3: 部署与完整验证

**Files:** Dockerfile, compose.yaml, .dockerignore, .gitignore, .env.example, README.md

- [ ] 使用绑定挂载 ./data:/data 和 ./config:/config，无命名卷。
- [ ] 编写首次启动、密码、安全访问、WebDAV 使用和备份迁移说明。
- [ ] 执行 gofmt、go test、go vet、Linux amd64/arm64 构建和本机 HTTP 冒烟。
- [ ] 记录 Docker 是否可用，清楚区分已验证和待 NAS 验证。
