/*
 * firmware.ino - soft-latch "true shutdown" + ESP32 sleep-mode demo
 *
 * Target : classic ESP32 (DevKitC), Arduino-ESP32 core 2.x / 3.x
 * Libs   : none (Arduino core + ESP-IDF headers shipped with it)
 * Serial : 115200 baud, type "help"
 * Circuit: see README.md  (LATCH -> R4 -> Q2 gate, SENSE <- node S with 10k pull-up)
 *
 */

#include <Arduino.h>
#include "esp_sleep.h"
#include "driver/gpio.h"

// ============================ Hardware pins ============================
// HINT LATCH : any output-capable, non-strapping GPIO. Avoid strapping 0,2,5,12,15, flash 6-11,
//              and input-only 34-39. Default level at reset must be input/low (R5 holds Q2 off).
//              S3: GPIO 0-21 (avoid 0,3,45,46 strapping). C3: GPIO 0-5 are RTC/hold capable.
// HINT SENSE : must be RTC-capable for deep-sleep button wake.
//              Classic ESP32: 0,2,4,12-15,25-27,32-39 (34-39 input-only, no internal pull -> use R2).
//              S3: GPIO 0-21, ext1 ANY_LOW. C3: no ext0/ext1, use esp_deep_sleep_enable_gpio_wakeup().
// HINT LED   : GPIO2 is the on-board LED on many DevKits but it is a strapping pin (keep it
//              floating/low at boot). Set -1 to disable LED feedback.
const int PIN_LATCH = 26;   // output: HIGH holds power on (via Q2 -> Q1)
const int PIN_SENSE = 33;   // input : LOW = button pressed (external 10k pull-up)
const int PIN_LED   = 2;    // output: hold-feedback LED, -1 = none

// ============================ Tunables (edit here) =====================
// Debounce is done in software, no capacitor needed on the button.
const uint32_t DEBOUNCE_MS      = 30;   // button debounce
const uint32_t POWER_ON_HOLD_MS = 300;  // cold boot: button must still be held this long after
                                        // setup() starts, else battery path is released again (0 = off).
                                        // When USB powers the MCU (no button) this just leaves the board
                                        // running from USB with the battery path off, which is harmless.
const uint32_t DOUBLE_CLICK_MS  = 300;  // double-click window; click action is delayed by this (0 = no double-click)

// Runtime settings: also changeable over Serial. RTC_DATA_ATTR keeps them through
// deep sleep (lost after hibernation / true shutdown -> defaults are restored).
enum Mode : uint8_t { MODE_ACTIVE = 0, MODE_LIGHT = 1, MODE_DEEP = 2, MODE_HIB = 3 };
const uint8_t NUM_MODES = 4;

const uint32_t CFG_MAGIC = 0xC0DE5A19;
RTC_DATA_ATTR uint32_t cfgMagic = 0;
RTC_DATA_ATTR uint32_t bootCount;
RTC_DATA_ATTR uint32_t midHoldMs;      // hold this long -> "release = status" zone
RTC_DATA_ATTR uint32_t longPressMs;    // hold this long -> true shutdown (must be > midHoldMs)
RTC_DATA_ATTR uint32_t sleepSeconds;   // timer wake period
RTC_DATA_ATTR uint32_t idleOffSec;     // auto shutdown after idle (0 = disabled)
RTC_DATA_ATTR uint32_t cpuMHz;         // 80 / 160 / 240
RTC_DATA_ATTR bool     wakeByButton;
RTC_DATA_ATTR bool     wakeByTimer;
RTC_DATA_ATTR uint8_t  demoMode;       // Mode, run by click or "run"

void loadDefaults() {
  bootCount    = 0;
  midHoldMs    = 800;
  longPressMs  = 2000;
  sleepSeconds = 5;
  idleOffSec   = 0;
  cpuMHz       = 240;
  wakeByButton = true;
  wakeByTimer  = true;
  demoMode     = MODE_LIGHT;
}

// ============================ State ====================================
esp_sleep_wakeup_cause_t bootCause;
String   rxLine;
uint32_t lastActivity = 0;
bool     lastRaw = false, btnStable = false, longDone = false, midShown = false;
bool     clickPending = false;
bool     latched = true;      // LATCH pin state: true = battery path held on
uint32_t lastChange = 0, pressedAt = 0, clickAt = 0;

bool pressed() { return digitalRead(PIN_SENSE) == LOW; }
void ledWrite(bool on) { if (PIN_LED >= 0) digitalWrite(PIN_LED, on ? HIGH : LOW); }

