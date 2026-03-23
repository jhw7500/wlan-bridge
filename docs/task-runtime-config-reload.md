# Task: wbridge 런타임 설정 변경 (SIGHUP reload)

## 목표

thermal state 변화 시 bridge를 재시작하지 않고 설정을 런타임으로 전환한다.

## 현재 구조

```
시작 시 1회 로드:
  config_init_defaults() → config_load_from_env() → config_parse_args()
  → bridge_init() (pcap 핸들 생성)
  → 메인 루프 (config 변경 불가)
```

설정 변경 시 서비스 재시작 필요 → 패킷 손실 발생.

## 설계

### Signal 기반 reload

SIGHUP 수신 시 `/run/wbridge.env` 파일을 다시 읽어 config를 업데이트한다.

### 설정 분류

| 설정 | 런타임 변경 | 이유 |
|------|:-:|------|
| `dispatch_budget` | O | 루프에서 매번 참조, 값만 교체 |
| `enable_debug_log` | O | 로그 플래그만 변경 |
| `enable_mac_filter` | O | 필터 판정에서 참조 |
| `enable_ip_filter` | O | 필터 판정에서 참조 |
| `rt_priority` | O | `sched_setparam()` 호출로 변경 가능 |
| `timeout_ms` | X | pcap 핸들 재생성 필요 |
| `snaplen` | X | pcap 핸들 재생성 필요 |
| `pcap_buffer_bytes` | X | pcap 핸들 재생성 필요 |
| `enable_immediate` | X | pcap 핸들 재생성 필요 |
| `enable_promisc` | X | pcap 핸들 재생성 필요 |

thermal 프로파일 전환에 핵심인 `dispatch_budget`과 `rt_priority`는 런타임 변경 가능.

### 구현 순서

#### 1. main.c — SIGHUP 핸들러 추가

```c
static volatile sig_atomic_t g_reload_requested = 0;

static void sighup_handler(int sig) {
    (void)sig;
    g_reload_requested = 1;
}

// main()에서 등록
signal(SIGHUP, sighup_handler);

// 메인 루프에서 처리
while (atomic_load(&ctx->keep_running)) {
    sleep(1);
    if (g_reload_requested) {
        g_reload_requested = 0;
        config_reload_from_env(&ctx->config);
        // rt_priority 변경 시 sched_setparam 재적용
    }
    if (g_print_stats_requested) {
        g_print_stats_requested = 0;
        stats_report(&ctx->stats);
    }
}
```

#### 2. config.c — reload 함수 추가

```c
void config_reload_from_env(struct bridge_config *cfg) {
    // 런타임 변경 가능한 설정만 업데이트
    cfg->dispatch_budget = clamp_int(
        env_to_int("WBRIDGE_DISPATCH_BUDGET", cfg->dispatch_budget), 1, 4096);
    cfg->enable_debug_log = env_to_int("WBRIDGE_DEBUG", cfg->enable_debug_log) ? 1 : 0;
    cfg->enable_mac_filter = env_to_int("WBRIDGE_MAC_FILTER", cfg->enable_mac_filter) ? 1 : 0;
    cfg->enable_ip_filter = env_to_int("WBRIDGE_IP_FILTER", cfg->enable_ip_filter) ? 1 : 0;

    int new_rt = clamp_int(env_to_int("WBRIDGE_RT_PRIORITY", cfg->rt_priority), 1, 99);
    if (new_rt != cfg->rt_priority) {
        cfg->rt_priority = new_rt;
        // 호출측에서 sched_setparam 재적용 필요
    }

    syslog(LOG_INFO, "Config reloaded: dispatch_budget=%d, rt_priority=%d, debug=%d",
           cfg->dispatch_budget, cfg->rt_priority, cfg->enable_debug_log);
}
```

#### 3. config.h — 함수 선언 추가

```c
void config_reload_from_env(struct bridge_config *cfg);
```

#### 4. bridge.c — rt_priority 재적용 함수

`bridge_init()`에서 `sched_setparam()`을 호출하는 부분을 별도 함수로 분리하여 reload 시 재사용.

```c
int bridge_apply_rt_priority(int priority);
```

#### 5. wbridge_thermal_state_update.sh 수정

bridge 재시작 대신 env 파일 업데이트 + SIGHUP 전송:

```sh
# 기존: systemctl try-restart wifi_bridge@mlan0.service
# 변경:
setup-irq-affinity.sh --mode "$NEW_STATE" ...   # IRQ affinity 재설정
kill -HUP $(cat /run/wbridge.mlan0.pid)          # wbridge에 reload 신호
```

PID 파일은 wifi_bridge.sh에서 `BRIDGE_PID`를 기록하도록 추가.

### thread safety 고려사항

- `bridge_config`는 메인 스레드에서만 쓰고, 워커 스레드에서 읽음
- `dispatch_budget`은 `int` — 워드 단위 읽기이므로 atomic 불필요 (torn read 없음)
- `enable_*` 플래그는 `uint8_t` 비트필드 — 같은 바이트 내 비트필드 동시 쓰기 문제 가능
  - 해결: reload 시 임시 config 구성 후 한번에 복사, 또는 `_Atomic` 사용

### 테스트 방법

```bash
# 1. bridge 실행 중 확인
systemctl status wifi_bridge@mlan0

# 2. env 파일 수정
echo "WBRIDGE_DISPATCH_BUDGET=128" > /run/wbridge.env
echo "WBRIDGE_RT_PRIORITY=80" >> /run/wbridge.env

# 3. SIGHUP 전송
kill -HUP $(pidof wifi-wbridge)

# 4. syslog에서 reload 확인
journalctl -u wifi_bridge@mlan0 --since "10 seconds ago" | grep reload
```

### 관련 파일

| 파일 | 변경 내용 |
|------|----------|
| `wbridge/main.c` | SIGHUP 핸들러, 메인 루프 reload 처리 |
| `wbridge/config.c` | `config_reload_from_env()` 추가 |
| `wbridge/config.h` | 함수 선언 |
| `wbridge/bridge.c` | `bridge_apply_rt_priority()` 분리 |
| `wbridge/bridge_types.h` | 필요 시 atomic 타입 변경 |
| `scripts/wbridge_thermal_state_update.sh` | restart → SIGHUP 전환 |
| `scripts/wifi_bridge.sh` | PID 파일 기록 추가 |
