#include <SPI.h>

struct AngleSample {
    uint32_t raw;
    float deg;
    uint8_t a3, a2, a1, crc, state;
    bool crcOk;
};

// picocom (and other raw terminals) only realign lines to column 0 on CRLF,
// not on a bare LF, so every output line is terminated with "\r\n".
static const char CRLF[] = "\r\n";
static void spln(const char* s) { Serial.print(s); Serial.print(CRLF); }

// ---- pins (from prompt.md) ----
#define PIN_CAL  7
#define PIN_MISO 12
#define PIN_MOSI 11
#define PIN_SCK  1
#define PIN_CS   6

// ---- MT6835 constants ----
#define MT_ID_DEFAULT 0x31      // expected value of ID register 0x01
#define SPI_FREQ      2000000UL // hardware SPI clock (500 kHz .. 2 MHz proven OK)

// MT6835 register map
#define REG_ID  0x01            // chip ID (0x31)
#define REG_A3  0x03            // angle byte A3  (bits 20..13)
#define REG_A2  0x04            // angle byte A2  (bits 12..5)
#define REG_A1  0x05            // angle byte A1  (bits 4..0 in top, status low)
#define REG_CRC 0x06            // CRC-8 over A3,A2,A1

// Hardware SPI on HSPI (SPI3). begin() argument order is (sck, miso, mosi, ss).
// ss = -1 => the peripheral does NOT drive CS; we toggle PIN_CS by hand.
// (required: letting the SPI peripheral own CS leaves MISO floating on this wiring)
static SPIClass hspi(HSPI);
static bool useBitBang = false; // flipped to true if the hardware path misses the ID

// CRC-8: poly 0x07, init 0x00, MSB-first, no reflect/xorout
static uint8_t crc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0x00;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

// Hardware-SPI single-register read (MODE0, manual CS).
// Frame: [0x30, reg, 0x00]; the sensor returns the data as the 3rd clocked-in byte.
static uint8_t hwReadReg(uint8_t reg) {
  digitalWrite(PIN_CS, HIGH);
  delayMicroseconds(50);
  digitalWrite(PIN_CS, LOW);
  delayMicroseconds(50);
  uint8_t tx[3] = { 0x30, reg, 0x00 };
  uint8_t rx[3];
  hspi.beginTransaction(SPISettings(SPI_FREQ, MSBFIRST, SPI_MODE0));
  for (int i = 0; i < 3; i++) rx[i] = hspi.transfer(tx[i]);
  hspi.endTransaction();
  digitalWrite(PIN_CS, HIGH);
  return rx[2];
}

