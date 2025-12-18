# 데몬 실행 오류 수정

**문제**: `FATAL: pthread_create(0) failed`
**원인**: systemd 메모리 잠금 제한 (기본 64KB)

---

## 🔍 문제 분석

```
Memory locked to prevent page faults  ← mlockall() 성공
FATAL: pthread_create(0) failed       ← 하지만 스레드 생성 실패
```

### 원인
1. `mlockall(MCL_CURRENT | MCL_FUTURE)` 성공
2. 하지만 systemd 기본 `LimitMEMLOCK=64KB`로 제한됨
3. `pthread_create()`가 스레드 스택(보통 8MB)을 할당하려 할 때 **한도 초과**

---

## ✅ 해결 방법 (2가지)

### 방법 1: systemd 서비스 파일 수정 (권장)

**장점**: 코드 수정 없음
**단점**: 시스템 설정 변경 필요

#### wifi_bridge@.service 수정

```ini
[Service]
Type=simple
ExecStart=/usr/local/scripts/wifi_bridge.sh %i
ExecStop=/usr/local/scripts/wifi_bridge_stop.sh %i
ExecStopPost=/bin/sleep 1
Restart=always
RestartSec=1s

# Capabilities for dumb bridge (mlockall + RT scheduling + network)
AmbientCapabilities=CAP_NET_RAW CAP_NET_ADMIN CAP_SYS_NICE CAP_IPC_LOCK
CapabilityBoundingSet=CAP_NET_RAW CAP_NET_ADMIN CAP_SYS_NICE CAP_IPC_LOCK
NoNewPrivileges=yes

# Resource limits for mlockall() and RT scheduling
LimitMEMLOCK=infinity
LimitSTACK=16777216
LimitRTPRIO=50
```

**추가된 설정**:
- `CAP_SYS_NICE`: RT 스케줄링 허용
- `CAP_IPC_LOCK`: mlockall() 허용
- `LimitMEMLOCK=infinity`: 메모리 잠금 무제한
- `LimitSTACK=16777216`: 스택 16MB (스레드당 8MB × 2)
- `LimitRTPRIO=50`: RT 우선순위 50

#### 적용 방법

```bash
# 1. 서비스 파일 복사
sudo cp wifi_bridge@.service /etc/systemd/system/

# 2. systemd 리로드
sudo systemctl daemon-reload

# 3. 서비스 재시작
sudo systemctl restart wifi_bridge@wlan0

# 4. 상태 확인
sudo systemctl status wifi_bridge@wlan0
sudo journalctl -u wifi_bridge@wlan0 -f
```

---

### 방법 2: 코드 수정 (더 안전함)

**장점**: systemd 설정 변경 불필요, 더 안전
**단점**: 코드 재빌드 필요

#### 변경 사항

**mlockall() 호출 시점을 스레드 생성 후로 이동**:

```diff
  // 시그널 핸들러 등록
  signal(SIGINT, sighandler);
  signal(SIGTERM, sighandler);

- // 메모리 잠금: 페이지 폴트 방지
- if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
-     fprintf(stderr, "WARNING: mlockall() failed...\n");
- }

  atomic_init(&ifs.ready, 0);
  ...

  // 스레드 생성
  for (int i = 0; i < 2; ++i) {
      pthread_create(&ifs.threads[i], NULL, thr, ...);
  }

+ // 스레드 생성 후 메모리 잠금 (더 안전함)
+ if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
+     fprintf(stderr, "WARNING: mlockall() failed...\n");
+ }
```

**이점**:
- pthread_create()가 스택을 할당할 때는 메모리 잠금 미적용
- 스레드 생성 성공 보장
- 이후 mlockall() 실패해도 프로그램 동작 (경고만)

#### 적용 방법

```bash
# 1. 코드 이미 수정됨 (dumb-daemon-fix.c)
cp dumb-daemon-fix.c dumb.c

# 2. 재빌드
make clean && make

# 3. 바이너리 배포
sudo cp dumb /usr/local/bin/

# 4. 서비스 재시작
sudo systemctl restart wifi_bridge@wlan0
```

---

## 🎯 권장 조합

**가장 안전한 방법**: 두 가지 모두 적용

1. ✅ **코드 수정** (mlockall 이동) - 기본 동작 보장
2. ✅ **systemd 설정** - 최적 성능

이렇게 하면:
- 코드 수정으로 pthread_create() 성공 보장
- systemd 설정으로 mlockall() + RT 스케줄링 허용
- 최상의 성능과 안정성

---

## 📊 비교

| 항목 | 방법 1 (systemd만) | 방법 2 (코드만) | 조합 (권장) |
|------|------------------|---------------|-----------|
| **pthread_create()** | ⚠️ 설정 필요 | ✅ 항상 성공 | ✅ 항상 성공 |
| **mlockall()** | ✅ 성공 | ⚠️ 제한적 | ✅ 성공 |
| **RT 스케줄링** | ✅ 가능 | ⚠️ 제한적 | ✅ 가능 |
| **코드 변경** | 불필요 | 필요 | 필요 |
| **시스템 설정** | 필요 | 불필요 | 필요 |

---

## 🧪 검증 방법

### 1. 서비스 로그 확인

```bash
sudo journalctl -u wifi_bridge@wlan0 -f
```

**성공 시 출력**:
```
Thread 0 pinned to CPU 0
Thread 0 set to SCHED_FIFO priority 50
Thread 1 pinned to CPU 1
Thread 1 set to SCHED_FIFO priority 50
Memory locked to prevent page faults    ← 이제 스레드 생성 후!
Thread 0: both interfaces ready, starting packet forwarding
Thread 1: both interfaces ready, starting packet forwarding
Bridge running. Press Ctrl+C to stop.
```

### 2. 메모리 잠금 확인

```bash
PID=$(pgrep dumb)
grep VmLck /proc/$PID/status
```

**출력 예시**:
```
VmLck:      8192 kB    ← 0이 아니면 성공
```

### 3. RT 우선순위 확인

```bash
PID=$(pgrep dumb)
chrt -p $PID
```

**출력 예시**:
```
pid 1234's current scheduling policy: SCHED_FIFO
pid 1234's current scheduling priority: 50
```

---

## 🐛 트러블슈팅

### mlockall() 여전히 실패

```
WARNING: mlockall() failed: Cannot allocate memory
```

**해결**: systemd LimitMEMLOCK 확인
```bash
sudo systemctl show wifi_bridge@wlan0 | grep LimitMEMLOCK
```

### RT 스케줄링 실패

```
WARNING: pthread_setschedparam(0) failed: Operation not permitted
```

**해결**: systemd LimitRTPRIO 확인
```bash
sudo systemctl show wifi_bridge@wlan0 | grep LimitRTPRIO
```

---

## 📁 파일 목록

```
dumb-daemon-fix.c           ← mlockall 이동 버전
wifi_bridge@.service        ← 수정된 systemd 서비스
DAEMON-FIX.md              ← 이 문서
```

---

**작성 일시**: 2025-12-18 13:55
**테스트 환경**: systemd 기반 Linux 시스템
