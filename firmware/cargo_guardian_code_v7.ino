// =====================================================
// 화물지킴이 최종 코드 v7 - ESP32
// v6 대비 변경사항:
//   1) 장력 페일세이프 추가 — 장력이 TENSION_FAILSAFE(90%) 이상 손실되고
//      축적 감지 조건(ACCUM_TENSION)까지 만족하면, 자이로/거리 점수와
//      무관하게 즉시 DANGER로 강제 판정. (벨트가 사실상 완전히 풀렸는데
//      다른 센서가 안 따라와서 총점이 6점 미만이라 WARNING에 머무는
//      상황을 방지하기 위함 — 장력 단독 최대 배점은 4점이라 총점제만으로는
//      DANGER(6점 이상)에 도달하지 못할 수 있음)
//   2) 페일세이프 발동 여부를 alarm 문자열에 "-FS" 접미사로 표시
//      (CSV 필드 개수는 기존과 동일하게 17개 유지 → 기존 파이썬 로거 그대로 사용 가능)
//   3) LCD DANGER 화면에 [FS] 표시 추가
//
// v2 대비 변경사항(v6에서 유지):
//   1) 자이로(MPU6050) 1개 → 4개 확장 (TCA9548A I2C 멀티플렉서)
//   2) LCD 1602(16x2) → 2004(20x4) 확장
//   3) 전원: 보조배터리(USB 5V) → 독립 동작
// 총점제 + 퍼센트 임계값 + 센서별 축적 감지 (박스형 실험 데이터 기반)
// =====================================================

#include <Wire.h>
#include <VL53L1X.h>
#include <HX711.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WiFiUdp.h>

// ── Wi-Fi 설정 ──
const char* ssid     = "Billiardchalktheif";
const char* password = "goqudeo1!";
const int   UDP_PORT = 12345;
WiFiUDP udp;

// ── 핀 정의 (기존과 동일, 추가 GPIO 없음) ──
#define XSHUT_PIN   4
#define BUZZER_PIN  13
#define DOUT        16
#define SCK_HX      17
#define MPU_ADDR    0x68     // 4개 전부 동일 주소 (채널로 구분)
#define RESET_BTN   12
#define MUTE_BTN    14

// ── TCA9548A 멀티플렉서 (자이로 4개용) ──
#define TCA_ADDR    0x70
#define NUM_GYRO    4        // 자이로 채널 0~3

// ── 퍼센트 임계값 (박스형 실험 기반 확정) ──
#define TILT_MINOR    3.00   // 자이로 경미: 300% → 1점
#define TILT_MAJOR    4.00   // 자이로 심각: 400% → 2점
#define TENSION_MINOR 0.50   // 장력 경미: 50% 감소 → 2점
#define TENSION_MAJOR 0.80   // 장력 심각: 80% 감소 → 4점
#define DIST_MINOR    0.15   // 거리 경미: 15% → 1점
#define DIST_MAJOR    0.30   // 거리 심각: 30% → 2점

// ── ★ v7 추가: 장력 페일세이프 임계값 ──
// 장력이 이 값 이상 손실되고 축적 감지까지 만족하면, 총점(다른 센서 점수)과
// 무관하게 즉시 DANGER로 강제 판정한다. (벨트 사실상 완전 이탈 대응)
#define TENSION_FAILSAFE 0.90   // 90% 이상 손실 시 페일세이프 발동

// ── 센서별 축적 감지 횟수 ──
#define ACCUM_TILT    3   // 자이로: 3회 × 200ms = 0.6초
#define ACCUM_DIST    5   // 거리:   5회 × 200ms = 1.0초
#define ACCUM_TENSION 7   // 장력:   7회 × 200ms = 1.4초

// ── 총점 임계값 ──
#define SCORE_WARNING  3   // 3점 이상 → WARNING
#define SCORE_DANGER   6   // 6점 이상 → DANGER

// ── 객체 생성 ──
VL53L1X vl53;
HX711 scale;
LiquidCrystal_I2C lcd(0x27, 20, 4);   // ★ 1602 → 2004 (20x4)로 변경

long  baseRaw  = 0;
int   baseDist = 0;
bool  isMuted  = false;

