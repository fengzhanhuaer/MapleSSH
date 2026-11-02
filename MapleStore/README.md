# MapleStore

一个部署在个人 NAS 上的轻量 Go 云端存储服务。一个容器同时提供中文网页文件管理、WebDAV 和多账号管理，不需要数据库或前端构建环境。

## 默认 Docker Compose

仓库自带 `compose.yaml`。将整个 `MapleStore` 目录复制到 NAS 后，在这个目录执行：

```sh
cp .env.example .env
chmod 600 .env
```

编辑 `.env`，设置管理员账号和至少 12 字节的强密码：

```dotenv
MAPLESTORE_ADMIN_USERNAME=admin
MAPLESTORE_ADMIN_PASSWORD='替换为你自己的长随机密码'
```

密码含 `$`、`#` 等字符时，请保留 `.env` 中的单引号。不要使用文档示例密码，不要提交或共享 `.env`。

```sh
docker compose up -d --build
docker compose ps
docker compose logs --tail=50
```

默认只监听 NAS 回环地址 `127.0.0.1:8080`，适合通过 NAS 的 HTTPS 反向代理访问。网页入口是 `/`，WebDAV 入口是 `/dav/`。默认不公开监听所有网络接口。

若仅在可信内网/VPN 内直接测试，可以将 `.env` 的 `MAPLESTORE_BIND_IP` 改为 NAS 对应网络接口的 IP；`0.0.0.0` 会监听所有接口，应额外配置防火墙。HTTP 不加密账号密码和文件，不要裸露到公网。

### 默认数据位置

所有相对路径以 `compose.yaml` 所在目录为基准，不使用 Docker 命名卷：

```text
MapleStore/
├── compose.yaml
├── .env
├── data/
│   ├── admin/
│   ├── alice/
│   └── bob/
└── config/
    └── accounts.json
```

- `./data:/data`：文件内容。每个账号固定为 `./data/<账号名>/`，账号只能访问自己的根目录。
- `./config:/config`：账号及 Argon2id 密码哈希，不在 WebDAV 可访问范围内。
- 缺失的挂载目录由 Compose 创建；首次启动创建管理员的目录，管理页面创建普通账号时建立其目录。
- 启动脚本仅调整 `data` 和 `config` 两个顶层目录的属主，然后降权为 `MAPLESTORE_UID:MAPLESTORE_GID` 运行，不递归修改已有文件。
- NAS 使用其他 UID/GID 时，在首次启动前设置对应值。已有数据迁移或更改 UID/GID 后，请由 NAS 管理员检查文件权限。

### 管理员凭据

管理员账号和密码通过 Compose 的 `environment` 传入。`.env` 只是默认取值来源，也可以在 NAS 容器界面设置相应变量。

- 没有默认密码，未提供密码时拒绝启动。
- 管理员密码每次启动以环境变量为准；修改 `.env` 后执行 `docker compose up -d --force-recreate`。
- 管理员账号名初始化后保持不变，避免旧目录和账号身份产生歧义。普通账号由网页维护。
- 网页不能修改管理员密码、停用管理员或将普通账号提升为管理员。
- 管理员的文件入口同样只显示自己的目录；但管理员能重置普通账号密码，所以不应把它当作无法访问用户数据的零知识系统。
- `.env` 内的管理员密码是明文，容器环境也可由 NAS/Docker 管理员读取；限制文件权限和 NAS 管理权限。

## 功能

- 网页：登录、浏览、当前目录筛选、上传进度与取消、下载、新建目录、重命名、移动、永久删除。
- WebDAV：GET/HEAD、PROPFIND、PUT、MKCOL、COPY、MOVE、DELETE、LOCK/UNLOCK。
- 普通账号：独立目录，可设只读、停用、重置密码；支持修改自己的密码。
- 修改密码、权限或停用账号后，后续请求会重新校验账号版本，旧网页登录会话失效；已经开始的传输不保证立即中止。
- 上传失败不会替换已存在文件；成功 PUT 会覆盖同名文件，不保留历史版本。网页会在已知同名文件覆盖前确认。
- 不支持共享目录、回收站、自动同步、数据库、配额、版本化、断点续传或持久化 WebDAV 锁。

删除文件夹会删除其全部内容，不提供撤销。停用账号只阻止访问，不删除文件或账号记录。

## HTTPS 反向代理

反向代理需要保留原始 Host（包括非默认端口），允许 WebDAV 方法、Destination 和 If 请求头，不要重写 `/dav/` 路径。服务不信任 X-Forwarded-Host。