const char* causeName(esp_sleep_wakeup_cause_t c) {
  switch (c) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "power-on / reset";
    case ESP_SLEEP_WAKEUP_TIMER:     return "timer";
    case ESP_SLEEP_WAKEUP_EXT0:      return "button (ext0)";
    case ESP_SLEEP_WAKEUP_EXT1:      return "button (ext1)";
    case ESP_SLEEP_WAKEUP_GPIO:      return "button (gpio)";
    default:                         return "other";
  }
}

const char* modeName(uint8_t m) {
  switch (m) {
    case MODE_ACTIVE: return "active";
    case MODE_LIGHT:  return "light";
    case MODE_DEEP:   return "deep";
    case MODE_HIB:    return "hib";
    default:          return "?";
  }
}

// After any blocking action, ignore a button that is still down until it is released.
void resyncButton() {
  lastRaw = btnStable = pressed();
  longDone = btnStable;
  clickPending = false;
  lastActivity = millis();
}

// ============================ True shutdown ============================
void powerOff(const char* why) {
  ledWrite(false);
  Serial.printf("[PWR] shutting down (%s). Release the button if held.\n", why);
  Serial.flush();
  while (pressed()) delay(10);       // gate is held low while SW1 is down, so wait for release
  delay(50);
  gpio_hold_dis((gpio_num_t)PIN_LATCH);
  digitalWrite(PIN_LATCH, LOW);      // Q2 off -> R1 pulls gate to VIN -> Q1 off -> 3V3 collapses
  delay(1500);

  // Only reached when another source (USB adapter, bench supply) keeps the MCU alive.
  Serial.println("[PWR] battery path is OFF, still running from USB/other source.");
  Serial.println("[PWR] press the button to re-latch the battery, or unplug USB to power off.");
  latched = false;
  resyncButton();
}

// ============================ Sleep demos ==============================
bool needWakeSource() {
  if (!wakeByButton && !wakeByTimer) {
    Serial.println("[SLEEP] refused: no wake source enabled (use 'wake b|t|bt').");
    return false;
  }
  return true;
}

void demoActive() {
  Serial.printf("[ACTIVE] staying awake at %lu MHz. Measure supply current now.\n",
                (unsigned long)getCpuFrequencyMhz());
}

void demoLight() {
  if (!needWakeSource()) return;
  while (pressed()) delay(10);       // low-level wake would fire immediately otherwise
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (wakeByTimer)  esp_sleep_enable_timer_wakeup((uint64_t)sleepSeconds * 1000000ULL);
  if (wakeByButton) {
    gpio_wakeup_enable((gpio_num_t)PIN_SENSE, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
  }
  Serial.printf("[LIGHT] sleeping (timer=%s %lus, button=%s)...\n",
                wakeByTimer ? "on" : "off", (unsigned long)sleepSeconds, wakeByButton ? "on" : "off");
  Serial.flush();
  uint32_t t0 = millis();
  esp_light_sleep_start();           // LATCH level is kept by the pad during light sleep
  Serial.printf("[LIGHT] woke after ~%lu ms by %s\n",
                (unsigned long)(millis() - t0), causeName(esp_sleep_get_wakeup_cause()));
  if (wakeByButton) gpio_wakeup_disable((gpio_num_t)PIN_SENSE);
  resyncButton();
}

void demoDeep(bool hibernate) {
  if (!needWakeSource()) return;
  while (pressed()) delay(10);
  ledWrite(false);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (wakeByTimer) esp_sleep_enable_timer_wakeup((uint64_t)sleepSeconds * 1000000ULL);

  if (hibernate) {
    // Everything optional off. RTC peripherals are off, so the pull-up must be external (R2).
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH,   ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
    if (wakeByButton) {
#ifdef CONFIG_IDF_TARGET_ESP32
      esp_sleep_enable_ext1_wakeup(1ULL << PIN_SENSE, ESP_EXT1_WAKEUP_ALL_LOW);
#else
      esp_sleep_enable_ext1_wakeup(1ULL << PIN_SENSE, ESP_EXT1_WAKEUP_ANY_LOW);
#endif
    }
  } else if (wakeByButton) {
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_SENSE, 0);   // wake on LOW
  }

  // Keep the latch pin frozen HIGH while the CPU is off, otherwise power would drop.
  gpio_hold_en((gpio_num_t)PIN_LATCH);
  gpio_deep_sleep_hold_en();

  Serial.printf("[%s] going to sleep (timer=%s %lus, button=%s). Device will reboot on wake.\n",
                hibernate ? "HIB" : "DEEP", wakeByTimer ? "on" : "off",
                (unsigned long)sleepSeconds, wakeByButton ? "on" : "off");
  Serial.flush();
  esp_deep_sleep_start();
}

