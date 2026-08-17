# syntax=docker/dockerfile:1
# Builds the SLP debug viewer for Coolify/Docker deployment.
#
# Coolify's Dockerfile build pack does not persist anonymous volumes.
# Keep a named volume (see docker-compose.yaml) or a Coolify Persistent
# Storage mount at /data/replays so uploads, game.iso, and cache survive
# rebuilds. The VOLUME line below is only a hint for local `docker run`.
#
# Coolify may inject per-deploy --build-arg values that bust layer cache.
# Independent stages + cache mounts keep apk/npm from re-downloading.
# In Coolify → Advanced: keep "Disable Build Cache" off; turn off
# "Include Source Commit in Build" and "Inject Build Args to Dockerfile".

FROM alpine:3.20 AS c-build
RUN --mount=type=cache,target=/etc/apk/cache \
    apk add build-base zlib-dev zlib-static
WORKDIR /src
COPY src/ src/
COPY tools/ tools/
COPY Makefile .
RUN make bin/viewer bin/extract_tool CFLAGS="-std=c11 -Wall -Wextra -O2 -static"

FROM node:20-alpine AS web-build
WORKDIR /src
COPY package.json package-lock.json ./
RUN --mount=type=cache,target=/root/.npm \
    npm ci
COPY tsconfig.json ./
COPY web/ web/
RUN npm run build

FROM alpine:3.20
RUN --mount=type=cache,target=/etc/apk/cache \
    apk add libc6-compat wget
RUN mkdir -p /data/replays
COPY --from=c-build /src/bin/viewer /usr/local/bin/viewer
COPY --from=c-build /src/bin/extract_tool /usr/local/bin/extract_tool
COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
COPY --from=web-build /src/web/ /web/
ENV PORT=8080 SLP_DIR=/data/replays ASSET_DIR=/data/replays/cache HOST=0.0.0.0 WEB_DIR=/web
EXPOSE 8080
VOLUME ["/data/replays"]
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
