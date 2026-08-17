# syntax=docker/dockerfile:1
# Builds the SLP debug viewer for Coolify/Docker deployment.
#
# Coolify's Dockerfile build pack does not persist anonymous volumes.
# Keep a named volume (see docker-compose.yaml) or a Coolify Persistent
# Storage mount at /data/replays so uploads, game.iso, and cache survive
# rebuilds. The VOLUME line below is only a hint for local `docker run`.
#
# Coolify injects stable ARGs after each FROM (URL/FQDN/branch/UUID), so
# layer cache should reuse apk/npm/ccache across commits. Per-commit image
# tags (uuid:sha) are new every deploy; that does not invalidate layers.

FROM alpine:3.22 AS c-build
RUN --mount=type=cache,id=apk-cbuild,target=/etc/apk/cache,sharing=locked \
    apk add gcc musl-dev make zlib-dev zlib-static ccache
WORKDIR /src
COPY src/ src/
COPY tools/ tools/
COPY Makefile .
RUN --mount=type=cache,id=ccache-viewer,target=/root/.ccache \
    make CC="ccache cc" bin/viewer bin/extract_tool \
        CFLAGS="-std=c11 -Wall -Wextra -O2 -static"

FROM node:20-alpine AS web-build
WORKDIR /src
COPY package.json package-lock.json ./
RUN --mount=type=cache,target=/root/.npm \
    npm ci
COPY web/ web/
RUN npm run build:web

FROM alpine:3.22
RUN --mount=type=cache,id=apk-runtime,target=/etc/apk/cache,sharing=locked \
    apk add wget
RUN mkdir -p /data/replays
COPY --from=c-build /src/bin/viewer /usr/local/bin/viewer
COPY --from=c-build /src/bin/extract_tool /usr/local/bin/extract_tool
COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
COPY --from=web-build /src/web/ /web/
ENV PORT=8080 SLP_DIR=/data/replays ASSET_DIR=/data/replays/cache HOST=0.0.0.0 WEB_DIR=/web
EXPOSE 8080
VOLUME ["/data/replays"]
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
