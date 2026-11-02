# MapleStore

一个部署在个人 NAS 上的轻量 Go 云端存储服务。一个容器同时提供中文网页文件管理、WebDAV 和多账号管理，不需要数据库或前端构建环境。

## 从 GitHub 安装（默认方式）

GitHub Actions 负责测试、构建并发布镜像；NAS 只拉取成品，不需要 Go、源码或 BuildKit。

正式版本发布后，从本仓库 GitHub Releases 下载 `MapleStore-install-<版本>.tar.gz` 和同名 `.sha256` 文件，放在同一个目录，先校验再解压：

```sh
sha256sum -c MapleStore-install-1.0.0.tar.gz.sha256
tar -xzf MapleStore-install-1.0.0.tar.gz
cd MapleStore
```

上面的 `1.0.0` 是命令示例，不表示该版本已经发布。安装包只包含 `compose.yaml` 和本文档；也可以从源码仓库单独复制这两个文件，不需要 `.env`。

在安装目录执行：

```sh
chmod 600 compose.yaml
```

直接编辑 `compose.yaml` 中的管理员账号和密码，密码至少 12 字节：

```yaml
    environment:
      MAPLESTORE_ADMIN_USERNAME: "admin"
      MAPLESTORE_ADMIN_PASSWORD: "替换为你自己的长随机密码"
      MAPLESTORE_SECURE_COOKIE: "false"
```

默认密码为空，必须填写后才能启动。密码中的每个 `$` 请写成 `$$`，避免 Compose 将其解释为变量；`#` 等字符保留在引号内。不要使用文档示例密码，不要提交或共享填写过密码的 `compose.yaml`。

```sh
docker compose pull
docker compose up -d
docker compose ps
docker compose logs --tail=50
```

默认镜像为 `ghcr.io/fengzhanhuaer/maplestore:latest`，包含 linux/amd64、linux/arm64 两种架构，由 Docker 自动选择。main 或正式标签首次成功发布后即可拉取 `latest`。正式安装包的 `compose.yaml` 会固定为该次发布的镜像版本；源码中的示例使用 `latest`。

默认只监听 NAS 回环地址 `127.0.0.1:8080`，适合通过 NAS 的 HTTPS 反向代理访问。网页入口是 `/`，WebDAV 入口是 `/dav/`。默认不公开监听所有网络接口。

若仅在可信内网/VPN 内直接测试，直接修改 `compose.yaml` 的 `ports`。例如 `"192.168.1.10:18080:8080"` 表示绑定 NAS 的 `192.168.1.10:18080`，右侧容器端口 `8080` 不变。请使用 NAS 实际的 IP；`0.0.0.0` 会监听所有接口，应额外配置防火墙。HTTP 不加密账号密码和文件，不要裸露到公网。

### 默认数据位置

所有相对路径以 `compose.yaml` 所在目录为基准，不使用 Docker 命名卷：

```text
MapleStore/
├── compose.yaml
├── data/
│   ├── admin/
│   ├── alice/
│   └── bob/
└── config/
    ├── accounts.json
    └── password.key
```

- `./data:/data`：文件内容。每个账号固定为 `./data/<账号名>/`，账号只能访问自己的根目录。
- `./config:/config`：账号、Argon2id 密码哈希、成员密码密文及解密密钥，不在 WebDAV 可访问范围内。
- 缺失的挂载目录由 Compose 创建；首次启动创建管理员的目录，管理页面创建普通账号时建立其目录。
- 启动脚本仅调整 `data` 和 `config` 两个顶层目录的属主，然后降权为 `1000:1000` 运行，不递归修改已有文件。
- NAS 使用其他 UID/GID 时，在首次启动前按下方高级设置添加 `PUID`、`PGID`。已有数据迁移或更改 UID/GID 后，请由 NAS 管理员检查文件权限。

### 管理员凭据

管理员账号和密码直接填写在 Compose 的 `environment` 中，也可以在 NAS 容器界面设置相应容器环境变量。不再依赖 `.env` 的外部引用；已有的账号、端口等自定义值请迁移到 `compose.yaml`。

- 没有默认密码，未提供密码时拒绝启动。
- 管理员密码每次启动以容器环境变量为准；修改 `compose.yaml` 后执行 `docker compose up -d --force-recreate`。
- 管理员账号名初始化后保持不变，避免旧目录和账号身份产生歧义。普通账号由网页维护。
- 网页不能修改管理员密码、停用管理员或将普通账号提升为管理员。
- 管理员的文件入口同样只显示自己的目录；但管理员能查看、重置成员密码，因此可以使用成员身份访问数据，不应把它当作对管理员保密的零知识系统。
- `compose.yaml` 内的管理员密码是明文，容器环境也可由 NAS/Docker 管理员读取；限制文件权限和 NAS 管理权限。

### 管理员查看成员密码

