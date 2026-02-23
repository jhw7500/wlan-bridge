# 드라이버 옵션 참조

NXP 88W9098 (moal/mlan) 및 커널 네트워크 스택 설정

---

## 1. wifi_mod_para.conf (모듈 파라미터)

파일 위치: `/usr/lib/firmware/cts/wifi_mod_para.conf`
적용 시점: 드라이버 로드 시 (`insmod moal.ko mod_para=cts/wifi_mod_para.conf`)

### 발열 관련 파라미터

| 파라미터 | 값 | 설명 |
|---|---|---|
| `pcie_int_mode` | 0 = Legacy (기본), 1 = MSI | MSI 모드가 Legacy보다 인터럽트 처리 효율적 |
| `napi` | 0 = 비활성, 1 = 활성 | NAPI 폴링 활성화. 대량 트래픽 시 인터럽트→폴링 자동 전환 |
| `ps_mode` | 1 = 활성, 2 = 비활성 | 전력 절약 모드 (STA 모드에서 유효) |
| `auto_ds` | 1 = 활성, 2 = 비활성 | 자동 Deep Sleep |

### PCIE9098 적용 예시

```conf
PCIE9098_0 = {
    cfg80211_wext=0xf
    max_vir_bss=1
    drv_mode=1
    ps_mode=2
    auto_ds=2
    host_mlme=1
    sta_name=mlan
    pcie_int_mode=1     # MSI 인터럽트
    napi=1              # NAPI 폴링
}
```

### 확인 방법

```bash
# 드라이버 지원 파라미터 목록
modinfo moal | grep parm

# 현재 로드된 파라미터 확인
cat /sys/module/moal/parameters/pcie_int_mode
cat /sys/module/moal/parameters/napi

# 인터럽트 모드 확인 (dmesg)
dmesg | grep -i "int_mode\|msi\|interrupt"
```

### 주의사항

- `pcie_int_mode=1` (MSI)은 일부 PCIe 브릿지 칩과 호환성 문제 가능
- 변경 후 반드시 타겟에서 WiFi 연결 안정성 테스트 필요
- ethtool coalescing은 moal 드라이버에서 미지원될 수 있음 → `pcie_int_mode`로 대체

---

## 2. sysctl.conf (커널 네트워크 스택)

파일 위치: `/etc/sysctl.conf`
적용: `sysctl -p`

### 발열 관련 파라미터

| 파라미터 | 기본값 | 권장값 | 설명 |
|---|---|---|---|
| `net.core.netdev_budget` | 300 | 600 | NAPI 폴링 사이클당 처리 패킷 수. 높이면 SoftIRQ 호출 빈도 감소 |
| `net.core.netdev_max_backlog` | 1000 | 2000 | 수신 백로그 큐 크기. 인터럽트 병합 시 버스트 패킷 수용 |

### 기존 설정 (유지)

| 파라미터 | 값 | 설명 |
|---|---|---|
| `net.core.rmem_max` | 12582912 | 수신 소켓 버퍼 최대 (12MB) |
| `net.core.wmem_max` | 12582912 | 송신 소켓 버퍼 최대 |
| `net.core.rmem_default` | 12582912 | 수신 소켓 버퍼 기본 |
| `net.core.wmem_default` | 12582912 | 송신 소켓 버퍼 기본 |
| `net.ipv4.conf.*.rp_filter` | 1 | Reverse Path Filter (스푸핑 방지) |

### 적용 예시

```bash
# /etc/sysctl.conf에 추가
net.core.netdev_budget=600
net.core.netdev_max_backlog=2000

# 즉시 적용
sysctl -p

# 개별 확인
sysctl net.core.netdev_budget
sysctl net.core.netdev_max_backlog
```

### 주의사항

- `netdev_budget`을 너무 높이면 (>1000) 단일 NAPI 폴링이 CPU를 오래 점유하여 다른 작업 지연 가능
- wbridge는 userspace 브릿지이므로 `net.bridge.bridge-nf-call-iptables`는 해당 없음 (Linux br0 전용)

---

## 3. DVFS / ASPM (추가 하드웨어 옵션 - 미적용)

현재 적용하지 않았으나, thermal 모드로도 발열이 부족할 경우 추가 적용 가능한 옵션입니다.

### CPU 클럭 제한 (DVFS)

```bash
# 현재 governor 확인
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

# ondemand로 변경
echo ondemand > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

# 최대 클럭을 1.2GHz로 제한 (100Mbps 브릿지에 충분)
echo 1200000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq

# 확인
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
```

**효과:** CPU 전압/주파수 동시 감소 → 온도 10~15도 하락 예상
**리스크:** 낮음 (100Mbps 트래픽에 1.2GHz 충분)

### PCIe ASPM (Active State Power Management)

```bash
# 현재 정책 확인
cat /sys/module/pcie_aspm/parameters/policy

# powersave 모드 (L1 적극 진입)
echo powersave > /sys/module/pcie_aspm/parameters/policy
```

**효과:** PCIe 링크 idle 시 저전력 상태 진입 → PCIe 컨트롤러 발열 감소
**리스크:** 일부 WiFi 칩에서 ASPM L1 진입/복귀 시 지연 또는 불안정 가능. 반드시 실측 테스트 필요

---

## 4. 모드별 드라이버 옵션 요약

| 항목 | latency | normal | thermal |
|---|---|---|---|
| **pcie_int_mode** | 0 (Legacy) | 1 (MSI) | 1 (MSI) |
| **napi** | 0 | 1 | 1 |
| **netdev_budget** | 300 (기본) | 600 | 600 |
| **netdev_max_backlog** | 1000 (기본) | 2000 | 2000 |
| **DVFS** | performance | ondemand | ondemand + 1.2GHz 제한 |
| **PCIe ASPM** | default | default | powersave |

**현재 적용 상태:**
- pcie_int_mode=1, napi=1: wifi_mod_para.conf에 적용됨
- netdev_budget=600, netdev_max_backlog=2000: sysctl.conf에 적용됨
- DVFS, ASPM: 미적용 (필요 시 수동 적용)
