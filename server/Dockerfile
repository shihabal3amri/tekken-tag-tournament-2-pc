# ttt2-server in a container (server/README.md "Docker"). Build from the server folder:
#   docker build -t ttt2-server --build-arg VERSION=0.1.8 .
# Run with a folder for server.toml and data/ (the first start writes server.toml and stops - edit it, start again):
#   docker run -it --name ttt2 -p 28200:28200/tcp -p 28200:28200/udp -v /srv/ttt2:/srv/ttt2 ttt2-server
# -it keeps the admin console (docker attach ttt2; detach with Ctrl+P Ctrl+Q); without it, admin commands go through
#   docker exec ttt2 /ttt2-server -config /srv/ttt2/server.toml admin <command>
# The web admin panel (off by default; README.md "Admin panel"): make its login with
#   docker exec ttt2 /ttt2-server -config /srv/ttt2/server.toml admin panel-setup
# set admin_panel = true in server.toml, and publish its port as well: -p 28201:28201/tcp
# The program runs as a non-root user: the mounted folder must be writable by uid 65532.
FROM golang:1.27 AS build
ARG VERSION=dev
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download && go mod verify
COPY . .
RUN CGO_ENABLED=0 go build -trimpath -ldflags "-s -w -X main.version=${VERSION}" -o /out/ttt2-server ./cmd/ttt2-server

FROM gcr.io/distroless/static-debian12:nonroot
COPY --from=build /out/ttt2-server /ttt2-server
COPY THIRD_PARTY_NOTICES.txt /THIRD_PARTY_NOTICES.txt
EXPOSE 28200/tcp 28200/udp
VOLUME ["/srv/ttt2"]
ENTRYPOINT ["/ttt2-server", "-config", "/srv/ttt2/server.toml"]
