// =====================================================
// 화물지킴이 최종 코드 v2 - ESP32
// 총점제 + 퍼센트 임계값 + 센서별 축적 감지
// 박스형 실험 데이터 기반 임계값 적용
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

// ── 핀 정의 ──
#define XSHUT_PIN   4
#define BUZZER_PIN  13
#define DOUT        16
#define SCK_HX      17
#define MPU_ADDR    0x68
#define RESET_BTN   12
#define MUTE_BTN    14

// ── 퍼센트 임계값 (박스형 실험 기반 확정) ──
#define TILT_MINOR    3.00   // 자이로 경미: 300% → 1점
#define TILT_MAJOR    4.00   // 자이로 심각: 400% → 2점
#define TENSION_MINOR 0.50   // 장력 경미: 20% 감소 → 2점
#define TENSION_MAJOR 0.80   // 장력 심각: 40% 감소 → 4점
#define DIST_MINOR    0.15   // 거리 경미: 15% → 1점
#define DIST_MAJOR    0.30   // 거리 심각: 30% → 2점

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
LiquidCrystal_I2C lcd(0x27, 16, 2);

long  baseRaw  = 0;
int   baseDist = 0;
float baseTilt = 0;
bool  isMuted  = false;

// ── 축적 카운터 (센서별 독립) ──
int accumT = 0;   // 자이로
int accumF = 0;   // 장력
int accumD = 0;   // 거리

// ── 이동평균 필터 ──
#define FILTER_SIZE 5
long  tensorBuf[FILTER_SIZE] = {0};
int   distBuf[FILTER_SIZE]   = {0};
float tiltBuf[FILTER_SIZE]   = {0};
int   fIdx = 0;

// ── MPU6050 직접 제어 ──
void mpuInit() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);
  Wire.write(0x00);
  Wire.endTransmission();
}

float getRawTilt() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 6, true);
  int16_t ax = Wire.read() << 8 | Wire.read();
  int16_t ay = Wire.read() << 8 | Wire.read();
  int16_t az = Wire.read() << 8 | Wire.read();
  float accY = ay / 16384.0;
  float accZ = az / 16384.0;
  tiltBuf[fIdx] = abs(atan2(accY, accZ) * 180.0 / PI);
  float sum = 0;
  for (int i = 0; i < FILTER_SIZE; i++) sum += tiltBuf[i];
  return sum / FILTER_SIZE;
}

float getTilt() {
  return abs(getRawTilt() - baseTilt);
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

void sendData(String data) {
  if (WiFi.status() == WL_CONNECTED) {
    udp.beginPacket("255.255.255.255", UDP_PORT);
    udp.print(data);
    udp.endPacket();
  }
  Serial.println(data);
}

void resetBaseline() {
  Serial.println("리셋 완료");  // 추가

  lcd.clear();
  Serial.println("LCD 클리어");  // 추가
  lcd.print("Ready!");
  Serial.println("Ready 출력");  // 추가
  delay(1000);
  Serial.println("셋업 완료");  // 추가
  for (int i = 0; i < FILTER_SIZE; i++) {
    tensorBuf[i] = scale.get_value(3);
    distBuf[i]   = vl53.read();
    tiltBuf[i]   = getRawTilt();
    delay(200);  // 100 → 200으로 늘리기
  }
  baseRaw  = getFilteredTension();
  baseDist = getFilteredDist();
  baseTilt = getRawTilt();

  accumT = 0;
  accumF = 0;
  accumD = 0;

  sendData("RESET");

  lcd.clear();
  lcd.print("RESET OK!");
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
  lcd.setCursor(0, 0); lcd.print("Cargo Guardian");
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
    lcd.print("WiFi OK!");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP());
    delay(2000);
  } else {
    Serial.println("\nWi-Fi 실패 - 오프라인 모드");
    lcd.clear();
    lcd.print("WiFi FAIL");
    lcd.setCursor(0, 1);
    lcd.print("Offline Mode");
    delay(2000);
  }

  udp.begin(UDP_PORT);
  mpuInit();
  delay(100);

  digitalWrite(XSHUT_PIN, LOW);  delay(10);
  digitalWrite(XSHUT_PIN, HIGH); delay(10);
  vl53.setTimeout(500);
  if (!vl53.init()) {
    lcd.clear(); lcd.print("VL53 ERROR!");
    while(1);
  }
  vl53.setDistanceMode(VL53L1X::Long);
  vl53.startContinuous(50);

  scale.begin(DOUT, SCK_HX);
  delay(1000);
  scale.tare();

  resetBaseline();

  lcd.clear();
  lcd.print("Ready!");
  delay(1000);
}

void loop() {

  // ── 영점 리셋 버튼 ──
  if (digitalRead(RESET_BTN) == LOW) {
    Serial.println("버튼 감지!");
    delay(50);
    if (digitalRead(RESET_BTN) == LOW) {
      Serial.println("버튼 확정!");
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
      if (isMuted) {
        lcd.print("MUTE ON");
        lcd.setCursor(0,1); lcd.print("Buzzer OFF");
      } else {
        lcd.print("MUTE OFF");
        lcd.setCursor(0,1); lcd.print("Buzzer ON");
      }
      delay(800);
      while (digitalRead(MUTE_BTN) == LOW);
    }
  }

  // ── 센서 raw값 읽기 ──
  float rawTilt    = getTilt();
  long  rawTension = getFilteredTension();
  int   rawDist    = getFilteredDist();

  fIdx = (fIdx + 1) % FILTER_SIZE;

  // ── 퍼센트 변화율 계산 ──
  float tiltPct    = rawTilt / (abs(baseTilt) + 1.0);
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

  String alarm = "NORMAL";
  if      (level == 2) alarm = "DANGER";
  else if (level == 1) alarm = "WARNING";

  // ── LCD 출력 ──
  lcd.clear();
  if (level == 0) {
    lcd.setCursor(0, 0);
    if (isMuted) lcd.print("NORMAL [MUTE]  ");
    else         lcd.print("STATUS: NORMAL ");
    lcd.setCursor(0, 1);
    lcd.print("T:"); lcd.print((int)(tiltPct*100));
    lcd.print("% F:"); lcd.print((int)(tensionPct*100));
    lcd.print("% D:"); lcd.print((int)(distPct*100));
    lcd.print("%");
    noTone(BUZZER_PIN);
  } else if (level == 1) {
    lcd.setCursor(0, 0); lcd.print("[WARNING]      ");
    lcd.setCursor(0, 1);
    String msg = "";
    if (scoreF > 0) msg += "Load ";
    if (scoreT > 0) msg += "Tilt ";
    if (scoreD > 0) msg += "Dist ";
    msg += "(" + String(totalScore) + "pt)";
    lcd.print(msg);
    if (!isMuted) {
      tone(BUZZER_PIN, 1000, 300);
      delay(600);
      noTone(BUZZER_PIN);
    }
  } else {
    lcd.setCursor(0, 0); lcd.print("[DANGER!!!]    ");
    lcd.setCursor(0, 1);
    String msg = "";
    if (scoreF > 0) msg += "Load ";
    if (scoreT > 0) msg += "Tilt ";
    if (scoreD > 0) msg += "Dist ";
    msg += "(" + String(totalScore) + "pt)";
    lcd.print(msg);
    if (!isMuted) {
      tone(BUZZER_PIN, 2000);
    } else {
      noTone(BUZZER_PIN);
    }
  }

  // ── CSV 전송 ──
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
    alarm;

  sendData(csvData);

  delay(200);
}
