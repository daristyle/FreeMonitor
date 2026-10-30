#!/usr/bin/env bash
set -euo pipefail

readonly HOST_PORT=50500
readonly DISCOVERY_PORT=50501
readonly BUFFER_BYTES=8388608
readonly SYSCTL_FILE="${FM_SYSCTL_FILE:-/etc/sysctl.d/90-freemonitor.conf}"
readonly PORTS=("$HOST_PORT/udp" "$DISCOVERY_PORT/udp")

usage() {
    echo "Usage: $0 [--remove]" >&2
    exit 2
}

requireRoot() {
    if [[ "$(id -u)" -ne 0 ]]; then
        echo "Run this script as root: sudo $0 $*" >&2
        exit 1
    fi
}

firewallRunning() {
    command -v firewall-cmd >/dev/null && firewall-cmd --state >/dev/null 2>&1
}

activeZones() {
    local zones
    zones="$(firewall-cmd --get-active-zones | awk '!/^[[:space:]]/ {print $1}')"
    if [[ -z "$zones" ]]; then
        zones="$(firewall-cmd --get-default-zone)"
    fi
    echo "$zones"
}

portIsOpen() {
    firewall-cmd --permanent --zone="$1" --query-port="$2" >/dev/null 2>&1
}

openPorts() {
    if ! firewallRunning; then
        echo "firewalld is not running, so no ports were opened."
        return
    fi
    local zone port
    for zone in $(activeZones); do
        for port in "${PORTS[@]}"; do
            if portIsOpen "$zone" "$port"; then
                echo "Port $port is already open in zone $zone."
                continue
            fi
            firewall-cmd --permanent --zone="$zone" --add-port="$port" >/dev/null
            firewall-cmd --zone="$zone" --add-port="$port" >/dev/null
            echo "Opened port $port in zone $zone."
        done
    done
}

closePorts() {
    if ! firewallRunning; then
        echo "firewalld is not running, so no ports were closed."
        return
    fi
    local zone port
    for zone in $(firewall-cmd --get-zones); do
        for port in "${PORTS[@]}"; do
            if portIsOpen "$zone" "$port"; then
                firewall-cmd --permanent --zone="$zone" --remove-port="$port" >/dev/null
                firewall-cmd --zone="$zone" --remove-port="$port" >/dev/null
                echo "Closed port $port in zone $zone."
            fi
        done
    done
}

bufferLimit() {
    local current
    current="$(sysctl -n "$1")"
    echo "$((current > BUFFER_BYTES ? current : BUFFER_BYTES))"
}

raiseBufferLimits() {
    local receiveLimit sendLimit
    receiveLimit="$(bufferLimit net.core.rmem_max)"
    sendLimit="$(bufferLimit net.core.wmem_max)"
    printf 'net.core.rmem_max = %s\nnet.core.wmem_max = %s\n' "$receiveLimit" "$sendLimit" >"$SYSCTL_FILE"
    sysctl -q -p "$SYSCTL_FILE"
    echo "Set net.core.rmem_max to $receiveLimit and net.core.wmem_max to $sendLimit in $SYSCTL_FILE."
}

removeBufferLimits() {
    if [[ -f "$SYSCTL_FILE" ]]; then
        rm "$SYSCTL_FILE"
        echo "Removed $SYSCTL_FILE. The buffer limits return to their defaults after a reboot."
    fi
}

main() {
    if [[ $# -gt 1 ]]; then
        usage
    fi
    case "${1:-}" in
        "")
            requireRoot "$@"
            openPorts
            raiseBufferLimits
            ;;
        --remove)
            requireRoot "$@"
            closePorts
            removeBufferLimits
            ;;
        *)
            usage
            ;;
    esac
}

main "$@"
