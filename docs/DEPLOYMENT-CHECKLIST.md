# Field Deployment Checklist

## Overview

This document provides a comprehensive checklist for deploying the L2 bridge in production environments. Follow these steps to ensure a smooth deployment and avoid common pitfalls.

**Target Environment:** i.MX8MM + NXP88W9098 wireless module
**Network:** VLAN 110 (OHT Network)
**Last Updated:** 2024-12-23

---

## Pre-Deployment Preparation

### 1. Build Verification

- [ ] **Clean build successful**
  ```bash
  cd /path/to/wlan-bridge/dumb
  make clean
  make
  ```
  - Expected: No warnings or errors
  - Files created: `dumb`, `dumb-tpacket`

- [ ] **Code version identified**
  ```bash
  git log -1 --oneline
  git status
  ```
  - Record commit hash for tracking
  - Ensure no uncommitted changes (or document them)

- [ ] **Binary copied to target device**
  ```bash
  scp dumb root@<target-ip>:/usr/local/bin/
  # OR use provided script
  ./scripts/scp_to_target.sh
  ```

### 2. Network Environment Verification

- [ ] **VLAN configuration confirmed**

  **Expected configuration (OHT Network):**
  - VLAN ID: **110**
  - OHT (wired): 192.168.10.11~240/23
  - Bridge (wireless): 192.168.11.11~240/23
  - Maint PCs: 192.168.10.241~254/23
  - Gateway: 192.168.10.1

  ```bash
  # Verify network configuration with network admin
  # Document any differences
  ```

- [ ] **VLAN tags verified on wireless interface**
  ```bash
  # On target device (bridge NOT running)
  sudo tcpdump -i mlan0 -e -n -c 10 -vv | grep -i vlan
  ```

  **Expected output:**
  ```
  ethertype 802.1Q (0x8100), vlan 110, p 0, ethertype IPv4 (0x0800), ...
  ```

  **If no VLAN tags found:**
  - VLAN parsing not required for current setup
  - IP filter may work without VLAN support
  - Document this finding

- [ ] **Interface names confirmed**
  ```bash
  ip link show
  ```
  - Wired interface: `eth0` or `__________`
  - Wireless interface: `mlan0` or `__________`
  - Document actual names if different

- [ ] **IP addresses assigned**
  ```bash
  ip addr show eth0
  ip addr show mlan0
  ```
  - eth0 IP: __________ (may be none or different subnet)
  - mlan0 IP: 192.168.11.__ (within 192.168.11.11~240)
  - Record actual assignments

### 3. System Requirements Check

- [ ] **Kernel version**
  ```bash
  uname -r
  ```
  - Minimum: 4.14 (for i.MX8MM)
  - Recommended: Latest stable for your BSP

- [ ] **Required capabilities**
  ```bash
  # Check if capabilities are supported
  getcap /usr/local/bin/dumb
  ```
  - Required: `cap_net_raw`, `cap_net_admin`, `cap_sys_nice`, `cap_ipc_lock`

- [ ] **Libraries installed**
  ```bash
  ldd /usr/local/bin/dumb
  ```
  - libpcap: Must be present
  - pthread: Must be present

- [ ] **Available CPU cores**
  ```bash
  nproc
  ```
  - Minimum: 2 (for dual-threaded bridge)
  - Recommended: 4

- [ ] **Available memory**
  ```bash
  free -h
  ```
  - Minimum: 128 MB free RAM
  - Recommended: 256 MB+

---

## Configuration Decisions

### 4. Filter Settings

Choose appropriate filter settings based on your requirements:

- [ ] **MAC Filter Decision**

  **Enable if:**
  - `pcap_setdirection(PCAP_D_IN)` fails on your system
  - Bridge captures its own transmitted packets (causes loops)

  **Verify need:**
  ```bash
  # Run bridge without filter and check for duplicate pings
  sudo ./dumb eth0 mlan0 &
  ping 8.8.8.8
  # If you see "DUP!" → MAC filter needed
  killall dumb
  ```

  **Decision:** Enable MAC filter? ☐ Yes ☐ No

