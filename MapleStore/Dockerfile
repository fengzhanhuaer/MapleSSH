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
ENV MAPLESTORE_ADDR=:8080 MAPLESTORE_DATA_DIR=/data MAPLESTORE_CONFIG_DIR=/config
COPY --from=builder /out/maplestore /usr/local/bin/maplestore
COPY --chmod=755 docker-entrypoint.sh /usr/local/bin/maplestore-entrypoint
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD ["/usr/local/bin/maplestore", "healthcheck"]
ENTRYPOINT ["/usr/local/bin/maplestore-entrypoint"]
