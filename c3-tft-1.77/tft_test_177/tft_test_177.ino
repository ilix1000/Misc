/*
 * ESP32-C3 SuperMini + 1.77" ST7735S TFT (128x160) test
 *
 * Same wiring as the 1.9" test (TFT pin -> C3 SuperMini GPIO):
 *   GND -> GND
 *   VCC -> 3.3V
 *   SCL -> GPIO 4   (SPI clock)
 *   SDA -> GPIO 6   (SPI MOSI)
 *   RES -> GPIO 10  (reset)
 *   DC  -> GPIO 3   (data/command, sometimes labelled RS or A0)
 *   CS  -> GPIO 7   (chip select)
 *   BLK -> GPIO 1   (backlight, PWM)
 *
 * Libraries: "Adafruit ST7735 and ST7789 Library" + "Adafruit GFX Library".
 * Arduino IDE: board "ESP32C3 Dev Module", USB CDC On Boot: Enabled.
 */

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

#define TFT_SCLK 4
#define TFT_MOSI 6
#define TFT_CS   7
#define TFT_DC   3
#define TFT_RST  10
#define TFT_BL   1
#define LED_PIN  8   // onboard LED, active LOW

// 1.77" ST7735S modules almost always work with BLACKTAB. If the image is
// shifted by a few pixels or there is a garbage line on one edge, try
// INITR_GREENTAB instead. If red and blue are swapped, also try GREENTAB.
#define TFT_TAB INITR_BLACKTAB

#define SPI_HZ 27000000  // ST7735 is rated lower than ST7789; drop to 16 MHz if noisy

Adafruit_ST7735 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);

static void backlight(uint8_t level) {
  analogWrite(TFT_BL, level);
}

static void title(const char *txt) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(2, 2);
  tft.print(txt);
  Serial.printf("[test] %s\n", txt);
}

static uint16_t wheel(uint8_t pos) {  // rainbow helper -> RGB565
  uint8_t r, g, b;
  if (pos < 85)       { r = 255 - pos * 3; g = pos * 3;       b = 0; }
  else if (pos < 170) { pos -= 85;  r = 0;           g = 255 - pos * 3; b = pos * 3; }
  else                { pos -= 170; r = pos * 3;     g = 0;           b = 255 - pos * 3; }
  return tft.color565(r, g, b);
}

// ---------------------------------------------------------------------------

static void testSolidColors() {
  const uint16_t colors[] = {ST77XX_RED, ST77XX_GREEN, ST77XX_BLUE,
                             ST77XX_WHITE, ST77XX_BLACK};
  const char *names[] = {"RED", "GREEN", "BLUE", "WHITE", "BLACK"};
  for (int i = 0; i < 5; i++) {
    uint32_t t = millis();
    tft.fillScreen(colors[i]);
    // Label each color so you can tell if red/blue are swapped.
    tft.setTextSize(2);
    tft.setTextColor(i == 3 ? ST77XX_BLACK : ST77XX_WHITE);
    tft.setCursor(8, tft.height() / 2 - 8);
    tft.print(names[i]);
    Serial.printf("[test] fill %s: %lu ms\n", names[i], millis() - t);
    delay(600);
  }
}

static void testCornersAndBorder() {
  // The border must be fully visible on all four edges with no garbage
  // line on any side. If not, change TFT_TAB.
  tft.fillScreen(ST77XX_BLACK);
  tft.drawRect(0, 0, tft.width(), tft.height(), ST77XX_WHITE);
  tft.drawRect(1, 1, tft.width() - 2, tft.height() - 2, ST77XX_RED);
  const int s = 14;
  tft.fillRect(0, 0, s, s, ST77XX_RED);                                   // TL
  tft.fillRect(tft.width() - s, 0, s, s, ST77XX_GREEN);                   // TR
  tft.fillRect(0, tft.height() - s, s, s, ST77XX_BLUE);                   // BL
  tft.fillRect(tft.width() - s, tft.height() - s, s, s, ST77XX_YELLOW);   // BR
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(16, tft.height() / 2 - 16);
  tft.printf("%dx%d", tft.width(), tft.height());
  tft.setTextSize(1);
  tft.setCursor(16, tft.height() / 2 + 6);
  tft.print("R G / B Y");
  delay(2500);
}

static void testText() {
  title("Text");
  tft.setCursor(2, 14);
  for (uint8_t sz = 1; sz <= 3; sz++) {
    tft.setTextSize(sz);
    tft.setTextColor(wheel(sz * 70));
    tft.println("Hello C3");
  }
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_CYAN);
  tft.println();
  tft.println("!\"#$%&'()*+,-./0123456789");
  tft.println(":;<=>?@ABCDEFGHIJKLMNOPQRS");
  tft.println("TUVWXYZ[\\]^_`abcdefghijklm");
  tft.println("nopqrstuvwxyz{|}~");
  delay(2500);
}

static void testLines() {
  title("Lines");
  uint32_t t = millis();
  int w = tft.width(), h = tft.height();
  for (int x = 0; x < w; x += 4)  tft.drawLine(0, 12, x, h - 1, wheel(x * 2));
  for (int y = 12; y < h; y += 4) tft.drawLine(0, 12, w - 1, y, wheel(y));
  Serial.printf("[test] lines: %lu ms\n", millis() - t);
  delay(1500);
}