- [ ] **IP Filter Decision**

  **Enable if:**
  - Bridge has an IP address (e.g., 192.168.11.11)
  - You want to prevent bridge from re-injecting packets destined for itself
  - VLAN tags are present (IP filter now supports VLAN parsing)

  **Verify need:**
  ```bash
  # Run bridge and try to ping bridge IP from wired side
  sudo ./dumb eth0 mlan0 &
  ping 192.168.11.11  # Bridge wireless IP
  # If ping works → IP filter recommended
  killall dumb
  ```

  **Decision:** Enable IP filter? ☐ Yes ☐ No

- [ ] **Debug Logging Decision**

  **Enable for:**
  - Initial deployment and testing
  - Troubleshooting issues

  **Disable for:**
  - Production long-term operation (reduces CPU/syslog overhead)

  **Decision:** Enable debug logging? ☐ Yes (testing) ☐ No (production)

### 5. Performance Tuning

- [ ] **Dispatch budget setting**

  **Default:** 64 packets per dispatch

  **Adjust if:**
  - High latency/jitter → decrease (e.g., 32)
  - High CPU usage → increase (e.g., 128)
  - Packet drops (PcapDrop) → increase

  **Decision:** Use default (64)? ☐ Yes ☐ No → Set to: ______

- [ ] **Real-time scheduling**

  **Default:** Enabled (SCHED_FIFO priority 50)

  **Disable if:**
  - System becomes unresponsive during bridge operation
  - Other real-time tasks need priority

  **Decision:** Enable RT? ☐ Yes ☐ No

- [ ] **CPU affinity**

  **Default:** Enabled (Thread 0→CPU 0, Thread 1→CPU 1)

  **Disable if:**
  - Only 1 CPU core available
  - Other processes need specific CPU cores

  **Decision:** Enable affinity? ☐ Yes ☐ No

---

## Initial Testing (Non-Production)

### 6. Basic Functionality Tests

- [ ] **Test 1: Bridge starts successfully**
  ```bash
  sudo ./dumb eth0 mlan0
  ```
  - Expected: No error messages
  - Check output for "both interfaces ready, starting packet forwarding"

- [ ] **Test 2: Ping test (wired → internet)**
  ```bash
  # From OHT or wired device
  ping -c 10 8.8.8.8
  ```
  - Expected: 0% packet loss
  - Latency: <10 ms (typical for wireless)

- [ ] **Test 3: Ping test (wireless → internet)**
  ```bash
  # From maintenance PC or wireless client
  ping -c 10 8.8.8.8
  ```
  - Expected: 0% packet loss

- [ ] **Test 4: Statistics verification**
  ```bash
  # In another terminal
  kill -USR1 $(pidof dumb)
  ```
  - Check output for:
    - RX packets increasing on both interfaces
    - TX packets increasing on both interfaces
    - Dropped = 0 (or very low)
    - PcapDrop = 0

- [ ] **Test 5: Bidirectional traffic**
  ```bash
  # From wired device, ping wireless device
  ping <wireless-client-ip>

  # From wireless device, ping wired device
  ping <wired-device-ip>
  ```
  - Both directions should work

### 7. Filter Functionality Tests

**If IP filter enabled:**

- [ ] **Test 6: Bridge IP filtering (should be blocked)**
  ```bash
  # From wired device
  ping 192.168.11.11  # Bridge wireless IP
  ```
  - Expected: No response (filtered)
  - Check syslog: `grep "ip-filter: skipped" /var/log/syslog`

- [ ] **Test 7: Other IPs (should work)**
  ```bash
  # From wired device
  ping 8.8.8.8  # Internet
  ping 192.168.10.241  # Maint PC
  ```
  - Expected: Normal responses

**If MAC filter enabled:**

- [ ] **Test 8: Self-loop prevention**
  ```bash
  # Monitor for "mac-filter: skipped" in syslog
  sudo tail -f /var/log/syslog | grep mac-filter
  ```
  - Should appear if bridge is capturing its own packets

### 8. VLAN Verification

- [ ] **Test 9: VLAN tag handling**
  ```bash
  # Capture on eth0 while bridge is running
  sudo tcpdump -i eth0 -e -n -c 10 -vv
  ```
  - Check if VLAN tags are preserved in forwarded packets
  - Document findings

