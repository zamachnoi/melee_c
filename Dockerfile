# Builds the SLP debug viewer for Coolify/Docker deployment.
# Data dir (/data/replays) is a volume so you can drop .slp files or upload
# them from the browser.

FROM alpine:3.20 AS build
RUN apk add --no-cache build-base zlib-dev zlib-static
WORKDIR /src
COPY src/ src/
COPY tools/ tools/
COPY Makefile .
RUN make bin/viewer bin/extract_tool CFLAGS="-std=c11 -Wall -Wextra -O2 -static"

FROM alpine:3.20
RUN apk add --no-cache libc6-compat
RUN mkdir -p /data/replays
COPY --from=build /src/bin/viewer /usr/local/bin/viewer
COPY --from=build /src/bin/extract_tool /usr/local/bin/extract_tool
COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
ENV PORT=8080 SLP_DIR=/data/replays ASSET_DIR=/data/replays/cache HOST=0.0.0.0
EXPOSE 8080
VOLUME ["/data/replays"]
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
