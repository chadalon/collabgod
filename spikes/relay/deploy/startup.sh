#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Startup script for the relay VM that gcloud.sh creates. Google Compute Engine
# runs it as root at every boot, so it is safe to run again: it installs what is
# missing, checks out the branch named in the instance metadata, and restarts
# the relay behind Caddy.
#
# Metadata it reads (gcloud.sh sets them):
#   relay-repo      Git URL to clone (default: the public CollabGod repository)
#   relay-branch    Branch to run (default: main)
#   relay-hostname  Name Caddy gets a certificate for (default: <ip-with-dashes>.sslip.io)

set -euo pipefail

NODE_MAJOR=22
APP_DIR=/opt/collabgod
RELAY_HOME=/var/lib/collabgod-relay
RELAY_USER=relay

metadata() {
  curl -fsS -H 'Metadata-Flavor: Google' "http://metadata.google.internal/computeMetadata/v1/$1"
}

attribute() {
  metadata "instance/attributes/$1" 2>/dev/null || true
}

as_relay() {
  runuser -u "$RELAY_USER" -- env HOME="$RELAY_HOME" PATH=/usr/local/bin:/usr/bin:/bin "$@"
}

repo=$(attribute relay-repo)
repo=${repo:-https://github.com/chadalon/collabgod}
branch=$(attribute relay-branch)
branch=${branch:-main}
hostname=$(attribute relay-hostname)
if [ -z "$hostname" ]; then
  ip=$(metadata instance/network-interfaces/0/access-configs/0/external-ip)
  hostname="${ip//./-}.sslip.io"
fi

echo "collabgod-relay: setting up branch $branch for $hostname"

# Caddy and Git come from Debian 12. Caddy obtains and renews the certificate by itself.
if ! command -v caddy >/dev/null || ! command -v git >/dev/null; then
  apt-get update
  DEBIAN_FRONTEND=noninteractive apt-get install -y caddy git curl ca-certificates xz-utils
fi

# Node from nodejs.org, checked against the published checksum list.
if ! /usr/local/bin/node --version 2>/dev/null | grep -q "^v$NODE_MAJOR\."; then
  base="https://nodejs.org/dist/latest-v$NODE_MAJOR.x"
  tmp=$(mktemp -d)
  curl -fsSL "$base/SHASUMS256.txt" -o "$tmp/SHASUMS256.txt"
  tarball=$(grep -o "node-v[0-9.]*-linux-x64\.tar\.xz" "$tmp/SHASUMS256.txt" | head -n 1)
  curl -fsSL "$base/$tarball" -o "$tmp/$tarball"
  (cd "$tmp" && grep " $tarball\$" SHASUMS256.txt | sha256sum -c -)
  tar -xJf "$tmp/$tarball" -C /usr/local --strip-components=1 \
    --exclude=CHANGELOG.md --exclude=LICENSE --exclude=README.md
  rm -rf "$tmp"
fi

# The relay runs as its own unprivileged user and owns its checkout.
if ! id "$RELAY_USER" >/dev/null 2>&1; then
  useradd --system --create-home --home-dir "$RELAY_HOME" --shell /usr/sbin/nologin "$RELAY_USER"
fi
install -d -o "$RELAY_USER" -g "$RELAY_USER" "$APP_DIR"
if [ ! -d "$APP_DIR/.git" ]; then
  as_relay git clone "$repo" "$APP_DIR"
fi
as_relay git -C "$APP_DIR" remote set-url origin "$repo"
as_relay git -C "$APP_DIR" fetch --prune origin
as_relay git -C "$APP_DIR" checkout --force -B "$branch" "origin/$branch"
commit=$(as_relay git -C "$APP_DIR" rev-parse --short HEAD)

# The relay needs only its runtime dependencies (tsx and ws).
(cd "$APP_DIR/spikes/relay" && as_relay npm ci --omit=dev --no-audit --no-fund)

# Logs go to the journal and the serial console, so `gcloud.sh logs` can read them without SSH.
cat > /etc/systemd/system/collabgod-relay.service <<EOF
[Unit]
Description=CollabGod relay spike
After=network-online.target
Wants=network-online.target

[Service]
User=$RELAY_USER
Environment=HOME=$RELAY_HOME
WorkingDirectory=$APP_DIR/spikes/relay
ExecStart=$APP_DIR/spikes/relay/node_modules/.bin/tsx src/cli.ts serve --host 127.0.0.1 --port 8080
Restart=on-failure
RestartSec=2
StandardOutput=journal+console
StandardError=journal+console
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=$RELAY_HOME

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/caddy/Caddyfile <<EOF
$hostname {
	reverse_proxy 127.0.0.1:8080
}
EOF

systemctl daemon-reload
systemctl enable collabgod-relay caddy
systemctl restart collabgod-relay
systemctl reload-or-restart caddy

echo "collabgod-relay: ready at wss://$hostname (branch $branch, commit $commit)"
