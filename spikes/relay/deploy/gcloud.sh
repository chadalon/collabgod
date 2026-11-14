#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Put the relay spike on a small Google Cloud VM behind Caddy, for the tests
# between two homes. Throwaway tooling for Phase 0: the production relay's
# hosting is issue #37.
#
#   deploy/gcloud.sh up     [--branch NAME] [--hostname NAME]   create the VM and wait until it answers
#   deploy/gcloud.sh update [--branch NAME] [--hostname NAME]   switch branch or name, reboot, wait
#   deploy/gcloud.sh status                                     show the VM and check /healthz
#   deploy/gcloud.sh logs                                       setup and relay output from the serial console
#   deploy/gcloud.sh down                                       delete the VM and its firewall rule
#
# Needs gcloud, logged in, with a project selected that has billing enabled
# (`gcloud config set project ID`). Google Cloud Shell has all of that.
# --branch defaults to the branch checked out here; it must be pushed.
# --hostname defaults to <ip-with-dashes>.sslip.io, which needs no DNS.
# RELAY_NAME, RELAY_ZONE and RELAY_MACHINE override the defaults below.

set -euo pipefail

NAME=${RELAY_NAME:-collabgod-relay}
ZONE=${RELAY_ZONE:-us-east1-b}
MACHINE=${RELAY_MACHINE:-e2-micro}
REPO=https://github.com/chadalon/collabgod
TAG=collabgod-relay
FIREWALL=collabgod-relay-web
HERE=$(cd "$(dirname "$0")" && pwd)

usage() {
  sed -n '4,18p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

fail() {
  echo "$*" >&2
  exit 1
}

command=${1:-}
case "$command" in ''|-h|--help|help) usage ;; esac
shift
branch=""
hostname=""
while [ $# -gt 0 ]; do
  case "$1" in
    --branch) branch=${2:?--branch needs a value}; shift 2 ;;
    --hostname) hostname=${2:?--hostname needs a value}; shift 2 ;;
    -h|--help) usage ;;
    *) fail "Unknown option: $1" ;;
  esac
done

command -v gcloud >/dev/null || fail "gcloud is not installed. Install the Google Cloud CLI, or run this in Cloud Shell."
project=$(gcloud config get-value project 2>/dev/null | tr -d '\r')
[ -n "$project" ] || fail "No project selected. Run: gcloud config set project YOUR_PROJECT_ID"

default_branch() {
  local current
  current=$(git -C "$HERE" rev-parse --abbrev-ref HEAD 2>/dev/null || true)
  if [ -z "$current" ] || [ "$current" = HEAD ]; then current=main; fi
  echo "$current"
}

check_branch_pushed() {
  git ls-remote --exit-code --heads "$REPO" "$1" >/dev/null 2>&1 \
    || fail "Branch '$1' is not on $REPO. Push it first, or pass --branch."
}

# The startup script with any Windows line endings removed, as a path gcloud can read.
startup_script_file() {
  local file
  file=$(mktemp)
  tr -d '\r' < "$HERE/startup.sh" > "$file"
  if command -v cygpath >/dev/null; then cygpath -w "$file"; else echo "$file"; fi
}

instance_exists() {
  gcloud compute instances describe "$NAME" --zone "$ZONE" --format='value(name)' >/dev/null 2>&1
}

external_ip() {
  gcloud compute instances describe "$NAME" --zone "$ZONE" \
    --format='value(networkInterfaces[0].accessConfigs[0].natIP)' | tr -d '\r'
}

metadata_value() {
  gcloud compute instances list --filter="name=($NAME)" --zones "$ZONE" \
    --flatten='metadata.items[]' --format='value(metadata.items.key,metadata.items.value)' \
    | tr -d '\r' | awk -F'\t' -v key="$1" '$1 == key { print $2; exit }'
}

relay_hostname() {
  local name ip
  name=$(metadata_value relay-hostname)
  if [ -n "$name" ]; then echo "$name"; return; fi
  ip=$(external_ip)
  echo "${ip//./-}.sslip.io"
}