管理员进入“账号管理”，点击成员行的“查看密码”，即可单独显示该成员的当前密码。普通成员不能使用此接口，账号列表不会返回密码；关闭弹窗或退出登录会清除页面上的密码内容。成员自行修改密码、管理员重置密码后，查看到的都是新密码。管理员自身的密码仍由 Compose 管理，不在网页显示。

- 登录认证继续使用 Argon2id 哈希；成员密码另以 AES-256-GCM 加密保存在 `accounts.json`，不保存明文。
- 首次启动自动生成随机 `config/password.key`，不需要增加 Compose 配置。查看密码要求管理员网页登录会话和 CSRF 校验，响应不缓存，日志只记录查看者和成员账号，不记录密码。
- 请完整备份 `config`。已有账号时密钥丢失、损坏或密码密文无效会拒绝启动，不会自动生成新密钥覆盖旧数据。本功能面向首次部署，不提供旧版仅哈希账号的迁移逻辑。
- 密钥与密文位于同一配置目录：拿到整个 `config` 的人也能解密成员密码；这只避免直接明文存储，不能防护宿主机管理员或完整配置泄露。网页查看请使用 HTTPS。

## 功能

- 网页：登录、浏览、当前目录筛选、上传进度与取消、下载、新建目录、重命名、移动、永久删除。
- WebDAV：GET/HEAD、PROPFIND、PUT、MKCOL、COPY、MOVE、DELETE、LOCK/UNLOCK。
- 普通账号：独立目录，可设只读、停用、重置密码；支持修改自己的密码，管理员可查看成员当前密码。
- 修改密码、权限或停用账号后，后续请求会重新校验账号版本，旧网页登录会话失效；已经开始的传输不保证立即中止。
- 上传失败不会替换已存在文件；成功 PUT 会覆盖同名文件，不保留历史版本。网页会在已知同名文件覆盖前确认。
- 不支持共享目录、回收站、自动同步、数据库、配额、版本化、断点续传或持久化 WebDAV 锁。

删除文件夹会删除其全部内容，不提供撤销。停用账号只阻止访问，不删除文件或账号记录。

## HTTPS 反向代理

反向代理需要保留原始 Host（包括非默认端口），允许 WebDAV 方法、Destination 和 If 请求头，不要重写 `/dav/` 路径。服务不信任 X-Forwarded-Host。

HTTPS 部署将 `compose.yaml` 中的 `MAPLESTORE_SECURE_COOKIE` 改为 `"true"`，再重新创建容器。该选项保护网页登录 Cookie；它不会让 Go 服务自动提供 TLS，也不会阻止直接使用 HTTP 的 Basic Auth，因此仍须限制后端端口只由代理/VPN 访问。

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

| Compose 配置项 | 默认值 | 含义 |
|---|---|---|
| `image` | `ghcr.io/fengzhanhuaer/maplestore:latest` | 已发布镜像；直接改为具体版本或 digest |
| `MAPLESTORE_ADMIN_USERNAME` | Compose 为 `admin` | 管理员账号，3–32 位小写字母、数字、下划线或连字符，以字母开头 |
| `MAPLESTORE_ADMIN_PASSWORD` | 无，必填 | 管理员密码，12–1024 字节 |
| `ports` | `127.0.0.1:8080:8080` | 宿主机 IP、宿主机端口、容器端口 |
| `MAPLESTORE_SECURE_COOKIE` | `false` | HTTPS 访问时设置为 `true` |

默认只需填写管理员密码；镜像、绑定地址、端口和其他值均直接在 `compose.yaml` 修改，没有额外的外部配置变量。数据路径和健康检查由镜像提供默认值，Compose 保留只读根文件系统、最小权限和日志限额。

### 高级设置（可选）

需要调整 NAS 文件属主或上传大小时，在 `compose.yaml` 已有的 `environment` 下添加相应项，保留管理员配置。例如：

```yaml
      PUID: "1026"
      PGID: "100"
      MAPLESTORE_MAX_UPLOAD_BYTES: "21474836480"
```

`PUID`、`PGID` 默认均为 `1000`，不能为 0。单次 PUT 默认最大 10 GiB，示例改为 20 GiB，设为 `0` 则不限制；流式超限返回上传失败。此前使用 `MAPLESTORE_UID`、`MAPLESTORE_GID` 的配置需改为上述写法。

程序直接运行时另支持 `MAPLESTORE_ADDR`（默认 `:8080`）、`MAPLESTORE_DATA_DIR`（默认 `./data`）、`MAPLESTORE_CONFIG_DIR`（默认 `./config`）。镜像默认数据路径为 `/data` 和 `/config`。

## 开发与验证

需要 Go 1.26 或更新的兼容版本。网页资源内嵌进二进制，修改网页后需要重新构建。

```sh
go test ./...
go vet ./...
go build -o ./bin/maplestore ./cmd/maplestore
```

直接启动二进制前，需在当前进程设置 `MAPLESTORE_ADMIN_USERNAME` 和 `MAPLESTORE_ADMIN_PASSWORD`。这些启动参数从进程环境读取；Docker 部署时由 Compose 的 `environment` 传入。

