# =====================================================
# 화물지킴이 - CSV 실시간 로거 (Wi-Fi UDP 수신, v6 코드 대응)
# openpyxl 대신 표준 csv 모듈 사용 + 매 줄마다 즉시 flush/fsync
# → 중간에 프로그램이 죽어도 그 직전 줄까지는 파일에 반드시 남음
#
# 실행법 (PowerShell / cmd 동일):
#   python cargo_guardian_csv_logger.py
#   (python이 안 먹으면 py cargo_guardian_csv_logger.py)
#   종료: Ctrl+C
# =====================================================

import socket
import csv
import os
from datetime import datetime

# ── 설정 ──
UDP_PORT = 12345
BUFFER_SIZE = 1024
SAVE_DIR = "."  # 필요하면 원하는 경로로 변경 (예: "C:/Users/mugi4/Desktop/가천대/공모전/실험데이터_실차")

timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
FILENAME = os.path.join(SAVE_DIR, f"실차_실험데이터_{timestamp}.csv")

HEADERS = [
    "시간(ms)",
    "기울기raw(최대값,°)", "장력raw", "거리raw(mm)",
    "기울기(%,대표값)", "장력(%)", "거리(%)",
    "자이로점수", "장력점수", "거리점수",
    "총점", "최종단계", "경보상태", "기록시각",
    "자이로1(%)", "자이로2(%)", "자이로3(%)", "자이로4(%)",
]

# ── UDP 소켓 ──
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", UDP_PORT))
sock.settimeout(1.0)

row_count = 0

# newline="" : csv 모듈 권장 설정 (윈도우에서 줄바꿈 중복 방지)
# encoding="utf-8-sig" : 엑셀에서 한글 CSV를 열 때 깨지지 않도록 BOM 포함
with open(FILENAME, "w", newline="", encoding="utf-8-sig") as f:
    writer = csv.writer(f)
    writer.writerow(HEADERS)
    f.flush()
    os.fsync(f.fileno())

    print("수신 대기 중...")
    print(f"저장 파일: {FILENAME}")
    print("종료: Ctrl+C")
    print("=" * 50)

    try:
        while True:
            try:
                data, addr = sock.recvfrom(BUFFER_SIZE)
                line = data.decode("utf-8", errors="ignore").strip()
            except socket.timeout:
                continue

            if not line:
                continue

            now = datetime.now().strftime("%H:%M:%S")

            # ── 영점 리셋 처리 ──
            if line == "RESET":
                writer.writerow(["===RESET==="] + [""] * 12 + [now] + [""] * 4)
                f.flush()
                os.fsync(f.fileno())
                print(f"[{now}] ===== 영점 리셋 =====")
                continue

            if not line.startswith("CSV,"):
                continue

            parts = line.split(",")[1:]
            if len(parts) != 17:
                continue

            try:
                time_ms     = int(parts[0])
                raw_tilt    = float(parts[1])
                raw_tension = int(parts[2])
                raw_dist    = int(parts[3])
                pct_tilt    = float(parts[4])
                pct_tension = float(parts[5])
                pct_dist    = float(parts[6])
                score_t     = int(parts[7])
                score_f     = int(parts[8])
                score_d     = int(parts[9])
                total_score = int(parts[10])
                level       = int(parts[11])
                alarm       = parts[12]
                gyro1       = float(parts[13])
                gyro2       = float(parts[14])
                gyro3       = float(parts[15])
                gyro4       = float(parts[16])
            except ValueError:
                # 파싱 실패한 줄은 건너뛰고 계속 수신 (프로그램은 안 죽음)
                continue

            writer.writerow([
                time_ms,
                raw_tilt, raw_tension, raw_dist,
                pct_tilt, pct_tension, pct_dist,
                score_t, score_f, score_d,
                total_score, level, alarm, now,
                gyro1, gyro2, gyro3, gyro4,
            ])
            # ★ 핵심: 매 줄마다 즉시 디스크에 반영
            f.flush()
            os.fsync(f.fileno())

            row_count += 1
            print(
                f"[{now}] 기울기:{pct_tilt:.1f}%({score_t}pt) "
                f"장력:{pct_tension:.1f}%({score_f}pt) "
                f"거리:{pct_dist:.1f}%({score_d}pt) "
                f"총점:{total_score}pt → {alarm}  (누적 {row_count}행)"
            )

    except KeyboardInterrupt:
        print("\n종료 중... (지금까지 받은 데이터는 이미 전부 저장되어 있습니다)")
    finally:
        sock.close()
        print(f"저장 완료 → {FILENAME} (총 {row_count}행)")