HTTPS 部署设置 `MAPLESTORE_SECURE_COOKIE=true`，再重新创建容器。该选项保护网页登录 Cookie；它不会让 Go 服务自动提供 TLS，也不会阻止直接使用 HTTP 的 Basic Auth，因此仍须限制后端端口只由代理/VPN 访问。

Nginx location 参考（证书和 server 配置按 NAS 环境设置）：

```nginx
location / {
    proxy_pass http://127.0.0.1:8080;
    proxy_set_header Host $http_host;
    proxy_request_buffering off;
    proxy_buffering off;
    client_max_body_size 10g;
    proxy_read_timeout 3600s;
    proxy_send_timeout 3600s;
}
```

其他 NAS 反向代理也需检查上传大小和超时设置；不要将站点挂在 URL 子路径下。

## WebDAV 使用

客户端填写 `https://你的域名/dav/`，使用对应账号密码。每个账号看到的 `/dav/` 都是自己的根目录，不需要也不能添加其他账号名来访问其文件。

```sh
curl --user alice -X PROPFIND -H 'Depth: 1' https://你的域名/dav/
curl --user alice --upload-file ./example.txt https://你的域名/dav/example.txt
curl --user alice --output ./download.txt https://你的域名/dav/example.txt
curl --user alice -X MKCOL https://你的域名/dav/documents
curl --user alice -X MOVE -H 'Destination: https://你的域名/dav/documents/example.txt' -H 'Overwrite: F' https://你的域名/dav/example.txt
curl --user alice -X DELETE https://你的域名/dav/documents/example.txt
```

上述命令交互输入密码，避免把密码写入命令历史。非 ASCII 文件名应进行 URL 编码。目录列表支持 Depth 0/1，不支持无限深度枚举。

## 配置

| 变量 | 默认值 | 含义 |
|---|---|---|
| `MAPLESTORE_ADMIN_USERNAME` | Compose 为 `admin` | 管理员账号，3–32 位小写字母、数字、下划线或连字符，以字母开头 |
| `MAPLESTORE_ADMIN_PASSWORD` | 无，必填 | 管理员密码，12–1024 字节 |
| `MAPLESTORE_BIND_IP` | `127.0.0.1` | Compose 在宿主机上绑定的地址 |
| `MAPLESTORE_PORT` | `8080` | 宿主机端口 |
| `MAPLESTORE_SECURE_COOKIE` | `false` | HTTPS 访问时设置为 `true` |
| `MAPLESTORE_UID` / `MAPLESTORE_GID` | `1000` / `1000` | 容器内服务运行身份，不能为 0 |
| `MAPLESTORE_MAX_UPLOAD_BYTES` | `10737418240` | 单次 PUT 最大 10 GiB，0 为不限制；流式超限返回上传失败 |
| `MAPLESTORE_GOPROXY` | `https://proxy.golang.org,direct` | 构建阶段 Go 模块代理，网络不通可改为 `https://goproxy.cn,direct` |

程序直接运行时另支持 `MAPLESTORE_ADDR`（默认 `:8080`）、`MAPLESTORE_DATA_DIR`（默认 `./data`）、`MAPLESTORE_CONFIG_DIR`（默认 `./config`）。Compose 内固定为 `/data` 和 `/config`。

## 开发与验证

需要 Go 1.26 或更新的兼容版本。网页资源内嵌进二进制，修改网页后需要重新构建。

```sh
go test ./...
go vet ./...
go build -o ./bin/maplestore ./cmd/maplestore
```

直接启动前，需在当前进程设置 `MAPLESTORE_ADMIN_USERNAME` 和 `MAPLESTORE_ADMIN_PASSWORD`。运行时不会自动读取 `.env`；Docker Compose 负责读取和传入变量。

Dockerfile 支持 BuildKit 的目标架构参数，可在对应 NAS 原生构建，或使用 buildx 构建 linux/amd64、linux/arm64 镜像。

## 数据保护和边界

- 建议备份 `data`、`config`，并单独安全保存管理员配置；账号隔离不代替 NAS 快照和备份。
- 一个配置目录同时只运行一个实例，暂不支持多副本共享配置或多进程写入。
- 防护覆盖通过服务接口进行的跨账号访问；NAS/Docker 管理员、宿主机 root、硬链接、挂载点和磁盘直接访问不属于此边界，不承诺静态数据加密。
- 不允许不可信程序直接修改存储目录或账号配置；不要将包含设备文件、挂载点或敏感硬链接的目录导入账号空间。
- 日志不记录密码、请求体、Cookie 或 Authorization，但会记录文件路径，应限制日志读取权限。
- 停止服务后复制整个目录即可迁移；`docker compose down` 不会删除绑定挂载数据。手动删除 `./data` 或 `./config` 会造成数据或账号丢失。