static void testShapes() {
  title("Shapes");
  int cx = tft.width() / 2;
  for (int r = 40; r > 0; r -= 5) tft.fillCircle(cx, 56, r, wheel(r * 6));
  for (int i = 0; i < 5; i++)
    tft.drawRoundRect(6 + i * 3, 100 + i * 3, tft.width() - 12 - i * 6, 56 - i * 6, 6, wheel(i * 50));
  tft.fillTriangle(cx, 112, cx - 20, 144, cx + 20, 144, ST77XX_YELLOW);
  delay(2000);
}

static void testGradient() {
  title("Gradient");
  int w = tft.width();
  for (int y = 12; y < tft.height(); y++) {
    uint8_t v = map(y, 12, tft.height() - 1, 0, 255);
    tft.drawFastHLine(0, y, w / 3, tft.color565(v, 0, 0));
    tft.drawFastHLine(w / 3, y, w / 3, tft.color565(0, v, 0));
    tft.drawFastHLine(2 * w / 3, y, w - 2 * w / 3, tft.color565(0, 0, v));
  }
  delay(2000);
}

static void testRotation() {
  for (uint8_t r = 0; r < 4; r++) {
    tft.setRotation(r);
    tft.fillScreen(ST77XX_BLACK);
    tft.drawRect(0, 0, tft.width(), tft.height(), ST77XX_GREEN);
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_GREEN);
    tft.setCursor(6, 6);
    tft.printf("Rot %d", r);
    tft.setCursor(6, 26);
    tft.printf("%dx%d", tft.width(), tft.height());
    tft.fillTriangle(6, 50, 22, 50, 6, 66, ST77XX_WHITE);  // top-left marker
    delay(1200);
  }
  tft.setRotation(0);
}

static void testInvert() {
  title("Invert");
  int w = tft.width() - 12;
  tft.fillRect(6, 14, w, 44, ST77XX_RED);
  tft.fillRect(6, 62, w, 44, ST77XX_GREEN);
  tft.fillRect(6, 110, w, 44, ST77XX_BLUE);
  for (int i = 0; i < 4; i++) {
    tft.invertDisplay(i % 2 == 0);
    delay(500);
  }
  tft.invertDisplay(false);
}

static void testBacklight() {
  title("Backlight PWM GPIO1");
  tft.fillRect(6, 14, tft.width() - 12, tft.height() - 20, ST77XX_WHITE);
  for (int v = 255; v >= 0; v -= 5)  { backlight(v); delay(10); }
  for (int v = 0; v <= 255; v += 5)  { backlight(v); delay(10); }
  backlight(255);
}

static void testFps() {
  title("FPS");
  const int frames = 30;
  uint32_t t = millis();
  for (int i = 0; i < frames; i++) tft.fillScreen(wheel(i * 8));
  uint32_t dt = millis() - t;
  float fps = frames * 1000.0f / dt;
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(2, 2);
  tft.print("Full-screen fills");
  tft.setTextSize(3);
  tft.setCursor(2, 20);
  tft.printf("%.1f", fps);
  tft.setTextSize(1);
  tft.print(" fps");
  tft.setCursor(2, 52);
  tft.printf("%lu ms/frame", dt / frames);
  tft.setCursor(2, 64);
  tft.printf("SPI %d MHz", SPI_HZ / 1000000);
  Serial.printf("[test] fill rate: %.1f fps (%lu ms/frame)\n", fps, dt / frames);
  delay(2500);
}

static void testBouncingBall() {
  title("Bounce");
  int x = 30, y = 50, dx = 2, dy = 3;
  const int r = 7, top = 12;
  uint32_t end = millis() + 4000;
  uint8_t c = 0;
  while (millis() < end) {
    tft.fillCircle(x, y, r, ST77XX_BLACK);
    x += dx; y += dy;
    if (x - r <= 0 || x + r >= tft.width() - 1)  dx = -dx;
    if (y - r <= top || y + r >= tft.height() - 1) dy = -dy;
    tft.fillCircle(x, y, r, wheel(c += 3));
    delay(10);
  }
}

static void showSummary(uint32_t loopNo) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_GREEN);
  tft.setTextSize(2);
  tft.setCursor(2, 6);
  tft.println("TEST OK");
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(2, 30);  tft.print("ESP32-C3 SuperMini");
  tft.setCursor(2, 42);  tft.print("ST7735S 1.77\" 128x160");
  tft.setCursor(2, 54);  tft.printf("CPU %lu MHz", getCpuFrequencyMhz());
  tft.setCursor(2, 66);  tft.printf("Heap %lu B", (unsigned long)ESP.getFreeHeap());
  tft.setCursor(2, 78);  tft.printf("Loop #%lu", loopNo);
  tft.setCursor(2, 90);  tft.printf("Up %lus", millis() / 1000);
  delay(2500);
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nESP32-C3 SuperMini 1.77\" TFT test");

  pinMode(LED_PIN, OUTPUT);
  pinMode(TFT_BL, OUTPUT);
  backlight(255);

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(TFT_TAB);
  tft.setSPISpeed(SPI_HZ);
  tft.setRotation(0);

  Serial.printf("Display ready: %dx%d\n", tft.width(), tft.height());
}

void loop() {
  static uint32_t n = 0;
  n++;
  digitalWrite(LED_PIN, LOW);   // LED on
  testSolidColors();
  testCornersAndBorder();
  digitalWrite(LED_PIN, HIGH);  // LED off
  testText();
  testLines();
  testShapes();
  testGradient();
  testRotation();
  testInvert();
  testBacklight();
  testFps();
  testBouncingBall();
  showSummary(n);
}