// Bit-bang SPI fallback (MODE0). Only used if the hardware peripheral path
// fails to answer the ID on this wiring.
static uint8_t bbReadReg(uint8_t reg) {
  pinMode(PIN_MISO, INPUT);
  pinMode(PIN_MOSI, OUTPUT);
  pinMode(PIN_SCK, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  digitalWrite(PIN_SCK, LOW);
  digitalWrite(PIN_MOSI, LOW);
  delayMicroseconds(200);
  digitalWrite(PIN_CS, LOW);
  delayMicroseconds(200);
  uint8_t tx[3] = { 0x30, reg, 0x00 };
  uint8_t rx[3] = { 0, 0, 0 };
  for (int b = 0; b < 3; b++) {
    uint8_t rxbyte = 0;
    for (int i = 7; i >= 0; i--) {
      digitalWrite(PIN_MOSI, (tx[b] >> i) & 1);
      delayMicroseconds(10);
      digitalWrite(PIN_SCK, HIGH);               // rising edge -> sample MISO
      delayMicroseconds(10);
      rxbyte |= ((uint8_t)digitalRead(PIN_MISO)) << i;
      digitalWrite(PIN_SCK, LOW);
      delayMicroseconds(10);
    }
    rx[b] = rxbyte;
  }
  digitalWrite(PIN_CS, HIGH);
  return rx[2];
}

// Read one register, dispatching to the working transport.
static uint8_t readReg(uint8_t reg) {
  return useBitBang ? bbReadReg(reg) : hwReadReg(reg);
}

// Read the 21-bit angle (A3,A2,A1) plus the CRC byte, decode it.
static AngleSample readAngle() {
  AngleSample s;
  s.a3  = readReg(REG_A3);
  s.a2  = readReg(REG_A2);
  s.a1  = readReg(REG_A1);
  s.crc = readReg(REG_CRC);
  uint8_t buf[3] = { s.a3, s.a2, s.a1 };
  s.crcOk = (crc8(buf, 3) == s.crc);
  s.raw   = ((uint32_t)s.a3 << 13) | ((uint32_t)s.a2 << 5) | (uint32_t)(s.a1 >> 3);
  s.deg   = (float)s.raw / (float)(1 << 21) * 360.0f;
  s.state = s.a1 & 0x07;   // bit0 overspeed, bit1 weak-field, bit2 undervoltage
  return s;
}

// Human-readable measurement quality: CRC integrity + MT6835 status flags
// (bit0 over-speed, bit1 weak-field, bit2 undervoltage).
static const char* qualityStr(uint8_t state, bool crcOk) {
  if (crcOk && state == 0) return "ok";
  static char buf[48];
  bool first = true;
  buf[0] = 0;
  auto add = [&](const char* s) {
    if (!first) strcat(buf, " ");
    strcat(buf, s);
    first = false;
  };
  if (!crcOk) add("crc-fail");
  if (state & 0x01) add("over-speed");
  if (state & 0x02) add("weak-field");
  if (state & 0x04) add("undervoltage");
  return buf;
}

void setup() {
  // [step 1] serial
  Serial.begin(115200);
  delay(300);
  spln("=== MT6835 angle sensor on ESP32-S3 ===");
  spln("[1/6] Serial @ 115200 baud ready");

  // [step 2] pins + run mode
  Serial.printf("[2/6] pins: CAL=%d MISO=%d MOSI=%d SCK=%d CSN=%d\r\n",
                PIN_CAL, PIN_MISO, PIN_MOSI, PIN_SCK, PIN_CS);
  pinMode(PIN_CAL, OUTPUT);
  digitalWrite(PIN_CAL, LOW);   // LOW = normal run mode (HIGH = calibration)
  spln("      CAL -> LOW (run mode)");

  // [step 3] SPI bus
  spln("[3/6] SPI begin (HSPI/SPI3, MODE0, manual CS)");
  hspi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);

  // [step 4] ID read with settle/retry
  spln("[4/6] reading ID register (settle + retry)");
  uint8_t id = 0xFF;
  for (int i = 0; i < 6; i++) {
    int ms = 100 * (i + 1);
    delay(ms);
    id = readReg(REG_ID);
    Serial.printf("      +%d ms  ID=0x%02X %s\r\n", ms, id, id == MT_ID_DEFAULT ? "(match)" : "");
    if (id == MT_ID_DEFAULT) break;
  }

  // [step 5] bit-bang fallback if the hardware SPI path didn't answer
  if (id != MT_ID_DEFAULT) {
    spln("[5/6] hardware SPI ID mismatch -> switching to bit-bang");
    useBitBang = true;
    id = readReg(REG_ID);
    Serial.printf("      bit-bang ID=0x%02X %s\r\n", id, id == MT_ID_DEFAULT ? "(match)" : "");
  } else {
    spln("[5/6] hardware SPI path OK (bit-bang not needed)");
  }

  // [step 6] final status
  Serial.printf("[6/6] MT6835 ID = 0x%02X (expect 0x%02X) -> %s\r\n", id, MT_ID_DEFAULT,
                id == MT_ID_DEFAULT ? "detected" : "NOT detected");
  spln("---- ready, sampling every 1 s ----");
}

void loop() {
  static unsigned long last = 0;
  if (millis() - last < 1000) return;
  last = millis();

  AngleSample s = readAngle();
  Serial.printf("%.3f deg  [%s]\r\n\r\n", s.deg, qualityStr(s.state, s.crcOk));
}
