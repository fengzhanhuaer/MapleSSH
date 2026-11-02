#!/usr/bin/env bash
set -euo pipefail

image="${1:-maplestore:ci}"
work_directory=$(mktemp -d -t maplestore-smoke.XXXXXXXX)
container_id=""

cleanup() {
    if [[ -n "$container_id" ]]; then docker rm -f "$container_id" >/dev/null 2>&1 || true; fi
    if [[ -d "$work_directory" && "$(basename "$work_directory")" == maplestore-smoke.* ]]; then
        rm -rf -- "$work_directory"
    fi
}
trap cleanup EXIT

mkdir "$work_directory/data" "$work_directory/config"
runtime_uid=$(id -u)
runtime_gid=$(id -g)
if [[ "$runtime_uid" == 0 ]]; then runtime_uid=1000; fi
if [[ "$runtime_gid" == 0 ]]; then runtime_gid=1000; fi
password=$(python3 -c 'import secrets; print(secrets.token_urlsafe(32))')

container_id=$(docker run --detach \
    --publish 127.0.0.1::8080 \
    --read-only --tmpfs /tmp:rw,noexec,nosuid,size=16m \
    --cap-drop ALL --cap-add CHOWN --cap-add SETUID --cap-add SETGID \
    --security-opt no-new-privileges:true \
    --env "MAPLESTORE_ADMIN_USERNAME=admin" \
    --env "MAPLESTORE_ADMIN_PASSWORD=$password" \
    --env MAPLESTORE_DATA_DIR=/data --env MAPLESTORE_CONFIG_DIR=/config \
    --env "PUID=$runtime_uid" --env "PGID=$runtime_gid" \
    --mount "type=bind,source=$work_directory/data,target=/data" \
    --mount "type=bind,source=$work_directory/config,target=/config" \
    "$image")

port=$(docker port "$container_id" 8080/tcp | head -n1)
export SMOKE_URL="http://$port"
export SMOKE_PASSWORD="$password"

python3 - <<'PY'
import base64
import json
import os
import time
import urllib.error
import urllib.request

base = os.environ["SMOKE_URL"]
password = os.environ["SMOKE_PASSWORD"]

def call(path, method="GET", data=None, headers=None, expected=200):
    request = urllib.request.Request(base + path, data=data, method=method, headers=headers or {})
    try:
        response = urllib.request.urlopen(request, timeout=5)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        content = response.read()
        if response.status != expected:
            raise AssertionError(f"{method} {path}: {response.status}, expected {expected}: {content!r}")
        return response.headers, content

for attempt in range(30):
    try:
        call("/healthz")
        break
    except (OSError, AssertionError):
        if attempt == 29:
            raise
        time.sleep(1)

call("/")
headers, content = call("/api/login", "POST", json.dumps({"username":"admin", "password":password}).encode(), {"Content-Type":"application/json"})
session = json.loads(content)
cookie = headers["Set-Cookie"].split(";", 1)[0]
admin_headers = {"Cookie":cookie, "X-CSRF-Token":session["csrf"], "Content-Type":"application/json"}
for username in ("alice", "bob"):
    call("/api/users", "POST", json.dumps({"username":username, "password":password}).encode(), admin_headers, 201)

def basic(username):
    return {"Authorization":"Basic " + base64.b64encode(f"{username}:{password}".encode()).decode()}

call("/dav/proof.txt", "PUT", b"persistent-data", basic("alice"), 201)
_, content = call("/dav/proof.txt", headers=basic("alice"))
assert content == b"persistent-data"
call("/dav/proof.txt", headers=basic("bob"), expected=404)
call("/dav/proof.txt", headers=basic("admin"), expected=404)
call("/dav/proof.txt", expected=401)
print("Container startup, login, WebDAV and account isolation verified")
PY

docker exec --user "$runtime_uid:$runtime_gid" "$container_id" /usr/local/bin/maplestore healthcheck
docker exec "$container_id" sh -c 'test "$(awk '\''/^Uid:/{print $2}'\'' /proc/1/status)" != 0'
docker restart "$container_id" >/dev/null
for attempt in {1..30}; do
    if curl --silent --fail "$SMOKE_URL/healthz" >/dev/null; then break; fi
    sleep 1
done
python3 - <<'PY'
import base64
import os
import urllib.request
headers = {"Authorization":"Basic " + base64.b64encode(("alice:" + os.environ["SMOKE_PASSWORD"]).encode()).decode()}
with urllib.request.urlopen(urllib.request.Request(os.environ["SMOKE_URL"] + "/dav/proof.txt", headers=headers), timeout=5) as response:
    assert response.read() == b"persistent-data"
print("Account and file persistence after restart verified")
PY