- [ ] **Test 10: IP filter with VLAN**

  If VLAN 110 tags are present on mlan0:
  ```bash
  # IP filter should still work
  ping 192.168.11.11  # Should be filtered
  ```
  - Verify filtering works despite VLAN tags

### 9. Performance Tests

- [ ] **Test 11: Throughput test (TCP)**
  ```bash
  # On server (wired or behind wired)
  iperf3 -s

  # On client (wireless or behind wireless)
  iperf3 -c <server-ip> -t 60
  ```
  - Expected: >100 Mbps (depends on wireless conditions)
  - Check bridge statistics during test:
    ```bash
    watch -n 1 "kill -USR1 \$(pidof dumb)"
    ```
  - Dropped should be low (<1%)

- [ ] **Test 12: Throughput test (UDP)**
  ```bash
  # Server
  iperf3 -s

  # Client
  iperf3 -c <server-ip> -u -b 400M -t 60
  ```
  - Monitor for packet loss
  - Check for ENOBUFS errors in syslog

- [ ] **Test 13: Latency test**
  ```bash
  ping -c 100 -i 0.2 <target-ip>
  ```
  - Check min/avg/max/mdev
  - Typical: avg <5ms, mdev <2ms

- [ ] **Test 14: Long-duration stability**
  ```bash
  # Run bridge for at least 1 hour
  sudo ./dumb [options] eth0 mlan0

  # Generate continuous traffic
  iperf3 -c <server> -t 3600

  # Monitor statistics
  watch -n 5 "kill -USR1 \$(pidof dumb)"
  ```
  - Check for:
    - Memory leaks (monitor with `top`)
    - CPU stability
    - Packet drop rate
    - Error counters

---

## Production Deployment

### 10. Final Configuration

- [ ] **Command line finalized**

  Based on tests above, decide on final command:

  **Example configurations:**

  ```bash
  # Minimal (no filters, default settings)
  sudo ./dumb --no-debug eth0 mlan0

  # Standard (IP+MAC filters, no debug)
  sudo ./dumb --ip-filter --mac-filter --no-debug eth0 mlan0

  # High performance (larger buffer, higher budget)
  sudo ./dumb --ip-filter --mac-filter --no-debug \
    --dispatch-budget 128 --pcap-buffer 8388608 eth0 mlan0

  # Low latency (smaller budget)
  sudo ./dumb --ip-filter --mac-filter --no-debug \
    --dispatch-budget 32 eth0 mlan0
  ```

  **Your configuration:**
  ```bash
  sudo ./dumb ________________________________
  ```

- [ ] **Systemd service configuration (optional)**

  If using systemd for automatic startup:
  ```bash
  # Copy service template
  sudo cp wifi_bridge@.service /etc/systemd/system/

  # Edit configuration
  sudo nano /etc/systemd/system/wifi_bridge@.service

  # Enable service
  sudo systemctl enable wifi_bridge@eth0-mlan0.service
  sudo systemctl start wifi_bridge@eth0-mlan0.service

  # Check status
  sudo systemctl status wifi_bridge@eth0-mlan0.service
  ```

- [ ] **Monitoring setup**

  **Syslog configuration:**
  ```bash
  # Create dedicated log file (optional)
  sudo nano /etc/rsyslog.d/50-dumb-bridge.conf
  ```
  Add:
  ```
  :programname, isequal, "dumb-bridge" /var/log/dumb-bridge.log
  & stop
  ```
  ```bash
  sudo systemctl restart rsyslog
  ```

  **Monitoring script:**
  ```bash
  # Create monitoring script
  cat > /usr/local/bin/monitor-bridge.sh << 'EOF'
  #!/bin/bash
  while true; do
    kill -USR1 $(pidof dumb) 2>/dev/null
    sleep 60
  done
  EOF
  chmod +x /usr/local/bin/monitor-bridge.sh
  ```

### 11. Production Verification

- [ ] **Verify bridge running**
  ```bash
  ps aux | grep dumb
  pgrep -a dumb
  ```

- [ ] **Verify no errors in syslog**
  ```bash
  sudo tail -n 50 /var/log/syslog | grep dumb-bridge
  ```
  - Should not have ERROR or WARN messages

- [ ] **Production traffic test**

  Have actual OHT devices communicate through bridge:
  - Verify normal operation
  - Monitor statistics for any anomalies