void runDemo() {
  switch (demoMode) {
    case MODE_ACTIVE: demoActive();    break;
    case MODE_LIGHT:  demoLight();     break;
    case MODE_DEEP:   demoDeep(false); break;
    case MODE_HIB:    demoDeep(true);  break;
  }
}

// ============================ Serial commands ==========================
void printHelp() {
  Serial.println(F(
    "\nCommands:\n"
    "  help | status\n"
    "  mode active|light|deep|hib   select demo run by click / 'run'\n"
    "  run                       run selected demo now\n"
    "  time <sec>                sleep timer period\n"
    "  cpu <80|160|240>          CPU frequency (MHz)\n"
    "  wake b|t|bt               wake by button / timer / both\n"
    "  mid <ms> | hold <ms>      medium-hold / shutdown-hold thresholds\n"
    "  idle <sec>                auto shutdown after idle (0 = off)\n"
    "  off                       true shutdown now\n"
    "Button: click = run, double-click = next mode, hold = status, hold longer = power off\n"));
}

void printStatus() {
  Serial.printf("\n--- status ---\n"
                "boot #%lu, last wake: %s\n"
                "mode=%s  time=%lus  cpu=%luMHz  wake: button=%s timer=%s\n"
                "midHoldMs=%lu  longPressMs=%lu  idleOffSec=%lu  POWER_ON_HOLD_MS=%lu\n"
                "SENSE(GPIO%d)=%s  LATCH(GPIO%d)=%d (battery path %s)\n",
                (unsigned long)bootCount, causeName(bootCause),
                modeName(demoMode), (unsigned long)sleepSeconds, (unsigned long)getCpuFrequencyMhz(),
                wakeByButton ? "on" : "off", wakeByTimer ? "on" : "off",
                (unsigned long)midHoldMs, (unsigned long)longPressMs,
                (unsigned long)idleOffSec, (unsigned long)POWER_ON_HOLD_MS,
                PIN_SENSE, pressed() ? "PRESSED" : "released", PIN_LATCH, digitalRead(PIN_LATCH),
                latched ? "held on" : "off");
}

void runCommand(String line) {
  line.trim();
  line.toLowerCase();
  int sp = line.indexOf(' ');
  String cmd = (sp < 0) ? line : line.substring(0, sp);
  String arg = (sp < 0) ? ""   : line.substring(sp + 1);
  arg.trim();
  long n = arg.toInt();

  if      (cmd == "help")   printHelp();
  else if (cmd == "status") printStatus();
  else if (cmd == "run")    runDemo();
  else if (cmd == "off")    powerOff("serial command");
  else if (cmd == "mode") {
    if      (arg == "active") demoMode = MODE_ACTIVE;
    else if (arg == "light")  demoMode = MODE_LIGHT;
    else if (arg == "deep")   demoMode = MODE_DEEP;
    else if (arg == "hib")    demoMode = MODE_HIB;
    else { Serial.println("usage: mode active|light|deep|hib"); return; }
    Serial.printf("mode = %s\n", modeName(demoMode));
  }
  else if (cmd == "time") {
    if (n < 1) { Serial.println("usage: time <sec>=1.."); return; }
    sleepSeconds = (uint32_t)n;
    Serial.printf("time = %lus\n", (unsigned long)sleepSeconds);
  }
  else if (cmd == "cpu") {
    if (n != 80 && n != 160 && n != 240) { Serial.println("usage: cpu 80|160|240"); return; }
    cpuMHz = (uint32_t)n;
    setCpuFrequencyMhz(cpuMHz);
    Serial.printf("cpu = %luMHz\n", (unsigned long)getCpuFrequencyMhz());
  }
  else if (cmd == "wake") {
    if (arg != "b" && arg != "t" && arg != "bt" && arg != "tb") { Serial.println("usage: wake b|t|bt"); return; }
    wakeByButton = arg.indexOf('b') >= 0;
    wakeByTimer  = arg.indexOf('t') >= 0;
    Serial.printf("wake: button=%s timer=%s\n", wakeByButton ? "on" : "off", wakeByTimer ? "on" : "off");
  }
  else if (cmd == "mid") {
    if (n < 200 || (uint32_t)n >= longPressMs) { Serial.println("usage: mid <ms>, 200 <= ms < hold"); return; }
    midHoldMs = (uint32_t)n;
    Serial.printf("midHoldMs = %lu\n", (unsigned long)midHoldMs);
  }
  else if (cmd == "hold") {
    if ((uint32_t)n <= midHoldMs) { Serial.println("usage: hold <ms>, must be > mid"); return; }
    longPressMs = (uint32_t)n;
    Serial.printf("longPressMs = %lu\n", (unsigned long)longPressMs);
  }
  else if (cmd == "idle") {
    if (n < 0) { Serial.println("usage: idle <sec>=0.."); return; }
    idleOffSec = (uint32_t)n;
    Serial.printf("idleOffSec = %lu\n", (unsigned long)idleOffSec);
  }
  else Serial.println("unknown command, type 'help'");
}