默认 Compose 不包含 build，开发时如需本地构建，另加源码仓库中的覆盖配置（需要 BuildKit）：

```sh
docker compose -f compose.yaml -f compose.build.yaml up -d --build --pull never
```

本地构建需要调整 Go 模块代理时，改用以下命令构建并启动；默认使用 `https://proxy.golang.org,direct`，拉取成品镜像不需要设置：

```sh
docker compose -f compose.yaml -f compose.build.yaml build --build-arg GOPROXY=https://goproxy.cn,direct
docker compose -f compose.yaml -f compose.build.yaml up -d --pull never
```

Dockerfile 支持 BuildKit 的目标架构参数。正式发布由 GitHub Actions 使用 Buildx 构建 linux/amd64、linux/arm64 镜像，不要求 NAS 具备构建能力。

## GitHub 构建与发布

工作流位于仓库根目录 `.github/workflows/maplestore.yml`，而不是子工程内部。

| 触发方式 | 行为 |
|---|---|
| 修改 MapleStore 的 PR | Go race 测试、vet、镜像构建、Compose 校验和容器冒烟；不发布 |
| 推送到 main 或手动运行默认分支 | 验证通过后发布 `latest`、`edge` 和 `sha-<提交前12位>` 镜像，不创建版本化 Release |
| 推送 `maplestore-v1.0.0` 一类正式标签 | 验证通过后发布 `1.0.0`、`latest`、SHA 镜像，并创建 Release 安装包及 SHA-256 文件 |

只接受 `maplestore-v主版本.次版本.补丁` 形式的稳定版本标签，避免与其他子工程混淆；不接受预发布后缀。不要重复使用已发布版本号。`latest` 跟随最近完成的 main 或正式标签发布，不等同于稳定版；需要固定正式版时使用具体版本号或 digest。发布后会检查本次所有镜像标签能否从仓库读取。

首次发布准备：

1. 将工作流和 MapleStore 源码推送到 GitHub 的 main，确认 Actions 验证与镜像发布通过；此时已提供默认 Compose 使用的 `latest`，同时保留 `edge` 镜像。
2. 维护者创建并推送正式标签，例如 `git tag maplestore-v1.0.0`、`git push origin maplestore-v1.0.0`。
3. 等待对应工作流的 verify、publish、release 三个任务成功，再下载安装包或拉取正式镜像。

工作流通过仓库内置 `GITHUB_TOKEN` 发布，无须额外配置 Docker Hub 密钥。仓库/组织必须允许 Actions 写入 Packages；镜像首次创建通常为 private，匿名拉取前需在 GitHub Packages 中显式设为 public，不会由工作流自动公开。

若保留私有镜像，在 NAS 执行 `docker login ghcr.io --username 你的GitHub用户名`，交互输入具有 `read:packages` 权限的 personal access token (classic)，不要使用 GitHub 登录密码，不要把 token 写进 Compose。发布公共镜像意味着其他人也能拉取程序镜像，但镜像不包含 `.env`、用户数据或账号配置。

Fork 本仓库时，工作流使用当前仓库 owner 的小写名称发布 `ghcr.io/<owner>/maplestore`；源码中的默认 Compose 仍指向原维护者镜像，需修改 `image`，正式安装包会自动写入当前发布者的镜像。

### 升级与回滚

如 `compose.yaml` 的 `image` 固定了具体版本，先修改为目标版本。使用 `latest` 时直接拉取即可：

```sh
docker compose pull
docker compose up -d
docker compose ps
```

升级不重建、不覆盖同级 `data` 和 `config` 目录。升级前仍建议备份二者；程序回滚时可能需要恢复相应版本的配置，不能假设未来版本配置永远兼容旧版。

回滚时将 `compose.yaml` 的 `image` 改为原来的具体版本，再执行相同命令。`latest` 和 `edge` 都可能包含尚未正式发布的改动，不要只保留一份重要文件；默认不会自动更新 NAS 上运行的容器。

## 数据保护和边界

- 建议备份 `data`、`config`，并单独安全保存管理员配置；账号隔离不代替 NAS 快照和备份。
- 一个配置目录同时只运行一个实例，暂不支持多副本共享配置或多进程写入。
- 防护覆盖通过服务接口进行的跨账号访问；NAS/Docker 管理员、宿主机 root、硬链接、挂载点和磁盘直接访问不属于此边界，不承诺静态数据加密。
- 不允许不可信程序直接修改存储目录或账号配置；不要将包含设备文件、挂载点或敏感硬链接的目录导入账号空间。
- 日志不记录密码、请求体、Cookie 或 Authorization，但会记录文件路径，应限制日志读取权限。
- 停止服务后复制整个目录即可迁移；`docker compose down` 不会删除绑定挂载数据。手动删除 `./data` 或 `./config` 会造成数据或账号丢失。
