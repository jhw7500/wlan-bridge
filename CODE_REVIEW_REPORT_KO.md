# 프로젝트: wlan-bridge
**보고서 위치:** `projects/wlan-bridge/CODE_REVIEW_REPORT.md`

#### 1. 개요 (Executive Summary)
`wlan-bridge` 프로젝트는 임베디드 리눅스 시스템(특히 i.MX8MP)을 위해 설계된 고성능 네트워크 브리지 유틸리티입니다. 이 프로젝트는 표준 `libpcap` 기반의 브리지(`dumb`)와 고성능 저지연 `TPACKET_V3` 기반 브리지(`dumb-tpacket`) 두 가지 구현을 제공합니다.

**전반적인 품질 점수:** 🟢 **높음 (High)** (상용 준비 완료)

이 코드베이스는 동시성(Concurrency), 메모리 관리, 리눅스 네트워크 스택 최적화 분야에서 수준 높은 시스템 프로그래밍 지식을 보여줍니다. 문서는 최신 상태이며 코드 상태를 정확하게 반영하고 있습니다.

#### 2. 주요 발견 사항 (Key Findings)

**🌟 강점 (Strengths)**
*   **성능 엔지니어링:**
    *   **Lock-free 통계:** C11 `_Atomic`과 패딩(padding)을 사용하여 캐시 라인 공유 문제(False Sharing)를 방지(`struct thread_stats`), 락 오버헤드 없이 정확한 통계를 수집합니다.
    *   **Zero-Copy 네트워킹:** `dumb-tpacket`은 커널 메모리에 직접 접근(8MB 버퍼)하는 `TPACKET_V3` 링 버퍼를 구현하여 CPU 사용량과 지연 시간을 획기적으로 줄였습니다.
    *   **시스템 튜닝:** `SCHED_FIFO` 실시간 스케줄링, CPU 코어 고정(Affinity), 페이지 폴트 방지를 위한 메모리 잠금(`mlockall`)을 명시적으로 지원합니다.
*   **견고성 (Robustness):**
    *   **Atomic 상태 관리:** 스레드 동기화(`ifs.ready`) 및 종료 신호 처리에 원자적 변수(Atomic variables)를 사용합니다.
    *   **방어적 설정:** `safe_atoi` 및 `clamp_int` 함수를 통해 잘못된 환경 변수가 입력되어도 애플리케이션이 충돌하지 않도록 합니다.
*   **문서 및 프로세스:**
    *   **정확성:** `docs/OPTIMIZATION_SUMMARY.md`는 최근 코드 변경 사항(예: `keep_running`의 atomic 전환, 버퍼 크기 조정)을 정확히 반영합니다.
    *   **운영 가이드:** `docs/DEPLOYMENT-CHECKLIST.md`는 현장 배포 시 매우 포괄적이고 실용적입니다.

**⚠️ 잠재적 문제 및 개선점**
*   **보안 (권한):** 애플리케이션이 루트 권한으로 실행됩니다(Raw 소켓 사용을 위해 필수).
    *   *위험:* 루트 권한은 잠재적인 버퍼 오버플로우 등의 취약점이 발생할 경우 시스템 전체를 위험하게 할 수 있습니다.
    *   *완화:* 소켓 생성 후 `libcap`을 사용하여 전체 루트 권한을 포기하고 `CAP_NET_RAW`, `CAP_IPC_LOCK`, `CAP_SYS_NICE` 권한만 유지하도록 개선하는 것이 좋습니다.
*   **스크립트 견고성:** `scripts/optimize-for-udp.sh`는 `ethtool`이 설치되어 있다고 가정합니다.
    *   *개선:* `setup-irq-affinity.sh`처럼 `command -v ethtool` 체크 로직을 추가하여 런타임 오류를 방지해야 합니다.

#### 3. 세부 구성 요소 분석

| 파일 | 분석 내용 |
| :--- | :--- |
| **`dumb/dumb.c`** | 깔끔한 `libpcap` 구현입니다. 로그 홍수를 막기 위한 속도 제한(Rate-limited) 로그가 잘 적용되어 있습니다. 802.1Q VLAN 헤더 처리가 정확합니다. |
| **`dumb/dumb-tpacket.c`** | **핵심 컴포넌트.** 매우 수준 높은 구현입니다. 복잡한 `TPACKET_V3` 블록 디스크립터 포맷을 정확히 처리합니다. TX 링 실패 시 `sendto`로 폴백(Fallback)하는 기능은 훌륭한 호환성 기능입니다. |
| **`dumb/Makefile`** | 구조가 잘 잡혀 있습니다. 릴리스, 디버그, 네이티브 빌드를 위한 타겟이 분리되어 있습니다. LTO(Link Time Optimization) 및 `-march=native` 최적화를 사용합니다. |
| **`optimize-for-udp.sh`** | 커널 버퍼(16MB)와 큐를 공격적으로 튜닝합니다. 고대역폭 UDP 비디오 스트리밍에 적합하지만, 전역 시스템 상태를 변경하므로 주의가 필요합니다. |
| **`setup-irq-affinity.sh`** | i.MX8 성능에 매우 중요합니다. IRQ를 정확히 식별하고 브리지 스레드와의 CPU 경합을 피하도록 설정합니다. |

#### 4. 제안 사항 (Recommendations)

**즉시 적용 (Quick Wins)**
1.  **스크립트 업데이트:** `optimize-for-udp.sh`에 `ethtool` 설치 확인 로직 추가.
2.  **문서화:** `dumb/README.md`에 `tpacket` 버전을 위한 커널 설정 요구사항(예: `CONFIG_PACKET`, `CONFIG_PACKET_MMAP`)을 명시.

**장기적 목표 (Architecture)**
1.  **권한 관리:** 최소 권한 원칙(Principle of Least Privilege)을 준수하기 위해 권한 축소(Capability dropping) 구현.
2.  **BPF 필터링:** 더 높은 성능을 위해 사용자 공간의 MAC/IP 필터링 로직을 eBPF(Classic BPF) 필터로 대체하여, 패킷이 링 버퍼에 도달하기 전에 커널에서 걸러내도록(Zero-copy filtering) 개선.

#### 5. 결론
이 코드는 의도된 목적(임베디드 비디오 브리징)에 매우 적합한 고성능 상용 등급(Production-grade) 코드베이스입니다. 사용된 최적화 기법들은 저지연 애플리케이션을 위한 업계 표준을 따르고 있습니다. 문서 관리 상태 또한 매우 우수합니다.
