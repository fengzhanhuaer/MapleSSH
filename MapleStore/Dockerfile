FROM --platform=$BUILDPLATFORM golang:1.26-alpine AS builder

ARG TARGETOS
ARG TARGETARCH
ARG GOPROXY=https://proxy.golang.org,direct
WORKDIR /src
COPY go.mod go.sum ./
RUN GOPROXY="$GOPROXY" go mod download
COPY cmd ./cmd
COPY internal ./internal
RUN CGO_ENABLED=0 GOOS=$TARGETOS GOARCH=$TARGETARCH go build -trimpath -ldflags="-s -w" -o /out/maplestore ./cmd/maplestore

FROM alpine:3.23
RUN apk add --no-cache ca-certificates su-exec
COPY --from=builder /out/maplestore /usr/local/bin/maplestore
COPY --chmod=755 docker-entrypoint.sh /usr/local/bin/maplestore-entrypoint
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/maplestore-entrypoint"]
