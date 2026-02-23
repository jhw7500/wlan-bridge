#!/bin/bash
set -euo pipefail

TAG=$(basename "$0")
IFACE="${1:-mlan0}"
SVC="wifi_bridge@${IFACE}.service"
DEFAULT_CFG="/etc/default/wbridge"
BACKUP_CFG="/tmp/wbridge.default.$$.bak"
THERMAL_TIMER="wbridge-thermal-state.timer"
THERMAL_SERVICE="wbridge-thermal-state.service"

PASS_COUNT=0
FAIL_COUNT=0

log_info() {
    echo "[INFO] $*"
}

log_pass() {
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "[PASS] $*"
}

log_fail() {
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "[FAIL] $*"
}

require_root() {
    if [ "${EUID}" -ne 0 ]; then
        echo "This script must run as root" >&2
        exit 1
    fi
}

set_kv() {
    local key="$1"
    local value="$2"
    if grep -q "^${key}=" "$DEFAULT_CFG"; then
        sed -i "s#^${key}=.*#${key}=${value}#" "$DEFAULT_CFG"
    else
        echo "${key}=${value}" >> "$DEFAULT_CFG"
    fi
}

json_get() {
    local file="$1"
    local key="$2"

    if [ ! -f "$file" ]; then
        echo ""
        return 0
    fi

    if command -v jq >/dev/null 2>&1; then
        jq -r ".${key} // \"\"" "$file" 2>/dev/null || echo ""
    else
        grep -E "\"${key}\"" "$file" | head -1 | sed -E 's/.*: "?([^",}]*)"?,?$/\1/'
    fi
}

expect_equal() {
    local desc="$1"
    local got="$2"
    local expected="$3"
    if [ "$got" = "$expected" ]; then
        log_pass "$desc (expected=$expected)"
    else
        log_fail "$desc (expected=$expected, got=$got)"
    fi
}

expect_not_equal() {
    local desc="$1"
    local got="$2"
    local not_expected="$3"
    if [ "$got" != "$not_expected" ]; then
        log_pass "$desc (not_expected=$not_expected, got=$got)"
    else
        log_fail "$desc (unexpected=$not_expected)"
    fi
}

expect_contains() {
    local desc="$1"
    local text="$2"
    local needle="$3"
    if printf '%s' "$text" | grep -q "$needle"; then
        log_pass "$desc"
    else
        log_fail "$desc (missing: $needle)"
    fi
}

expect_file_key() {
    local desc="$1"
    local file="$2"
    local key="$3"
    if [ -f "$file" ] && grep -q "^${key}=" "$file"; then
        log_pass "$desc"
    else
        log_fail "$desc (missing ${key} in ${file})"
    fi
}

service_restart() {
    if ! systemctl daemon-reload >/dev/null 2>&1; then
        log_fail "systemctl daemon-reload failed"
        return 1
    fi
    if ! systemctl restart "$SVC" >/dev/null 2>&1; then
        log_fail "systemctl restart $SVC failed"
        return 1
    fi
    sleep 2
    return 0
}

get_cmdline() {
    local pid
    pid=$(systemctl show -p MainPID --value "$SVC" 2>/dev/null || echo 0)
    if [ -z "$pid" ] || [ "$pid" = "0" ] || [ ! -r "/proc/${pid}/cmdline" ]; then
        echo ""
        return 0
    fi
    tr '\0' ' ' < "/proc/${pid}/cmdline"
}

run_pcap_case() {
    log_info "Running case: pcap_normal"
    set_kv WBRIDGE_ENGINE pcap
    set_kv WBRIDGE_OPTIMIZE 1
    set_kv WBRIDGE_MODE normal
    set_kv WBRIDGE_THERMAL_STATE ok
    set_kv WBRIDGE_MODE_FORCE 0

    service_restart || return

    local cmdline engine effective
    cmdline=$(get_cmdline)
    engine=$(json_get /run/wbridge.apply.json engine)
    effective=$(json_get /run/wbridge.effective.json profile_effective)

    expect_equal "pcap engine snapshot" "$engine" "pcap"
    expect_contains "pcap cmdline uses wifi-wbridge" "$cmdline" "wifi-wbridge"
    expect_equal "pcap effective mode" "$effective" "normal"
}