wait_healthy() {
  local host=$1 ip
  ip=$(external_ip)
  echo
  echo "Relay URL: wss://$host"
  if [ "${host%.sslip.io}" = "$host" ]; then
    echo "Point a DNS A record for $host at $ip. Caddy keeps retrying its certificate until it resolves."
  fi
  echo "Waiting for https://$host/healthz (the first boot installs Node and Caddy, which takes a few minutes)..."
  for _ in $(seq 1 60); do
    if curl -fsS --max-time 5 "https://$host/healthz" >/dev/null 2>&1; then
      echo "Relay is up: https://$host/healthz answers ok."
      return 0
    fi
    sleep 10
  done
  echo "No answer after 10 minutes. Check DNS for $host, and run: $0 logs" >&2
  return 1
}

up() {
  instance_exists && fail "$NAME already exists in $ZONE. Use '$0 update' or '$0 down'."
  branch=${branch:-$(default_branch)}
  check_branch_pushed "$branch"
  echo "Project $project, zone $ZONE, $MACHINE, branch $branch"

  gcloud services enable compute.googleapis.com
  if ! gcloud compute firewall-rules describe "$FIREWALL" >/dev/null 2>&1; then
    gcloud compute firewall-rules create "$FIREWALL" --network default --direction INGRESS \
      --allow tcp:80,tcp:443 --target-tags "$TAG" \
      --description "HTTP for certificates and HTTPS/WSS for the CollabGod relay spike"
  fi

  local metadata="relay-repo=$REPO,relay-branch=$branch"
  if [ -n "$hostname" ]; then metadata="$metadata,relay-hostname=$hostname"; fi
  gcloud compute instances create "$NAME" --zone "$ZONE" --machine-type "$MACHINE" \
    --image-family debian-12 --image-project debian-cloud \
    --boot-disk-size 10GB --boot-disk-type pd-standard \
    --tags "$TAG" --metadata "$metadata" \
    --metadata-from-file "startup-script=$(startup_script_file)"

  wait_healthy "$(relay_hostname)"
}

update() {
  instance_exists || fail "$NAME does not exist in $ZONE. Use '$0 up'."
  branch=${branch:-$(default_branch)}
  check_branch_pushed "$branch"
  local metadata="relay-branch=$branch"
  if [ -n "$hostname" ]; then metadata="$metadata,relay-hostname=$hostname"; fi
  gcloud compute instances add-metadata "$NAME" --zone "$ZONE" --metadata "$metadata" \
    --metadata-from-file "startup-script=$(startup_script_file)"
  echo "Rebooting $NAME onto branch $branch. Lobbies in memory are lost."
  gcloud compute instances reset "$NAME" --zone "$ZONE"
  sleep 20
  wait_healthy "$(relay_hostname)"
}

status() {
  instance_exists || { echo "$NAME does not exist in $ZONE (project $project)."; return 0; }
  gcloud compute instances describe "$NAME" --zone "$ZONE" \
    --format='table(name,status,machineType.basename(),networkInterfaces[0].accessConfigs[0].natIP:label=IP)'
  echo "Branch: $(metadata_value relay-branch)"
  local host
  host=$(relay_hostname)
  echo "Relay URL: wss://$host"
  if curl -fsS --max-time 5 "https://$host/healthz" >/dev/null 2>&1; then
    echo "Health: ok"
  else
    echo "Health: no answer from https://$host/healthz"
  fi
}

logs() {
  instance_exists || fail "$NAME does not exist in $ZONE."
  # The startup script's lines carry a startup-script prefix; the relay's own lines start with a timestamp.
  gcloud compute instances get-serial-port-output "$NAME" --zone "$ZONE" 2>/dev/null | tr -d '\r' | tail -n 150
}

down() {
  if instance_exists; then
    gcloud compute instances delete "$NAME" --zone "$ZONE" --quiet
  fi
  if gcloud compute firewall-rules describe "$FIREWALL" >/dev/null 2>&1; then
    gcloud compute firewall-rules delete "$FIREWALL" --quiet
  fi
  echo "Deleted. If you pointed a DNS name at the VM, that record now points nowhere."
}

case "$command" in
  up) up ;;
  update) update ;;
  status) status ;;
  logs) logs ;;
  down) down ;;
  ''|-h|--help|help) usage ;;
  *) echo "Unknown command: $command" >&2; usage 1 ;;
esac