- [ ] **Failover test (optional but recommended)**
  ```bash
  # Kill bridge process
  sudo killall dumb

  # Verify OHT detects network loss
  # Restart bridge
  sudo ./dumb [options] eth0 mlan0

  # Verify OHT reconnects
  ```

---

## Post-Deployment Monitoring

### 12. First 24 Hours

- [ ] **Check statistics regularly**
  ```bash
  # Every hour for first 24 hours
  kill -USR1 $(pidof dumb)
  ```
  - Monitor dropped packet rate
  - Monitor PcapDrop (kernel buffer overflows)
  - Ensure error counters stay low

- [ ] **Monitor system resources**
  ```bash
  top -p $(pidof dumb)
  ```
  - CPU usage should be stable
  - Memory usage should not increase over time

- [ ] **Review syslog**
  ```bash
  sudo grep dumb-bridge /var/log/syslog | tail -n 100
  ```
  - Look for patterns of errors
  - Note any ENOBUFS occurrences

### 13. First Week

- [ ] **Weekly statistics review**
  - Collect statistics daily
  - Look for trends (increasing drops, errors, etc.)

- [ ] **Performance validation**
  - Run iperf3 tests weekly
  - Compare with baseline measurements
  - Investigate any degradation

- [ ] **User feedback**
  - Collect feedback from OHT operators
  - Any connectivity issues?
  - Any performance complaints?

---

## Rollback Plan

### 14. Rollback Preparation

- [ ] **Backup of previous version**
  ```bash
  # Before deploying new version
  sudo cp /usr/local/bin/dumb /usr/local/bin/dumb.backup
  ```

- [ ] **Document configuration changes**
  - Record what changed from previous version
  - Keep notes on why changes were made

- [ ] **Rollback procedure**

  If issues occur:
  ```bash
  # Stop current bridge
  sudo killall dumb

  # Restore previous version
  sudo cp /usr/local/bin/dumb.backup /usr/local/bin/dumb

  # Restart with previous configuration
  sudo ./dumb [previous-options] eth0 mlan0

  # Verify operation
  ping 8.8.8.8
  ```

---

## Troubleshooting Guide

### Common Issues and Solutions

**Issue: Bridge fails to start**
```bash
# Check error messages
sudo ./dumb eth0 mlan0

# Common causes:
# - Interface names wrong → check with 'ip link'
# - Insufficient permissions → run with sudo
# - Interfaces not up → 'ip link set eth0 up'
```

**Issue: High packet drop rate**
```bash
# Check statistics
kill -USR1 $(pidof dumb)

# If Dropped counter high:
# - Check wireless signal quality
# - Increase pcap buffer: --pcap-buffer 8388608
# - Try dumb-tpacket version

# If PcapDrop high:
# - CPU too slow → decrease dispatch-budget
# - OR increase pcap buffer
```

**Issue: High latency**
```bash
# Decrease dispatch budget
sudo killall dumb
sudo ./dumb --dispatch-budget 32 eth0 mlan0

# Disable RT if causing system issues
sudo ./dumb --no-rt --no-affinity eth0 mlan0
```

**Issue: IP filter not working**
```bash
# Verify VLAN tags
sudo tcpdump -i mlan0 -e -n -c 5 -vv

# If VLAN tags present, ensure using latest version
# with VLAN parsing support

# Verify bridge IP is set
ip addr show mlan0

# Check syslog for filter messages
sudo grep "ip-filter" /var/log/syslog
```

---

## Sign-Off

### Deployment Information

- **Date:** __________________
- **Deployed by:** __________________
- **Location:** __________________
- **Bridge version:** __________________
- **Git commit:** __________________

### Test Results Summary

- ☐ All pre-deployment tests passed
- ☐ Basic functionality verified
- ☐ Filters working as expected
- ☐ Performance meets requirements
- ☐ No errors in production trial

### Configuration Used

```bash
Command: sudo ./dumb _______________________________
```

### Notes and Observations

```
_________________________________________________________________
_________________________________________________________________
_________________________________________________________________
```

### Approval

- **Network Administrator:** __________________
- **System Engineer:** __________________
- **Date:** __________________

---

**Document maintained for use with AI assistants (Claude, GPT, Gemini, etc.)**