run_tpacket_case() {
    log_info "Running case: tpacket_normal"
    set_kv WBRIDGE_ENGINE tpacket
    set_kv WBRIDGE_OPTIMIZE 1
    set_kv WBRIDGE_MODE normal
    set_kv WBRIDGE_THERMAL_STATE ok
    set_kv WBRIDGE_MODE_FORCE 0

    service_restart || return

    local cmdline engine effective
    cmdline=$(get_cmdline)
    engine=$(json_get /run/wbridge.apply.json engine)
    effective=$(json_get /run/wbridge.effective.json profile_effective)

    expect_equal "tpacket engine snapshot" "$engine" "tpacket"
    expect_contains "tpacket cmdline uses wifi-wbridge-tpacket" "$cmdline" "wifi-wbridge-tpacket"
    expect_equal "tpacket effective mode" "$effective" "normal"
}

run_thermal_clamp_case() {
    log_info "Running case: thermal_clamp"
    set_kv WBRIDGE_ENGINE pcap
    set_kv WBRIDGE_OPTIMIZE 1
    set_kv WBRIDGE_MODE latency
    set_kv WBRIDGE_THERMAL_STATE hot
    set_kv WBRIDGE_MODE_FORCE 0

    service_restart || return

    local effective udp_result
    effective=$(json_get /run/wbridge.effective.json profile_effective)
    udp_result=$(json_get /run/wbridge.apply.json udp_optimization)

    expect_equal "thermal clamp effective mode" "$effective" "thermal"
    expect_equal "thermal clamp udp optimization skipped" "$udp_result" "skipped_thermal"
}

run_force_override_case() {
    log_info "Running case: force_override"
    set_kv WBRIDGE_ENGINE pcap
    set_kv WBRIDGE_OPTIMIZE 1
    set_kv WBRIDGE_MODE latency
    set_kv WBRIDGE_THERMAL_STATE hot
    set_kv WBRIDGE_MODE_FORCE 1

    service_restart || return

    local effective udp_result
    effective=$(json_get /run/wbridge.effective.json profile_effective)
    udp_result=$(json_get /run/wbridge.apply.json udp_optimization)

    expect_equal "force override effective mode" "$effective" "latency"
    expect_not_equal "force override udp optimization not skipped_thermal" "$udp_result" "skipped_thermal"
}

run_thermal_timer_case() {
    log_info "Running case: thermal_timer"
    if systemctl enable --now "$THERMAL_TIMER" >/dev/null 2>&1; then
        log_pass "enable thermal timer"
    else
        log_fail "enable thermal timer"
    fi

    if systemctl start "$THERMAL_SERVICE" >/dev/null 2>&1; then
        log_pass "start thermal updater service"
    else
        log_fail "start thermal updater service"
    fi

    if systemctl is-active --quiet "$THERMAL_TIMER"; then
        log_pass "thermal timer active"
    else
        log_fail "thermal timer inactive"
    fi

    expect_file_key "thermal env has state key" "/run/wbridge.thermal.env" "WBRIDGE_THERMAL_STATE"
}

run_sysctl_case() {
    log_info "Running case: sysctl_values"
    local budget backlog
    budget=$(sysctl -n net.core.netdev_budget 2>/dev/null || echo "")
    backlog=$(sysctl -n net.core.netdev_max_backlog 2>/dev/null || echo "")

    expect_equal "netdev_budget baseline" "$budget" "600"

    case "$backlog" in
        2000|10000)
            log_pass "netdev_max_backlog acceptable ($backlog; 10000 means udp optimization path)"
            ;;
        *)
            log_fail "netdev_max_backlog unexpected ($backlog)"
            ;;
    esac
}

cleanup() {
    if [ -f "$BACKUP_CFG" ]; then
        cp "$BACKUP_CFG" "$DEFAULT_CFG"
        rm -f "$BACKUP_CFG"
    fi

    systemctl daemon-reload >/dev/null 2>&1 || true

    if [ "${INITIAL_ACTIVE:-unknown}" = "active" ]; then
        systemctl restart "$SVC" >/dev/null 2>&1 || true
    else
        systemctl stop "$SVC" >/dev/null 2>&1 || true
    fi
}

main() {
    require_root

    if [ ! -f "$DEFAULT_CFG" ]; then
        echo "Missing $DEFAULT_CFG" >&2
        exit 1
    fi

    cp "$DEFAULT_CFG" "$BACKUP_CFG"
    INITIAL_ACTIVE=$(systemctl is-active "$SVC" 2>/dev/null || true)
    trap cleanup EXIT

    log_info "Starting wbridge smoke tests for $SVC"

    run_pcap_case
    run_tpacket_case
    run_thermal_clamp_case
    run_force_override_case
    run_thermal_timer_case
    run_sysctl_case

    echo
    echo "=== SUMMARY ==="
    echo "PASS: $PASS_COUNT"
    echo "FAIL: $FAIL_COUNT"

    if [ "$FAIL_COUNT" -gt 0 ]; then
        exit 1
    fi
}

main "$@"