// ── 축적 카운터 (센서별 독립) ──
int accumT = 0;   // 자이로 (좌/우 2개씩 평균 후 더 큰 쪽 기준)
int accumF = 0;   // 장력
int accumD = 0;   // 거리

// ── 이동평균 필터 ──
#define FILTER_SIZE 5
long  tensorBuf[FILTER_SIZE] = {0};
int   distBuf[FILTER_SIZE]   = {0};
int   fIdx = 0;

// ── 자이로 4개용 데이터 구조 ──
float tiltBuf[NUM_GYRO][FILTER_SIZE] = {{0}};
float baseTilt[NUM_GYRO] = {0, 0, 0, 0};
float tiltPctArr[NUM_GYRO] = {0, 0, 0, 0};   // 매 루프 각 채널 변화율(%) 저장 (LCD/CSV용)

// ── TCA9548A 채널 선택 ──
void tcaSelect(uint8_t channel) {
  if (channel > 7) return;
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(1 << channel);
  Wire.endTransmission();
}

// ── MPU6050 초기화 (4개 채널 전부) ──
void mpuInit() {
  for (uint8_t ch = 0; ch < NUM_GYRO; ch++) {
    tcaSelect(ch);
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x6B);
    Wire.write(0x00);
    Wire.endTransmission();
  }
}

// ── 특정 채널 raw 기울기 1회 측정 ──
float readRawTiltOnce(uint8_t ch) {
  tcaSelect(ch);
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 6, true);
  int16_t ax = Wire.read() << 8 | Wire.read();
  int16_t ay = Wire.read() << 8 | Wire.read();
  int16_t az = Wire.read() << 8 | Wire.read();
  float accY = ay / 16384.0;
  float accZ = az / 16384.0;
  return abs(atan2(accY, accZ) * 180.0 / PI);
}

// ── 특정 채널 이동평균 필터링된 기울기 ──
float getFilteredTilt(uint8_t ch) {
  tiltBuf[ch][fIdx] = readRawTiltOnce(ch);
  float sum = 0;
  for (int i = 0; i < FILTER_SIZE; i++) sum += tiltBuf[ch][i];
  return sum / FILTER_SIZE;
}

// ── 4개 채널 영점 대비 변화폭의 평균(도 단위, raw 로깅용) 반환 ──
// tiltPctArr[]를 채우는 역할도 겸함 (실제 대표 tiltPct는 loop()에서 좌/우 그룹별로 재계산)
float getAvgTiltDeviation() {
  float sumDev = 0;
  for (uint8_t ch = 0; ch < NUM_GYRO; ch++) {
    float filtered = getFilteredTilt(ch);
    float dev = abs(filtered - baseTilt[ch]);
    tiltPctArr[ch] = dev / (abs(baseTilt[ch]) + 1.0);  // 각 채널 변화율(%) 개별 저장 (LCD/CSV용, 그대로 유지)
    sumDev += dev;
  }
  return sumDev / NUM_GYRO;
}

long getFilteredTension() {
  tensorBuf[fIdx] = scale.get_value(3);
  long sum = 0;
  for (int i = 0; i < FILTER_SIZE; i++) sum += tensorBuf[i];
  return sum / FILTER_SIZE;
}

int getFilteredDist() {
  distBuf[fIdx] = vl53.read();
  int sum = 0;
  for (int i = 0; i < FILTER_SIZE; i++) sum += distBuf[i];
  return sum / FILTER_SIZE;
}

// UDP로만 CSV 원본 전송 (엑셀 저장용 - 파이썬이 이 포맷을 파싱함)
void sendUDP(String data) {
  if (WiFi.status() == WL_CONNECTED) {
    udp.beginPacket("255.255.255.255", UDP_PORT);
    udp.print(data);
    udp.endPacket();
  }
}

