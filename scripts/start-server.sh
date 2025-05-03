#!/bin/sh
set -eux

# Precheck: Ensure PORT is a number or undefined
if [ -n "${PORT:-}" ]; then
    if ! echo "$PORT" | grep -Eq '^[0-9]+$'; then
        echo "Error: PORT must be a valid number" >&2
        exit 1
    fi
fi

# Wait for avahi-daemon to initialize
while true; do
    if [ -f "/var/run/avahi-daemon/pid" ] || [ -f "/run/avahi-daemon/pid" ]; then
        echo "avahi-daemon is active. Starting ps-printer-app..."
        break
    fi

    echo "Waiting for avahi-daemon to initialize..."
    sleep 1
done

# Start the ps-printer-app server
ps-printer-app -o log-file="/ps-printer-app.log" ${PORT:+-o server-port="$PORT"} server
