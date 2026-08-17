# Builds the SLP debug viewer for Coolify/Docker deployment.
#
# Coolify's Dockerfile build pack does not persist anonymous volumes.
# Keep a named volume (see docker-compose.yaml) or a Coolify Persistent
# Storage mount at /data/replays so uploads, game.iso, and cache survive
# rebuilds. The VOLUME line below is only a hint for local `docker run`.

FROM alpine:3.20 AS build
RUN apk add --no-cache build-base zlib-dev zlib-static nodejs npm
WORKDIR /src
COPY src/ src/
COPY tools/ tools/
COPY web/ web/
COPY package.json package-lock.json tsconfig.json ./
COPY Makefile .
RUN npm ci && npm run build
RUN make bin/viewer bin/extract_tool CFLAGS="-std=c11 -Wall -Wextra -O2 -static"

FROM alpine:3.20
RUN apk add --no-cache libc6-compat
RUN mkdir -p /data/replays
COPY --from=build /src/bin/viewer /usr/local/bin/viewer
COPY --from=build /src/bin/extract_tool /usr/local/bin/extract_tool
COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
COPY --from=build /src/web/ /web/
ENV PORT=8080 SLP_DIR=/data/replays ASSET_DIR=/data/replays/cache HOST=0.0.0.0 WEB_DIR=/web
EXPOSE 8080
VOLUME ["/data/replays"]
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