// 시리얼 모니터에는 사람이 읽기 편한 형식으로 출력
void printReadable(unsigned long t, float tiltPct, float tensionPct, float distPct,
                    int scoreT, int scoreF, int scoreD, int totalScore, String alarm) {
  Serial.print("[");
  Serial.print(t / 1000.0, 1);
  Serial.print("s] ");

  Serial.print("Tilt(avg) ");
  Serial.print(tiltPct, 1);
  Serial.print("%  [T1:");
  Serial.print(tiltPctArr[0] * 100.0, 1);
  Serial.print(" T2:");
  Serial.print(tiltPctArr[1] * 100.0, 1);
  Serial.print(" T3:");
  Serial.print(tiltPctArr[2] * 100.0, 1);
  Serial.print(" T4:");
  Serial.print(tiltPctArr[3] * 100.0, 1);
  Serial.print("]  ");

  Serial.print("Tension ");
  Serial.print(tensionPct, 1);
  Serial.print("%  Dist ");
  Serial.print(distPct, 1);
  Serial.print("%  ");

  Serial.print("| Score T:");
  Serial.print(scoreT);
  Serial.print(" F:");
  Serial.print(scoreF);
  Serial.print(" D:");
  Serial.print(scoreD);
  Serial.print(" Total:");
  Serial.print(totalScore);

  Serial.print("  ==> ");
  Serial.println(alarm);
}

void resetBaseline() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Resetting...");
  delay(500);

  scale.tare();

  // 자이로 4채널 + 장력 + 거리 버퍼 채우기
  for (int i = 0; i < FILTER_SIZE; i++) {
    tensorBuf[i] = scale.get_value(3);
    distBuf[i]   = vl53.read();
    for (uint8_t ch = 0; ch < NUM_GYRO; ch++) {
      tiltBuf[ch][i] = readRawTiltOnce(ch);
    }
    delay(200);
  }

  baseRaw  = getFilteredTension();
  baseDist = getFilteredDist();

  for (uint8_t ch = 0; ch < NUM_GYRO; ch++) {
    float sum = 0;
    for (int i = 0; i < FILTER_SIZE; i++) sum += tiltBuf[ch][i];
    baseTilt[ch] = sum / FILTER_SIZE;
  }

  accumT = 0;
  accumF = 0;
  accumD = 0;

  sendUDP("RESET");
  Serial.println("===== RESET =====");

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("RESET OK!");
  delay(1000);
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(XSHUT_PIN,  OUTPUT);
  pinMode(RESET_BTN,  INPUT_PULLUP);
  pinMode(MUTE_BTN,   INPUT_PULLUP);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("Cargo Guardian v7");
  lcd.setCursor(0, 1); lcd.print("Connecting...");

  WiFi.begin(ssid, password);
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500);
    Serial.print(".");
    retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi 연결 성공!");
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("WiFi OK!");
    lcd.setCursor(0, 1); lcd.print(WiFi.localIP());
    delay(2000);
  } else {
    Serial.println("\nWi-Fi 실패 - 오프라인 모드");
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("WiFi FAIL");
    lcd.setCursor(0, 1); lcd.print("Offline Mode");
    delay(2000);
  }

  udp.begin(UDP_PORT);
  mpuInit();          // 자이로 4채널 전부 초기화
  delay(100);

  digitalWrite(XSHUT_PIN, LOW);  delay(10);
  digitalWrite(XSHUT_PIN, HIGH); delay(10);
  vl53.setTimeout(500);
  if (!vl53.init()) {
    lcd.clear(); lcd.setCursor(0, 0); lcd.print("VL53 ERROR!");
    while (1);
  }
  vl53.setDistanceMode(VL53L1X::Long);
  vl53.startContinuous(50);

  scale.begin(DOUT, SCK_HX);
  delay(1000);
  scale.tare();

  resetBaseline();

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Ready!");
  delay(1000);
}

