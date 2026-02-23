# Wbridge Smoke Test Checklist

This checklist verifies that thermal profile changes from `thermal_email` are applied in real runtime paths for both `pcap` and `tpacket` engines.

## Scope

- Engine switch (`WBRIDGE_ENGINE=pcap|tpacket`)
- Thermal clamp and force override (`WBRIDGE_MODE_FORCE`)
- Thermal updater timer/service behavior
- Sysctl baseline (`netdev_budget`, `netdev_max_backlog`)
- Runtime snapshots (`/run/wbridge.effective.json`, `/run/wbridge.apply.json`)

## Test Script

Run:

```bash
sudo /usr/local/scripts/wbridge_smoke_test.sh mlan0
```

Source copy:

```bash
sudo ./scripts/wbridge_smoke_test.sh mlan0
```

The script prints `[PASS]/[FAIL]` per check and exits non-zero on any failure.

## Manual PASS/FAIL Matrix

| Check ID | Item | PASS Criteria | Evidence |
|---|---|---|---|
| C1 | PCAP engine boot | `wifi_bridge@mlan0` cmdline contains `wifi-wbridge` and apply snapshot `engine=pcap` | `/proc/<pid>/cmdline`, `/run/wbridge.apply.json` |
| C2 | TPACKET engine boot | cmdline contains `wifi-wbridge-tpacket` and apply snapshot `engine=tpacket` | `/proc/<pid>/cmdline`, `/run/wbridge.apply.json` |
| C3 | Thermal clamp | `WBRIDGE_MODE=latency`, `thermal=hot`, `force=0` -> effective becomes `thermal` and `udp_optimization=skipped_thermal` | `/run/wbridge.effective.json`, `/run/wbridge.apply.json` |
| C4 | Force override | `WBRIDGE_MODE=latency`, `thermal=hot`, `force=1` -> effective remains `latency` and UDP optimization is not `skipped_thermal` | `/run/wbridge.effective.json`, `/run/wbridge.apply.json`, journal |
| C5 | Thermal timer | `wbridge-thermal-state.timer` active, updater runs, `/run/wbridge.thermal.env` has `WBRIDGE_THERMAL_STATE=` | `systemctl status`, `/run/wbridge.thermal.env` |
| C6 | Sysctl baseline | `net.core.netdev_budget=600`; `net.core.netdev_max_backlog` is `2000` baseline or `10000` when UDP optimization path has applied | `sysctl -n ...` |

## Quick Validation Commands

```bash
systemctl status wifi_bridge@mlan0 --no-pager
cat /run/wbridge.effective.json
cat /run/wbridge.apply.json
cat /run/wbridge.thermal.env
sysctl -n net.core.netdev_budget net.core.netdev_max_backlog
journalctl -u wifi_bridge@mlan0 -n 120 --no-pager
```

## Notes

- `netdev_max_backlog=10000` can appear after `optimize-for-udp.sh` runs by design.
- `WBRIDGE_MODE_FORCE=1` bypasses thermal clamp; keep it disabled for thermal safety unless explicitly required.
- The smoke script backs up `/etc/default/wbridge` and restores it automatically on exit.