void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    lastActivity = millis();
    if (c == '\n' || c == '\r') {
      if (rxLine.length()) { runCommand(rxLine); rxLine = ""; }
    } else if (rxLine.length() < 64) {
      rxLine += c;
    }
  }
}

// ============================ Button gestures ==========================
void onClick() {
  Serial.printf("[BTN] click -> run '%s'\n", modeName(demoMode));
  runDemo();
}

void onDoubleClick() {
  demoMode = (demoMode + 1) % NUM_MODES;
  Serial.printf("[BTN] double-click -> mode = %s\n", modeName(demoMode));
}

void handleButton() {
  uint32_t now = millis();
  bool raw = pressed();
  if (raw != lastRaw) { lastRaw = raw; lastChange = now; }

  if ((now - lastChange) >= DEBOUNCE_MS && raw != btnStable) {
    btnStable = raw;
    lastActivity = now;
    if (btnStable) {                           // press
      pressedAt = now;
      longDone = false;
      midShown = false;
      if (!latched) {                          // alive on USB, button = "power on" again
        digitalWrite(PIN_LATCH, HIGH);
        latched = true;
        longDone = true;                       // swallow this press as a gesture
        Serial.println("[PWR] battery path re-latched");
      }
    } else {                                   // release
      ledWrite(false);
      if (!longDone) {
        uint32_t held = now - pressedAt;
        if (held >= midHoldMs) {
          Serial.println("[BTN] medium hold released -> status");
          printStatus();
        } else if (DOUBLE_CLICK_MS == 0) {
          onClick();
        } else if (clickPending) {
          clickPending = false;
          onDoubleClick();
        } else {
          clickPending = true;
          clickAt = now;
        }
      }
    }
  }

  if (btnStable && !longDone) {
    uint32_t held = now - pressedAt;
    if (held >= longPressMs) {
      longDone = true;
      powerOff("long hold");
    } else if (held >= midHoldMs) {
      if (!midShown) {
        midShown = true;
        Serial.println("[BTN] hold: release = status, keep holding = power off");
      }
      // LED solid, then fast blink during the last 500 ms before shutdown
      if (longPressMs - held <= 500) ledWrite((now / 80) & 1);
      else                           ledWrite(true);
    }
  }

  if (clickPending && (now - clickAt) > DOUBLE_CLICK_MS) {
    clickPending = false;
    onClick();
  }
}

// ============================ Arduino entry points =====================
void setup() {
  // 1) FIRST THING: take over the latch before the user lets go of the button.
  pinMode(PIN_LATCH, OUTPUT);
  digitalWrite(PIN_LATCH, HIGH);           // level is already HIGH when the hold is released -> no glitch
  latched = true;
  gpio_hold_dis((gpio_num_t)PIN_LATCH);
  gpio_deep_sleep_hold_dis();

  pinMode(PIN_SENSE, INPUT);               // external 10k pull-up (R2)
  if (PIN_LED >= 0) { pinMode(PIN_LED, OUTPUT); ledWrite(false); }

  // 2) Settings and serial.
  if (cfgMagic != CFG_MAGIC) { loadDefaults(); cfgMagic = CFG_MAGIC; }
  bootCount++;
  setCpuFrequencyMhz(cpuMHz);
  Serial.begin(115200);
  delay(100);

  bootCause = esp_sleep_get_wakeup_cause();
  Serial.printf("\n\n=== soft-latch demo | boot #%lu | wake: %s ===\n",
                (unsigned long)bootCount, causeName(bootCause));

  // 3) Cold boot: optionally require the button to still be held (rejects accidental taps).
  if (bootCause == ESP_SLEEP_WAKEUP_UNDEFINED && POWER_ON_HOLD_MS > 0) {
    uint32_t t0 = millis();
    while (millis() - t0 < POWER_ON_HOLD_MS) {
      if (!pressed()) { powerOff("button released too early"); break; }
      delay(5);
    }
  }

  // 4) If the button is still down (power-on / wake press), ignore it until released.
  resyncButton();
  printHelp();
}

void loop() {
  handleButton();
  handleSerial();
  if (idleOffSec && (millis() - lastActivity) > idleOffSec * 1000UL) {
    powerOff("idle timeout");
  }
  delay(2);
}