void loop() {

  // ── 영점 리셋 버튼 ──
  if (digitalRead(RESET_BTN) == LOW) {
    delay(50);
    if (digitalRead(RESET_BTN) == LOW) {
      resetBaseline();
      while (digitalRead(RESET_BTN) == LOW);
    }
  }

  // ── 음소거 버튼 ──
  if (digitalRead(MUTE_BTN) == LOW) {
    delay(50);
    if (digitalRead(MUTE_BTN) == LOW) {
      isMuted = !isMuted;
      noTone(BUZZER_PIN);
      lcd.clear();
      lcd.setCursor(0, 0);
      if (isMuted) {
        lcd.print("MUTE ON");
        lcd.setCursor(0, 1); lcd.print("Buzzer OFF");
      } else {
        lcd.print("MUTE OFF");
        lcd.setCursor(0, 1); lcd.print("Buzzer ON");
      }
      delay(800);
      while (digitalRead(MUTE_BTN) == LOW);
    }
  }

  // ── 센서 raw값 읽기 ──
  float rawTilt    = getAvgTiltDeviation();   // 4개 자이로 변화폭의 평균 (tiltPctArr[]도 함께 갱신됨)
  long  rawTension = getFilteredTension();
  int   rawDist    = getFilteredDist();

  fIdx = (fIdx + 1) % FILTER_SIZE;

  // ── 퍼센트 변화율 계산 ──
  // 좌(채널0,1) / 우(채널2,3) 각각 2개씩 평균 → 둘 중 큰 쪽을 대표값으로 사용
  // (채널-위치 매핑이 실제와 다르면 아래 인덱스만 바꾸면 됩니다)
  float leftAvg  = (tiltPctArr[0] + tiltPctArr[1]) / 2.0;
  float rightAvg = (tiltPctArr[2] + tiltPctArr[3]) / 2.0;
  float tiltPct  = (leftAvg > rightAvg) ? leftAvg : rightAvg;
  float tensionPct = (float)(abs(baseRaw) - abs(rawTension))
                     / (abs(baseRaw) + 1.0);
  float distPct    = (float)abs(rawDist - baseDist)
                     / (abs(baseDist) + 1.0);

  // ── 센서별 축적 카운터 업데이트 ──
  if (tiltPct >= TILT_MINOR)       accumT++;
  else                              accumT = 0;

  if (tensionPct >= TENSION_MINOR) accumF++;
  else                              accumF = 0;

  if (distPct >= DIST_MINOR)       accumD++;
  else                              accumD = 0;

  // ── 센서별 점수 계산 (각자 다른 축적 횟수) ──
  int scoreT = 0;
  int scoreF = 0;
  int scoreD = 0;

  // 자이로 (3회 연속 = 0.6초)
  if (accumT >= ACCUM_TILT) {
    if      (tiltPct >= TILT_MAJOR) scoreT = 2;
    else if (tiltPct >= TILT_MINOR) scoreT = 1;
  }

  // 장력 (7회 연속 = 1.4초)
  if (accumF >= ACCUM_TENSION) {
    if      (tensionPct >= TENSION_MAJOR) scoreF = 4;
    else if (tensionPct >= TENSION_MINOR) scoreF = 2;
  }

  // 거리 (5회 연속 = 1.0초)
  if (accumD >= ACCUM_DIST) {
    if      (distPct >= DIST_MAJOR) scoreD = 2;
    else if (distPct >= DIST_MINOR) scoreD = 1;
  }

  // ── 총점 계산 ──
  int totalScore = scoreT + scoreF + scoreD;

  // ── 단계 판정 ──
  int level = 0;
  if      (totalScore >= SCORE_DANGER)  level = 2;
  else if (totalScore >= SCORE_WARNING) level = 1;

  // ── ★ v7 추가: 장력 페일세이프 ──
  // 장력이 거의 완전히 빠졌는데(90% 이상) 자이로/거리가 안 따라와서
  // 총점이 6점 미만인 경우, 다른 센서 점수와 무관하게 즉시 DANGER로 강제 판정.
  // (장력 단독 최대 배점은 4점이라 총점제만으로는 DANGER에 도달 못 하는 경우 대응)
  bool tensionFailsafe = false;
  if (accumF >= ACCUM_TENSION && tensionPct >= TENSION_FAILSAFE) {
    level = 2;
    tensionFailsafe = true;
  }

  String alarm = "NORMAL";
  if      (level == 2) alarm = tensionFailsafe ? "DANGER-FS" : "DANGER";
  else if (level == 1) alarm = "WARNING";

  // ── LCD 출력 (20x4로 확장 - 4개 자이로 값도 함께 표시) ──
  lcd.clear();
  if (level == 0) {
    lcd.setCursor(0, 0);
    if (isMuted) lcd.print("NORMAL [MUTE]");
    else         lcd.print("STATUS: NORMAL");

    lcd.setCursor(0, 1);
    lcd.print("F:"); lcd.print((int)(tensionPct * 100));
    lcd.print("% D:"); lcd.print((int)(distPct * 100));
    lcd.print("%");

    lcd.setCursor(0, 2);
    lcd.print("T1:"); lcd.print((int)(tiltPctArr[0] * 100));
    lcd.print(" T2:"); lcd.print((int)(tiltPctArr[1] * 100));

    lcd.setCursor(0, 3);
    lcd.print("T3:"); lcd.print((int)(tiltPctArr[2] * 100));
    lcd.print(" T4:"); lcd.print((int)(tiltPctArr[3] * 100));

    noTone(BUZZER_PIN);

  } else if (level == 1) {
    lcd.setCursor(0, 0); lcd.print("[WARNING]");

    lcd.setCursor(0, 1);
    String msg = "";
    if (scoreF > 0) msg += "Load ";
    if (scoreT > 0) msg += "Tilt ";
    if (scoreD > 0) msg += "Dist ";
    msg += "(" + String(totalScore) + "pt)";
    lcd.print(msg);

    lcd.setCursor(0, 2);
    lcd.print("T1:"); lcd.print((int)(tiltPctArr[0] * 100));
    lcd.print(" T2:"); lcd.print((int)(tiltPctArr[1] * 100));

    lcd.setCursor(0, 3);
    lcd.print("T3:"); lcd.print((int)(tiltPctArr[2] * 100));
    lcd.print(" T4:"); lcd.print((int)(tiltPctArr[3] * 100));

    if (!isMuted) {
      tone(BUZZER_PIN, 1000, 300);
      delay(600);
      noTone(BUZZER_PIN);
    }

  } else {
    lcd.setCursor(0, 0); lcd.print("[DANGER!!!]");

    lcd.setCursor(0, 1);
    String msg = "";
    if (scoreF > 0) msg += "Load ";
    if (scoreT > 0) msg += "Tilt ";
    if (scoreD > 0) msg += "Dist ";
    if (tensionFailsafe) msg += "[FS]";       // ★ v7: 페일세이프로 강제 발동됐음을 표시
    msg += "(" + String(totalScore) + "pt)";
    lcd.print(msg);

    lcd.setCursor(0, 2);
    lcd.print("T1:"); lcd.print((int)(tiltPctArr[0] * 100));
    lcd.print(" T2:"); lcd.print((int)(tiltPctArr[1] * 100));

    lcd.setCursor(0, 3);
    lcd.print("T3:"); lcd.print((int)(tiltPctArr[2] * 100));
    lcd.print(" T4:"); lcd.print((int)(tiltPctArr[3] * 100));

    if (!isMuted) {
      tone(BUZZER_PIN, 2000);
    } else {
      noTone(BUZZER_PIN);
    }
  }

  // ── CSV 전송 (자이로 4채널 raw/pct 개별 추가) ──
  // ★ v7: alarm 문자열에 "-FS" 접미사가 붙을 수 있지만 필드 개수(17개)는 v6과 동일
  //        → 기존 파이썬 CSV 로거를 코드 수정 없이 그대로 사용 가능
  String csvData = "CSV," +
    String(millis())                       + "," +
    String(rawTilt)                        + "," +
    String(abs(baseRaw) - abs(rawTension)) + "," +
    String(abs(rawDist - baseDist))        + "," +
    String(tiltPct * 100.0, 1)             + "," +
    String(tensionPct * 100.0, 1)          + "," +
    String(distPct * 100.0, 1)             + "," +
    String(scoreT)                         + "," +
    String(scoreF)                         + "," +
    String(scoreD)                         + "," +
    String(totalScore)                     + "," +
    String(level)                          + "," +
    alarm                                  + "," +
    String(tiltPctArr[0] * 100.0, 1)       + "," +   // 자이로1 개별 %
    String(tiltPctArr[1] * 100.0, 1)       + "," +   // 자이로2 개별 %
    String(tiltPctArr[2] * 100.0, 1)       + "," +   // 자이로3 개별 %
    String(tiltPctArr[3] * 100.0, 1);                // 자이로4 개별 %

  sendUDP(csvData);
  printReadable(millis(), tiltPct * 100.0, tensionPct * 100.0, distPct * 100.0,
                scoreT, scoreF, scoreD, totalScore, alarm);

  delay(200);
}
