// M5StickC Sony camera remote tally.
// BtnA toggles record. BtnB toggles the display on a short press and cycles backgrounds on a long press.
// This version uses only Arduino ESP32 core libraries plus direct M5StickC hardware access.

#include <SPI.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <Wire.h>
#include <ctype.h>

#if __has_include("config.h")
#include "config.h"
static constexpr char CONFIG_SOURCE[] = "config.h";
#else
#include "config_fallback.h"
static constexpr char CONFIG_SOURCE[] = "config_fallback.h";
#endif

#ifndef M5_SONY_WIFI_SSID
#define M5_SONY_WIFI_SSID "SET_ME"
#endif

#ifndef M5_SONY_WIFI_PASSWORD
#define M5_SONY_WIFI_PASSWORD "SET_ME"
#endif

static constexpr char WIFI_SSID[] = M5_SONY_WIFI_SSID;
static constexpr char WIFI_PASSWORD[] = M5_SONY_WIFI_PASSWORD;

namespace {

constexpr char CAMERA_HOST[] = "192.168.122.1";
constexpr uint16_t CAMERA_PORT = 8080;
constexpr char CAMERA_PATH[] = "/sony/camera";

constexpr uint32_t POLL_INTERVAL_MS = 400;
constexpr uint32_t WIFI_SCAN_INTERVAL_MS = 2500;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t HTTP_TIMEOUT_MS = 4000;
constexpr uint32_t HTTP_CONNECT_STEP_TIMEOUT_MS = 120;
constexpr uint32_t START_REC_MODE_COOLDOWN_MS = 5000;
constexpr uint32_t RECORD_SETTLE_MS = 150;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 25;
constexpr uint32_t BUTTON_LONG_PRESS_MS = 700;
constexpr uint32_t BATTERY_REFRESH_MS = 5000;
constexpr uint32_t MAIN_LOOP_DELAY_MS = 10;
constexpr bool DIM_DISPLAY = true;
constexpr uint8_t DISPLAY_BRIGHTNESS_NORMAL_PERCENT = 80;
constexpr uint8_t DISPLAY_BRIGHTNESS_DIM_PERCENT = 35;
constexpr uint8_t DISPLAY_BRIGHTNESS_PERCENT =
    DIM_DISPLAY ? DISPLAY_BRIGHTNESS_DIM_PERCENT : DISPLAY_BRIGHTNESS_NORMAL_PERCENT;
constexpr bool SHOW_RECORDING_TIME = true;
constexpr bool SHOW_INFO_LINE = true;
// M5Stack documents the current StickC battery as 95 mAh; adjust to 80 for early 2019 units if needed.
constexpr uint16_t BATTERY_CAPACITY_MAH = 95;
constexpr uint16_t BATTERY_RUNTIME_MAX_MINUTES = 999;
constexpr size_t HTTP_READ_CHUNK_BYTES = 128;

constexpr int BUTTON_A_PIN = 37;
constexpr int BUTTON_B_PIN = 39;
constexpr int REC_LED_PIN = 10;
constexpr int REC_LED_ON = LOW;
constexpr int REC_LED_OFF = HIGH;

constexpr int TFT_MOSI_PIN = 15;
constexpr int TFT_SCLK_PIN = 13;
constexpr int TFT_CS_PIN = 5;
constexpr int TFT_DC_PIN = 23;
constexpr int TFT_RST_PIN = 18;

constexpr int PMU_SDA_PIN = 21;
constexpr int PMU_SCL_PIN = 22;
constexpr uint8_t PMU_I2C_ADDR = 0x34;

constexpr uint16_t COLOR_BLACK = 0x0000;
constexpr uint16_t COLOR_WHITE = 0xFFFF;
constexpr uint16_t COLOR_RED = 0xF800;
constexpr uint16_t COLOR_GREEN = 0x07E0;
constexpr uint16_t COLOR_BLUE = 0x001F;
constexpr uint16_t COLOR_CYAN = 0x07FF;
constexpr uint16_t COLOR_MAGENTA = 0xF81F;
constexpr uint16_t COLOR_YELLOW = 0xFFE0;
constexpr uint16_t COLOR_ORANGE = 0xFD20;
constexpr uint16_t COLOR_NAVY = 0x0013;
constexpr uint16_t COLOR_GRAPHITE = 0x4208;

enum class UiFontMode {
  Standard,
  Din2014Regular,
  Din2014Bold,
};

// Select the UI font here: built-in 3x5, DIN2014 regular, or DIN2014 bold.
constexpr UiFontMode UI_FONT_MODE = UiFontMode::Standard;
// Size multiplier for the selected UI font. Standard fits 1..3; DIN2014 is fixed at 1 on this layout.
constexpr uint8_t UI_FONT_SCALE = UI_FONT_MODE == UiFontMode::Standard ? 2 : 1;
static_assert(
    (UI_FONT_MODE == UiFontMode::Standard && UI_FONT_SCALE >= 1 && UI_FONT_SCALE <= 3) ||
        (UI_FONT_MODE != UiFontMode::Standard && UI_FONT_SCALE == 1),
    "UI_FONT_SCALE is out of range for the selected UI font.");

enum class ReadyState {
  Boot,
  NoCamera,
  CameraVisible,
  WifiConnected,
  CameraContact,
  Ready,
  Error,
};

struct CameraState {
  bool reachable = false;
  bool ready = false;
  bool has_start_movie_rec = false;
  bool has_stop_movie_rec = false;
  bool has_start_rec_mode = false;
  String camera_status;
};

struct AppState {
  ReadyState ready_state = ReadyState::Boot;
  bool camera_visible = false;
  bool have_wifi = false;
  bool display_enabled = true;
  bool button_busy = false;
  bool recording_timer_active = false;
  uint8_t theme_index = 0;
  uint32_t recording_started_ms = 0;
  CameraState camera;
  String last_error;
};

struct BatteryStatus {
  bool valid = false;
  uint16_t millivolts = 0;
  uint8_t percent = 0;
  bool charging = false;
  uint16_t discharge_tenths_milliamps = 0;
};

enum class CameraHttpPhase {
  Idle,
  ConnectAndSend,
  WaitResponse,
  ReadResponse,
};

enum class CameraJobKind {
  Idle,
  Poll,
  ToggleRecord,
};

enum class CameraJobStep {
  Idle,
  PollRequestEvent,
  PollWaitEvent,
  PollRequestStartRecMode,
  PollWaitStartRecMode,
  PollWaitAfterStartRecMode,
  ToggleEvaluate,
  ToggleRequestStartRecMode,
  ToggleWaitStartRecMode,
  ToggleWaitAfterStartRecMode,
  ToggleRequestEventAfterStartRecMode,
  ToggleWaitEventAfterStartRecMode,
  ToggleRequestCommand,
  ToggleWaitCommand,
  ToggleWaitAfterCommand,
  ToggleRequestEventAfterCommand,
  ToggleWaitEventAfterCommand,
};

struct CameraHttpRequest {
  CameraHttpPhase phase = CameraHttpPhase::Idle;
  const char* method = nullptr;
  const char* params_json = "[]";
  uint8_t version_index = 0;
  uint32_t phase_started_ms = 0;
  bool completed = false;
  bool success = false;
  String json_payload;
  String request_text;
  String raw_response;
  String response_body;
  String error;
  WiFiClient client;
};

struct CameraJob {
  CameraJobKind kind = CameraJobKind::Idle;
  CameraJobStep step = CameraJobStep::Idle;
  uint32_t wait_until_ms = 0;
  const char* command_method = nullptr;
  bool attempted_start_rec_mode = false;
  CameraState working_state;
};

struct ThemeOption {
  const char* name;
  uint16_t background;
};

struct BitmapGlyph {
  char ch;
  uint8_t width;
  uint8_t rows[10];
};

struct BitmapFontView {
  const BitmapGlyph* glyphs;
  size_t count;
  uint8_t height;
  uint8_t top_trim;
  uint8_t bottom_trim;
};

struct DisplaySnapshot {
  uint16_t background = COLOR_BLACK;
  String title;
  uint16_t title_color = COLOR_WHITE;
  bool battery_valid = false;
  uint8_t battery_percent = 0;
  bool battery_charging = false;
  uint16_t battery_outline_color = COLOR_WHITE;
  uint16_t battery_fill_color = COLOR_GREEN;
  uint16_t battery_charge_color = COLOR_YELLOW;
  String ready;
  uint16_t ready_color = COLOR_WHITE;
  String net;
  uint16_t net_color = COLOR_WHITE;
  String camera;
  uint16_t camera_color = COLOR_WHITE;
  String footer;
  uint16_t footer_color = COLOR_WHITE;
};

constexpr ThemeOption THEME_OPTIONS[] = {
    {"BLACK", COLOR_BLACK},
    {"NAVY", COLOR_NAVY},
    {"GRAPHITE", COLOR_GRAPHITE},
    {"WHITE", COLOR_WHITE},
};
constexpr size_t THEME_OPTION_COUNT = sizeof(THEME_OPTIONS) / sizeof(THEME_OPTIONS[0]);

constexpr char SETTINGS_NAMESPACE[] = "sony_tally";
constexpr char THEME_PREF_KEY[] = "bg";
constexpr char THEME_PREF_VERSION_KEY[] = "bgv";
constexpr uint8_t THEME_PREF_VERSION = 2;

constexpr int16_t TOP_ROW_HEIGHT = 16;
constexpr int16_t STATUS_SLOT_HEIGHT = 16;
constexpr int16_t STATUS_ROW_HEIGHT = 12;
constexpr int16_t TITLE_Y = 4;
constexpr int16_t BATTERY_Y = 3;
constexpr int16_t READY_Y = 20;
constexpr int16_t NET_Y = 36;
constexpr int16_t CAMERA_Y = 52;
constexpr int16_t FOOTER_Y = 68;
constexpr int16_t BATTERY_BODY_WIDTH = 18;
constexpr int16_t BATTERY_BODY_HEIGHT = 10;
constexpr int16_t BATTERY_CAP_WIDTH = 2;
constexpr int16_t BATTERY_CAP_HEIGHT = 5;

static constexpr uint8_t DIN2014_REGULAR_GLYPHS_HEIGHT = 10;
static constexpr BitmapGlyph DIN2014_REGULAR_GLYPHS[] = {
    {' ', 2, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'A', 6, {0x00, 0x08, 0x08, 0x14, 0x14, 0x14, 0x1E, 0x22, 0x00, 0x00}},
    {'B', 5, {0x00, 0x0F, 0x09, 0x09, 0x0E, 0x09, 0x09, 0x0E, 0x00, 0x00}},
    {'C', 5, {0x00, 0x07, 0x08, 0x08, 0x08, 0x08, 0x08, 0x07, 0x00, 0x00}},
    {'D', 5, {0x00, 0x0E, 0x09, 0x09, 0x09, 0x09, 0x09, 0x0E, 0x00, 0x00}},
    {'E', 5, {0x00, 0x0F, 0x08, 0x08, 0x0F, 0x08, 0x08, 0x0F, 0x00, 0x00}},
    {'F', 5, {0x00, 0x0F, 0x08, 0x08, 0x0F, 0x08, 0x08, 0x08, 0x00, 0x00}},
    {'G', 5, {0x00, 0x07, 0x08, 0x08, 0x09, 0x08, 0x08, 0x07, 0x00, 0x00}},
    {'H', 6, {0x00, 0x12, 0x12, 0x12, 0x1E, 0x12, 0x12, 0x12, 0x00, 0x00}},
    {'I', 3, {0x00, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x00, 0x00}},
    {'J', 5, {0x00, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x1C, 0x00, 0x00}},
    {'K', 7, {0x00, 0x24, 0x24, 0x28, 0x38, 0x24, 0x24, 0x22, 0x00, 0x00}},
    {'L', 5, {0x00, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x00, 0x00}},
    {'M', 7, {0x00, 0x22, 0x36, 0x36, 0x3A, 0x2A, 0x2A, 0x22, 0x00, 0x00}},
    {'N', 6, {0x00, 0x12, 0x1A, 0x1A, 0x1A, 0x16, 0x16, 0x12, 0x00, 0x00}},
    {'O', 5, {0x00, 0x07, 0x08, 0x08, 0x08, 0x08, 0x08, 0x07, 0x00, 0x00}},
    {'P', 5, {0x00, 0x0E, 0x09, 0x09, 0x09, 0x0E, 0x08, 0x08, 0x00, 0x00}},
    {'Q', 6, {0x00, 0x0E, 0x11, 0x11, 0x11, 0x11, 0x13, 0x0F, 0x00, 0x00}},
    {'R', 6, {0x00, 0x1C, 0x12, 0x12, 0x1C, 0x14, 0x12, 0x12, 0x00, 0x00}},
    {'S', 5, {0x00, 0x07, 0x04, 0x04, 0x03, 0x00, 0x04, 0x07, 0x00, 0x00}},
    {'T', 5, {0x00, 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x00}},
    {'U', 7, {0x00, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x1C, 0x00, 0x00}},
    {'V', 6, {0x00, 0x22, 0x22, 0x12, 0x14, 0x14, 0x0C, 0x08, 0x00, 0x00}},
    {'W', 8, {0x00, 0x92, 0x9A, 0x9A, 0x6A, 0x6A, 0x64, 0x64, 0x00, 0x00}},
    {'X', 5, {0x00, 0x12, 0x0A, 0x0C, 0x04, 0x0E, 0x0A, 0x11, 0x00, 0x00}},
    {'Y', 5, {0x00, 0x11, 0x0A, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x00, 0x00}},
    {'Z', 6, {0x00, 0x1E, 0x02, 0x04, 0x08, 0x08, 0x10, 0x1E, 0x00, 0x00}},
    {'0', 5, {0x00, 0x06, 0x09, 0x09, 0x09, 0x09, 0x09, 0x06, 0x00, 0x00}},
    {'1', 5, {0x00, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x00}},
    {'2', 5, {0x00, 0x06, 0x09, 0x01, 0x02, 0x04, 0x0C, 0x0F, 0x00, 0x00}},
    {'3', 5, {0x00, 0x06, 0x09, 0x01, 0x06, 0x01, 0x09, 0x06, 0x00, 0x00}},
    {'4', 5, {0x00, 0x04, 0x04, 0x08, 0x0A, 0x0A, 0x1F, 0x02, 0x00, 0x00}},
    {'5', 5, {0x00, 0x0F, 0x08, 0x0E, 0x01, 0x01, 0x09, 0x06, 0x00, 0x00}},
    {'6', 5, {0x00, 0x02, 0x04, 0x04, 0x0E, 0x09, 0x09, 0x06, 0x00, 0x00}},
    {'7', 5, {0x00, 0x0F, 0x0A, 0x02, 0x02, 0x04, 0x04, 0x04, 0x00, 0x00}},
    {'8', 5, {0x00, 0x06, 0x09, 0x09, 0x06, 0x09, 0x09, 0x06, 0x00, 0x00}},
    {'9', 5, {0x00, 0x06, 0x09, 0x09, 0x07, 0x02, 0x02, 0x04, 0x00, 0x00}},
    {'-', 3, {0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00}},
    {':', 2, {0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00}},
    {'.', 1, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'/', 5, {0x02, 0x02, 0x04, 0x04, 0x04, 0x08, 0x08, 0x08, 0x10, 0x00}},
    {'?', 4, {0x00, 0x03, 0x04, 0x00, 0x01, 0x02, 0x00, 0x02, 0x00, 0x00}},
};

static constexpr uint8_t DIN2014_BOLD_GLYPHS_HEIGHT = 10;
static constexpr BitmapGlyph DIN2014_BOLD_GLYPHS[] = {
    {' ', 2, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'A', 7, {0x00, 0x10, 0x18, 0x38, 0x28, 0x2C, 0x7C, 0x64, 0x00, 0x00}},
    {'B', 5, {0x00, 0x0F, 0x09, 0x09, 0x0E, 0x09, 0x09, 0x0E, 0x00, 0x00}},
    {'C', 6, {0x00, 0x0E, 0x12, 0x10, 0x10, 0x10, 0x12, 0x0E, 0x00, 0x00}},
    {'D', 5, {0x00, 0x0E, 0x09, 0x09, 0x09, 0x09, 0x09, 0x0E, 0x00, 0x00}},
    {'E', 5, {0x00, 0x0F, 0x08, 0x08, 0x0E, 0x08, 0x08, 0x0F, 0x00, 0x00}},
    {'F', 5, {0x00, 0x0F, 0x08, 0x08, 0x0E, 0x08, 0x08, 0x08, 0x00, 0x00}},
    {'G', 6, {0x00, 0x0E, 0x11, 0x10, 0x13, 0x11, 0x11, 0x0E, 0x00, 0x00}},
    {'H', 6, {0x00, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11, 0x00, 0x00}},
    {'I', 3, {0x00, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x00, 0x00}},
    {'J', 5, {0x00, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0C, 0x00, 0x00}},
    {'K', 6, {0x00, 0x12, 0x14, 0x1C, 0x1C, 0x14, 0x12, 0x13, 0x00, 0x00}},
    {'L', 5, {0x00, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x00, 0x00}},
    {'M', 8, {0x00, 0x42, 0x66, 0x66, 0x7E, 0x5A, 0x5A, 0x42, 0x00, 0x00}},
    {'N', 7, {0x00, 0x22, 0x32, 0x3A, 0x2A, 0x2E, 0x26, 0x22, 0x00, 0x00}},
    {'O', 6, {0x00, 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E, 0x00, 0x00}},
    {'P', 5, {0x00, 0x0E, 0x09, 0x09, 0x09, 0x0E, 0x08, 0x08, 0x00, 0x00}},
    {'Q', 7, {0x00, 0x1C, 0x22, 0x22, 0x22, 0x26, 0x26, 0x1F, 0x00, 0x00}},
    {'R', 6, {0x00, 0x1C, 0x12, 0x12, 0x1E, 0x14, 0x12, 0x12, 0x00, 0x00}},
    {'S', 5, {0x00, 0x07, 0x09, 0x08, 0x07, 0x01, 0x09, 0x0E, 0x00, 0x00}},
    {'T', 7, {0x00, 0x3E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x00, 0x00}},
    {'U', 7, {0x00, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x1C, 0x00, 0x00}},
    {'V', 6, {0x00, 0x32, 0x32, 0x16, 0x14, 0x1C, 0x0C, 0x0C, 0x00, 0x00}},
    {'W', 8, {0x00, 0x9B, 0xDB, 0x5A, 0x5A, 0x66, 0x66, 0x66, 0x00, 0x00}},
    {'X', 6, {0x00, 0x16, 0x14, 0x1C, 0x0C, 0x1C, 0x14, 0x32, 0x00, 0x00}},
    {'Y', 6, {0x00, 0x1B, 0x0A, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x00, 0x00}},
    {'Z', 6, {0x00, 0x1E, 0x06, 0x04, 0x0C, 0x08, 0x18, 0x1E, 0x00, 0x00}},
    {'0', 5, {0x00, 0x06, 0x09, 0x09, 0x09, 0x09, 0x09, 0x06, 0x00, 0x00}},
    {'1', 5, {0x00, 0x0C, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x00}},
    {'2', 5, {0x00, 0x06, 0x09, 0x01, 0x03, 0x06, 0x04, 0x0F, 0x00, 0x00}},
    {'3', 5, {0x00, 0x0C, 0x1A, 0x02, 0x04, 0x02, 0x12, 0x0C, 0x00, 0x00}},
    {'4', 6, {0x00, 0x04, 0x04, 0x0C, 0x0A, 0x1A, 0x1F, 0x02, 0x00, 0x00}},
    {'5', 5, {0x00, 0x0F, 0x08, 0x0E, 0x09, 0x01, 0x09, 0x0E, 0x00, 0x00}},
    {'6', 5, {0x00, 0x06, 0x06, 0x04, 0x0E, 0x09, 0x09, 0x06, 0x00, 0x00}},
    {'7', 5, {0x00, 0x0F, 0x0B, 0x02, 0x02, 0x06, 0x06, 0x04, 0x00, 0x00}},
    {'8', 5, {0x00, 0x06, 0x09, 0x09, 0x06, 0x09, 0x09, 0x06, 0x00, 0x00}},
    {'9', 5, {0x00, 0x06, 0x09, 0x09, 0x07, 0x02, 0x06, 0x06, 0x00, 0x00}},
    {'-', 4, {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00}},
    {':', 2, {0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00}},
    {'.', 3, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00}},
    {'/', 5, {0x02, 0x02, 0x02, 0x04, 0x04, 0x04, 0x08, 0x08, 0x08, 0x00}},
    {'?', 4, {0x00, 0x07, 0x05, 0x01, 0x03, 0x02, 0x00, 0x02, 0x00, 0x00}},
};

class DebouncedButton {
 public:
  explicit DebouncedButton(int pin, bool click_on_press = false, bool suppress_click_on_long_press = false)
      : pin_(pin),
        click_on_press_(click_on_press),
        suppress_click_on_long_press_(suppress_click_on_long_press) {}

  void begin() {
    pinMode(pin_, INPUT);
    stable_pressed_ = rawPressed();
    last_raw_pressed_ = stable_pressed_;
    last_change_ms_ = millis();
    armed_ = !stable_pressed_;
    press_started_ms_ = stable_pressed_ ? millis() : 0;
  }

  void tick() {
    const uint32_t now = millis();
    const bool raw_pressed = rawPressed();
    if (raw_pressed != last_raw_pressed_) {
      last_raw_pressed_ = raw_pressed;
      last_change_ms_ = now;
    }

    if (raw_pressed != stable_pressed_ && now - last_change_ms_ >= BUTTON_DEBOUNCE_MS) {
      stable_pressed_ = raw_pressed;
      if (stable_pressed_) {
        press_started_ms_ = now;
        long_press_reported_ = false;
        if (click_on_press_ && armed_) {
          click_pending_ = true;
        }
      } else {
        if (armed_ && !click_on_press_ && press_started_ms_ != 0 && !long_press_reported_) {
          click_pending_ = true;
        } else if (click_on_press_ && suppress_click_on_long_press_ && long_press_reported_) {
          click_pending_ = false;
        }
        armed_ = true;
        press_started_ms_ = 0;
        long_press_reported_ = false;
      }
    }

    if (stable_pressed_ && armed_ && !long_press_reported_ && press_started_ms_ != 0 &&
        now - press_started_ms_ >= BUTTON_LONG_PRESS_MS) {
      long_press_pending_ = true;
      long_press_reported_ = true;
    }
  }

  bool consumeClick() {
    if (!click_pending_) {
      return false;
    }
    click_pending_ = false;
    return true;
  }

  bool consumeLongPress() {
    if (!long_press_pending_) {
      return false;
    }
    long_press_pending_ = false;
    return true;
  }

 private:
  bool rawPressed() const {
    return digitalRead(pin_) == LOW;
  }

  const int pin_;
  const bool click_on_press_;
  const bool suppress_click_on_long_press_;
  bool stable_pressed_ = false;
  bool last_raw_pressed_ = false;
  bool armed_ = false;
  bool click_pending_ = false;
  bool long_press_pending_ = false;
  bool long_press_reported_ = false;
  uint32_t last_change_ms_ = 0;
  uint32_t press_started_ms_ = 0;
};

class PmuControl {
 public:
  void begin() {
    wire_.begin(PMU_SDA_PIN, PMU_SCL_PIN, 400000);
    writeByte(0x10, 0xFF);
    writeByte(0x28, 0xCC);
    writeByte(0x84, 0xF2);
    writeByte(0x82, 0xFF);
    writeByte(0x33, 0xC0);
    uint8_t buf = (readByte(0x12) & 0xEF) | 0x4D;
    writeByte(0x12, buf);
    writeByte(0x36, 0x0C);
    writeByte(0x91, 0xA0);
    writeByte(0x90, 0x02);
    writeByte(0x30, 0x80);
    writeByte(0x39, 0xFC);
    writeByte(0x35, 0xA2);
    writeByte(0x32, 0x46);
    writeByte(0x31, (readByte(0x31) & 0xF8) | (1 << 2));
    screenBreath(DISPLAY_BRIGHTNESS_PERCENT);
  }

  void screenBreath(uint8_t brightness_percent) {
    if (brightness_percent > 100) {
      brightness_percent = 100;
    }
    int voltage = map(brightness_percent, 0, 100, 2500, 3200);
    voltage = (voltage < 1800) ? 0 : (voltage - 1800) / 100;
    const uint8_t buf = readByte(0x28);
    writeByte(0x28, (buf & 0x0F) | (static_cast<uint8_t>(voltage) << 4));
  }

  void screenSwitch(bool enabled) {
    if (!enabled) {
      const uint8_t buf = readByte(0x28);
      writeByte(0x28, (buf & 0x0F));
      return;
    }
    screenBreath(DISPLAY_BRIGHTNESS_PERCENT);
  }

  uint16_t batteryMillivolts() {
    const uint16_t raw = read12Bit(0x78);
    if (raw == 0) {
      return 0;
    }
    return static_cast<uint16_t>((static_cast<uint32_t>(raw) * 11 + 5) / 10);
  }

  uint16_t batteryDischargeTenthsMilliamps() {
    const uint16_t raw = read13Bit(0x7C);
    return static_cast<uint16_t>(raw * 5);
  }

  uint8_t powerModeStatus() {
    return readByte(0x01);
  }

  bool isCharging() {
    return (powerModeStatus() & 0x40) != 0;
  }

  bool batteryConnected() {
    return (powerModeStatus() & 0x20) != 0;
  }

 private:
  void writeByte(uint8_t addr, uint8_t data) {
    wire_.beginTransmission(PMU_I2C_ADDR);
    wire_.write(addr);
    wire_.write(data);
    wire_.endTransmission();
  }

  uint8_t readByte(uint8_t addr) {
    wire_.beginTransmission(PMU_I2C_ADDR);
    wire_.write(addr);
    wire_.endTransmission();
    wire_.requestFrom(static_cast<int>(PMU_I2C_ADDR), 1);
    if (!wire_.available()) {
      return 0;
    }
    return wire_.read();
  }

  uint16_t read12Bit(uint8_t addr) {
    const uint8_t high = readByte(addr);
    const uint8_t low = readByte(addr + 1);
    return (static_cast<uint16_t>(high) << 4) | (low & 0x0F);
  }

  uint16_t read13Bit(uint8_t addr) {
    const uint8_t high = readByte(addr);
    const uint8_t low = readByte(addr + 1);
    return (static_cast<uint16_t>(high) << 5) | (low & 0x1F);
  }

  TwoWire wire_ = TwoWire(1);
};

class StickDisplay {
 public:
  void begin() {
    pinMode(TFT_CS_PIN, OUTPUT);
    pinMode(TFT_DC_PIN, OUTPUT);
    pinMode(TFT_RST_PIN, OUTPUT);
    digitalWrite(TFT_CS_PIN, HIGH);
    digitalWrite(TFT_DC_PIN, HIGH);
    digitalWrite(TFT_RST_PIN, HIGH);

    spi_ = new SPIClass(HSPI);
    spi_->begin(TFT_SCLK_PIN, -1, TFT_MOSI_PIN, TFT_CS_PIN);

    reset();
    initPanel();
    setRotation(3);
    fillScreen(COLOR_BLACK);
  }

  void fillScreen(uint16_t color) {
    fillRect(0, 0, width_, height_, color);
  }

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (w <= 0 || h <= 0) {
      return;
    }
    if (x < 0) {
      w += x;
      x = 0;
    }
    if (y < 0) {
      h += y;
      y = 0;
    }
    if (x + w > width_) {
      w = width_ - x;
    }
    if (y + h > height_) {
      h = height_ - y;
    }
    if (w <= 0 || h <= 0) {
      return;
    }

    setAddrWindow(x, y, x + w - 1, y + h - 1);
    beginData();
    const uint16_t panel_color = panelColor(color);
    const uint8_t hi = static_cast<uint8_t>(panel_color >> 8);
    const uint8_t lo = static_cast<uint8_t>(panel_color & 0xFF);
    const uint32_t count = static_cast<uint32_t>(w) * static_cast<uint32_t>(h);
    for (uint32_t index = 0; index < count; ++index) {
      spi_->transfer(hi);
      spi_->transfer(lo);
    }
    endWrite();
  }

  void drawText(int16_t x, int16_t y, const String& text, uint16_t color, uint8_t scale) {
    int16_t cursor_x = x;
    for (size_t index = 0; index < static_cast<size_t>(text.length()); ++index) {
      drawChar(cursor_x, y, text[index], color, scale);
      cursor_x += static_cast<int16_t>(4 * scale);
    }
  }

  int16_t width() const {
    return width_;
  }

 private:
  uint16_t panelColor(uint16_t color) const {
    return static_cast<uint16_t>((color << 8) | (color >> 8));
  }

  void reset() {
    digitalWrite(TFT_RST_PIN, HIGH);
    delay(5);
    digitalWrite(TFT_RST_PIN, LOW);
    delay(20);
    digitalWrite(TFT_RST_PIN, HIGH);
    delay(150);
  }

  void initPanel() {
    writeCommand(0x01);
    delay(150);
    writeCommand(0x11);
    delay(255);

    writeCommandData(0xB1, {0x01, 0x2C, 0x2D});
    writeCommandData(0xB2, {0x01, 0x2C, 0x2D});
    writeCommandData(0xB3, {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D});
    writeCommandData(0xB4, {0x07});
    writeCommandData(0xC0, {0xA2, 0x02, 0x84});
    writeCommandData(0xC1, {0xC5});
    writeCommandData(0xC2, {0x0A, 0x00});
    writeCommandData(0xC3, {0x8A, 0x2A});
    writeCommandData(0xC4, {0x8A, 0xEE});
    writeCommandData(0xC5, {0x0E});
    writeCommandData(0x20, {});
    writeCommandData(0x3A, {0x05});
    writeCommandData(0xE0, {0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10});
    writeCommandData(0xE1, {0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10});
    writeCommand(0x13);
    delay(10);
    writeCommand(0x29);
    delay(100);
  }

  void setRotation(uint8_t rotation) {
    rotation &= 3;
    uint8_t madctl = 0x08;
    switch (rotation) {
      case 0:
        madctl = 0xC8;
        width_ = 80;
        height_ = 160;
        x_offset_ = 26;
        y_offset_ = 1;
        break;
      case 1:
        madctl = 0xA8;
        width_ = 160;
        height_ = 80;
        x_offset_ = 1;
        y_offset_ = 26;
        break;
      case 2:
        madctl = 0x08;
        width_ = 80;
        height_ = 160;
        x_offset_ = 26;
        y_offset_ = 1;
        break;
      default:
        madctl = 0x68;
        width_ = 160;
        height_ = 80;
        x_offset_ = 1;
        y_offset_ = 26;
        break;
    }
    writeCommandData(0x36, {madctl});
  }

  void setAddrWindow(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    const uint16_t xs = static_cast<uint16_t>(x0 + x_offset_);
    const uint16_t xe = static_cast<uint16_t>(x1 + x_offset_);
    const uint16_t ys = static_cast<uint16_t>(y0 + y_offset_);
    const uint16_t ye = static_cast<uint16_t>(y1 + y_offset_);
    writeCommandData(0x2A, {static_cast<uint8_t>(xs >> 8), static_cast<uint8_t>(xs & 0xFF), static_cast<uint8_t>(xe >> 8), static_cast<uint8_t>(xe & 0xFF)});
    writeCommandData(0x2B, {static_cast<uint8_t>(ys >> 8), static_cast<uint8_t>(ys & 0xFF), static_cast<uint8_t>(ye >> 8), static_cast<uint8_t>(ye & 0xFF)});
    writeCommand(0x2C);
  }

  void drawChar(int16_t x, int16_t y, char ch, uint16_t color, uint8_t scale) {
    uint8_t rows[5];
    glyphRows(ch, rows);
    for (uint8_t row = 0; row < 5; ++row) {
      for (uint8_t col = 0; col < 3; ++col) {
        if ((rows[row] & (1 << (2 - col))) == 0) {
          continue;
        }
        fillRect(x + static_cast<int16_t>(col * scale), y + static_cast<int16_t>(row * scale), scale, scale, color);
      }
    }
  }

  void glyphRows(char input, uint8_t rows[5]) {
    memset(rows, 0, 5);
    const char ch = static_cast<char>(toupper(static_cast<unsigned char>(input)));
    switch (ch) {
      case 'A': rows[0] = 0b010; rows[1] = 0b101; rows[2] = 0b111; rows[3] = 0b101; rows[4] = 0b101; break;
      case 'B': rows[0] = 0b110; rows[1] = 0b101; rows[2] = 0b110; rows[3] = 0b101; rows[4] = 0b110; break;
      case 'C': rows[0] = 0b011; rows[1] = 0b100; rows[2] = 0b100; rows[3] = 0b100; rows[4] = 0b011; break;
      case 'D': rows[0] = 0b110; rows[1] = 0b101; rows[2] = 0b101; rows[3] = 0b101; rows[4] = 0b110; break;
      case 'E': rows[0] = 0b111; rows[1] = 0b100; rows[2] = 0b110; rows[3] = 0b100; rows[4] = 0b111; break;
      case 'F': rows[0] = 0b111; rows[1] = 0b100; rows[2] = 0b110; rows[3] = 0b100; rows[4] = 0b100; break;
      case 'G': rows[0] = 0b011; rows[1] = 0b100; rows[2] = 0b101; rows[3] = 0b101; rows[4] = 0b011; break;
      case 'H': rows[0] = 0b101; rows[1] = 0b101; rows[2] = 0b111; rows[3] = 0b101; rows[4] = 0b101; break;
      case 'I': rows[0] = 0b111; rows[1] = 0b010; rows[2] = 0b010; rows[3] = 0b010; rows[4] = 0b111; break;
      case 'J': rows[0] = 0b001; rows[1] = 0b001; rows[2] = 0b001; rows[3] = 0b101; rows[4] = 0b010; break;
      case 'K': rows[0] = 0b101; rows[1] = 0b101; rows[2] = 0b110; rows[3] = 0b101; rows[4] = 0b101; break;
      case 'L': rows[0] = 0b100; rows[1] = 0b100; rows[2] = 0b100; rows[3] = 0b100; rows[4] = 0b111; break;
      case 'M': rows[0] = 0b101; rows[1] = 0b111; rows[2] = 0b111; rows[3] = 0b101; rows[4] = 0b101; break;
      case 'N': rows[0] = 0b101; rows[1] = 0b111; rows[2] = 0b111; rows[3] = 0b111; rows[4] = 0b101; break;
      case 'O': rows[0] = 0b010; rows[1] = 0b101; rows[2] = 0b101; rows[3] = 0b101; rows[4] = 0b010; break;
      case 'P': rows[0] = 0b110; rows[1] = 0b101; rows[2] = 0b110; rows[3] = 0b100; rows[4] = 0b100; break;
      case 'Q': rows[0] = 0b010; rows[1] = 0b101; rows[2] = 0b101; rows[3] = 0b010; rows[4] = 0b001; break;
      case 'R': rows[0] = 0b110; rows[1] = 0b101; rows[2] = 0b110; rows[3] = 0b101; rows[4] = 0b101; break;
      case 'S': rows[0] = 0b011; rows[1] = 0b100; rows[2] = 0b010; rows[3] = 0b001; rows[4] = 0b110; break;
      case 'T': rows[0] = 0b111; rows[1] = 0b010; rows[2] = 0b010; rows[3] = 0b010; rows[4] = 0b010; break;
      case 'U': rows[0] = 0b101; rows[1] = 0b101; rows[2] = 0b101; rows[3] = 0b101; rows[4] = 0b111; break;
      case 'V': rows[0] = 0b101; rows[1] = 0b101; rows[2] = 0b101; rows[3] = 0b101; rows[4] = 0b010; break;
      case 'W': rows[0] = 0b101; rows[1] = 0b101; rows[2] = 0b111; rows[3] = 0b111; rows[4] = 0b101; break;
      case 'X': rows[0] = 0b101; rows[1] = 0b101; rows[2] = 0b010; rows[3] = 0b101; rows[4] = 0b101; break;
      case 'Y': rows[0] = 0b101; rows[1] = 0b101; rows[2] = 0b010; rows[3] = 0b010; rows[4] = 0b010; break;
      case 'Z': rows[0] = 0b111; rows[1] = 0b001; rows[2] = 0b010; rows[3] = 0b100; rows[4] = 0b111; break;
      case '0': rows[0] = 0b111; rows[1] = 0b101; rows[2] = 0b101; rows[3] = 0b101; rows[4] = 0b111; break;
      case '1': rows[0] = 0b010; rows[1] = 0b110; rows[2] = 0b010; rows[3] = 0b010; rows[4] = 0b111; break;
      case '2': rows[0] = 0b110; rows[1] = 0b001; rows[2] = 0b111; rows[3] = 0b100; rows[4] = 0b111; break;
      case '3': rows[0] = 0b110; rows[1] = 0b001; rows[2] = 0b111; rows[3] = 0b001; rows[4] = 0b110; break;
      case '4': rows[0] = 0b101; rows[1] = 0b101; rows[2] = 0b111; rows[3] = 0b001; rows[4] = 0b001; break;
      case '5': rows[0] = 0b111; rows[1] = 0b100; rows[2] = 0b111; rows[3] = 0b001; rows[4] = 0b110; break;
      case '6': rows[0] = 0b011; rows[1] = 0b100; rows[2] = 0b110; rows[3] = 0b101; rows[4] = 0b010; break;
      case '7': rows[0] = 0b111; rows[1] = 0b001; rows[2] = 0b010; rows[3] = 0b100; rows[4] = 0b100; break;
      case '8': rows[0] = 0b111; rows[1] = 0b101; rows[2] = 0b111; rows[3] = 0b101; rows[4] = 0b111; break;
      case '9': rows[0] = 0b010; rows[1] = 0b101; rows[2] = 0b011; rows[3] = 0b001; rows[4] = 0b110; break;
      case '-': rows[0] = 0b000; rows[1] = 0b000; rows[2] = 0b111; rows[3] = 0b000; rows[4] = 0b000; break;
      case ':': rows[0] = 0b000; rows[1] = 0b010; rows[2] = 0b000; rows[3] = 0b010; rows[4] = 0b000; break;
      case '.': rows[0] = 0b000; rows[1] = 0b000; rows[2] = 0b000; rows[3] = 0b000; rows[4] = 0b010; break;
      case '/': rows[0] = 0b001; rows[1] = 0b001; rows[2] = 0b010; rows[3] = 0b100; rows[4] = 0b100; break;
      case '?': rows[0] = 0b110; rows[1] = 0b001; rows[2] = 0b010; rows[3] = 0b000; rows[4] = 0b010; break;
      default: break;
    }
  }

  void writeCommand(uint8_t command) {
    beginWrite();
    digitalWrite(TFT_DC_PIN, LOW);
    spi_->transfer(command);
    endWrite();
  }

  void writeCommandData(uint8_t command, std::initializer_list<uint8_t> data) {
    beginWrite();
    digitalWrite(TFT_DC_PIN, LOW);
    spi_->transfer(command);
    if (data.size() > 0) {
      digitalWrite(TFT_DC_PIN, HIGH);
      for (uint8_t value : data) {
        spi_->transfer(value);
      }
    }
    endWrite();
  }

  void beginData() {
    beginWrite();
    digitalWrite(TFT_DC_PIN, HIGH);
  }

  void beginWrite() {
    spi_->beginTransaction(SPISettings(27000000, MSBFIRST, SPI_MODE0));
    digitalWrite(TFT_CS_PIN, LOW);
  }

  void endWrite() {
    digitalWrite(TFT_CS_PIN, HIGH);
    spi_->endTransaction();
  }

  SPIClass* spi_ = nullptr;
  int16_t width_ = 160;
  int16_t height_ = 80;
  int16_t x_offset_ = 1;
  int16_t y_offset_ = 26;
};

AppState app;
BatteryStatus battery_status;
DebouncedButton button_a(BUTTON_A_PIN, true);
DebouncedButton button_b(BUTTON_B_PIN);
PmuControl pmu;
StickDisplay display;
Preferences preferences;
uint32_t last_wifi_scan_ms = 0;
uint32_t wifi_connect_started_ms = 0;
uint32_t last_poll_ms = 0;
uint32_t last_start_rec_mode_ms = 0;
uint32_t last_battery_refresh_ms = 0;
bool wifi_scan_in_progress = false;
bool record_toggle_requested = false;
CameraHttpRequest camera_http;
CameraJob camera_job;
bool have_last_display = false;
DisplaySnapshot last_display;

void cycleTheme();
void setDisplayEnabled(bool enabled);

const BitmapFontView* activeBitmapFont() {
  static const BitmapFontView kRegularFont = {
      DIN2014_REGULAR_GLYPHS,
      sizeof(DIN2014_REGULAR_GLYPHS) / sizeof(DIN2014_REGULAR_GLYPHS[0]),
      DIN2014_REGULAR_GLYPHS_HEIGHT,
      1,
      2,
  };
  static const BitmapFontView kBoldFont = {
      DIN2014_BOLD_GLYPHS,
      sizeof(DIN2014_BOLD_GLYPHS) / sizeof(DIN2014_BOLD_GLYPHS[0]),
      DIN2014_BOLD_GLYPHS_HEIGHT,
      1,
      2,
  };

  switch (UI_FONT_MODE) {
    case UiFontMode::Din2014Regular:
      return &kRegularFont;
    case UiFontMode::Din2014Bold:
      return &kBoldFont;
    case UiFontMode::Standard:
    default:
      return nullptr;
  }
}

int16_t activeTextHeight() {
  const BitmapFontView* font = activeBitmapFont();
  if (font == nullptr) {
    return static_cast<int16_t>(5 * UI_FONT_SCALE);
  }

  const int16_t visible_rows =
      static_cast<int16_t>(font->height) - static_cast<int16_t>(font->top_trim) -
      static_cast<int16_t>(font->bottom_trim);
  return visible_rows > 0 ? static_cast<int16_t>(visible_rows * UI_FONT_SCALE) : 0;
}

int16_t titleTextY() {
  return (TOP_ROW_HEIGHT - activeTextHeight()) / 2;
}

int16_t statusRowFillHeight() {
  int16_t row_height = activeTextHeight();
  if (row_height < STATUS_ROW_HEIGHT) {
    row_height = STATUS_ROW_HEIGHT;
  }
  if (row_height > STATUS_SLOT_HEIGHT) {
    row_height = STATUS_SLOT_HEIGHT;
  }
  return row_height;
}

int16_t statusRowFillY(uint8_t row_index) {
  const int16_t slot_top = TOP_ROW_HEIGHT + static_cast<int16_t>(row_index * STATUS_SLOT_HEIGHT);
  return slot_top + (STATUS_SLOT_HEIGHT - statusRowFillHeight());
}

int16_t statusTextY(uint8_t row_index) {
  return statusRowFillY(row_index) + (statusRowFillHeight() - activeTextHeight()) / 2;
}

const BitmapGlyph* findBitmapGlyph(const BitmapFontView& font, char input) {
  const char ch = static_cast<char>(toupper(static_cast<unsigned char>(input)));
  for (size_t index = 0; index < font.count; ++index) {
    if (font.glyphs[index].ch == ch) {
      return &font.glyphs[index];
    }
  }

  for (size_t index = 0; index < font.count; ++index) {
    if (font.glyphs[index].ch == '?') {
      return &font.glyphs[index];
    }
  }
  return nullptr;
}

void drawBitmapGlyph(
    int16_t x,
    int16_t y,
    const BitmapGlyph& glyph,
    const BitmapFontView& font,
    uint8_t scale,
    uint16_t color) {
  const uint8_t last_row = static_cast<uint8_t>(font.height - font.bottom_trim);
  for (uint8_t row = font.top_trim; row < last_row; ++row) {
    const uint16_t row_bits = glyph.rows[row];
    int16_t run_start = -1;
    for (uint8_t col = 0; col < glyph.width; ++col) {
      const bool pixel_on = (row_bits & (1U << (glyph.width - 1 - col))) != 0;
      if (pixel_on && run_start < 0) {
        run_start = col;
      }

      const bool end_of_row = col + 1 == glyph.width;
      if (run_start >= 0 && (!pixel_on || end_of_row)) {
        const int16_t run_end = (pixel_on && end_of_row) ? col : static_cast<int16_t>(col - 1);
        display.fillRect(
            x + static_cast<int16_t>(run_start * scale),
            y + static_cast<int16_t>((row - font.top_trim) * scale),
            static_cast<int16_t>((run_end - run_start + 1) * scale),
            scale,
            color);
        run_start = -1;
      }
    }
  }
}

void drawBitmapText(
    int16_t x,
    int16_t y,
    const String& text,
    uint16_t color,
    const BitmapFontView& font,
    uint8_t scale) {
  int16_t cursor_x = x;
  for (size_t index = 0; index < static_cast<size_t>(text.length()); ++index) {
    const BitmapGlyph* glyph = findBitmapGlyph(font, text[index]);
    if (glyph == nullptr) {
      continue;
    }
    drawBitmapGlyph(cursor_x, y, *glyph, font, scale, color);
    cursor_x += static_cast<int16_t>((glyph->width + 1) * scale);
  }
}

void drawUiText(int16_t x, int16_t y, const String& text, uint16_t color) {
  const BitmapFontView* font = activeBitmapFont();
  if (font == nullptr) {
    display.drawText(x, y, text, color, UI_FONT_SCALE);
    return;
  }
  drawBitmapText(x, y, text, color, *font, UI_FONT_SCALE);
}

void logLine(const String& message) {
  Serial.print('[');
  Serial.print(millis());
  Serial.print(" ms] ");
  Serial.println(message);
}

bool hasPlaceholders() {
  return String(WIFI_SSID) == "SET_ME" || String(WIFI_PASSWORD) == "SET_ME";
}

void serviceButtons() {
  button_a.tick();
  button_b.tick();

  if (button_b.consumeLongPress()) {
    cycleTheme();
  } else if (button_b.consumeClick()) {
    setDisplayEnabled(!app.display_enabled);
  }

  if (button_a.consumeClick() && camera_job.kind != CameraJobKind::ToggleRecord && !app.button_busy) {
    record_toggle_requested = true;
  }
}

void responsiveDelay(uint32_t duration_ms) {
  const uint32_t deadline_ms = millis() + duration_ms;
  while (static_cast<int32_t>(deadline_ms - millis()) > 0) {
    serviceButtons();
    delay(1);
  }
}

void setRecordLed(bool enabled) {
  digitalWrite(REC_LED_PIN, enabled ? REC_LED_ON : REC_LED_OFF);
}

void updateReadyState(ReadyState state) {
  app.ready_state = state;
}

int findTypedObject(const String& body, const char* type_name) {
  const String marker = String("\"type\":\"") + type_name + "\"";
  return body.indexOf(marker);
}

String extractTypedString(const String& body, const char* type_name, const char* key_name) {
  const int type_index = findTypedObject(body, type_name);
  if (type_index < 0) {
    return "";
  }

  const String marker = String("\"") + key_name + "\":\"";
  const int value_index = body.indexOf(marker, type_index);
  if (value_index < 0) {
    return "";
  }

  const int string_start = value_index + marker.length();
  const int string_end = body.indexOf('"', string_start);
  if (string_end < 0) {
    return "";
  }

  return body.substring(string_start, string_end);
}

bool typedNamesContain(const String& body, const char* type_name, const char* needle) {
  const int type_index = findTypedObject(body, type_name);
  if (type_index < 0) {
    return false;
  }

  const int names_index = body.indexOf("\"names\":[", type_index);
  if (names_index < 0) {
    return false;
  }

  const int names_end = body.indexOf(']', names_index);
  if (names_end < 0) {
    return false;
  }

  const String marker = String("\"") + needle + "\"";
  const int match_index = body.indexOf(marker, names_index);
  return match_index >= 0 && match_index < names_end;
}

bool responseHasApiError(const String& body, String& error_message) {
  const int error_index = body.indexOf("\"error\":");
  if (error_index < 0) {
    return false;
  }

  const int value_index = body.indexOf(':', error_index);
  if (value_index < 0) {
    return false;
  }

  int scan_index = value_index + 1;
  while (scan_index < body.length() && body[scan_index] == ' ') {
    ++scan_index;
  }

  if (body.startsWith("null", scan_index)) {
    return false;
  }

  error_message = "camera error";
  return true;
}

const char* currentCameraApiVersion() {
  static const char* const kVersions[] = {"1.0", "1.1", "1.2", "1.3"};
  return kVersions[camera_http.version_index];
}

void clearCameraHttpResult() {
  camera_http.completed = false;
  camera_http.success = false;
  camera_http.raw_response = "";
  camera_http.response_body = "";
  camera_http.error = "";
}

void cancelCameraHttpRequest() {
  camera_http.client.stop();
  camera_http.phase = CameraHttpPhase::Idle;
  camera_http.method = nullptr;
  camera_http.params_json = "[]";
  camera_http.version_index = 0;
  camera_http.phase_started_ms = 0;
  camera_http.json_payload = "";
  camera_http.request_text = "";
  clearCameraHttpResult();
}

void prepareCameraHttpRequestText() {
  camera_http.json_payload = "";
  camera_http.json_payload.reserve(160);
  camera_http.json_payload += "{\"method\":\"";
  camera_http.json_payload += camera_http.method;
  camera_http.json_payload += "\",\"params\":";
  camera_http.json_payload += camera_http.params_json;
  camera_http.json_payload += ",\"id\":1,\"version\":\"";
  camera_http.json_payload += currentCameraApiVersion();
  camera_http.json_payload += "\"}";

  camera_http.request_text = "";
  camera_http.request_text.reserve(camera_http.json_payload.length() + 128);
  camera_http.request_text += "POST ";
  camera_http.request_text += CAMERA_PATH;
  camera_http.request_text += " HTTP/1.1\r\nHost: ";
  camera_http.request_text += CAMERA_HOST;
  camera_http.request_text += ':';
  camera_http.request_text += CAMERA_PORT;
  camera_http.request_text += "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ";
  camera_http.request_text += camera_http.json_payload.length();
  camera_http.request_text += "\r\n\r\n";
  camera_http.request_text += camera_http.json_payload;
}

void startCameraHttpRequest(const char* method, const char* params_json) {
  cancelCameraHttpRequest();
  camera_http.method = method;
  camera_http.params_json = params_json;
  prepareCameraHttpRequestText();
  camera_http.phase = CameraHttpPhase::ConnectAndSend;
  camera_http.phase_started_ms = millis();
}

void completeCameraHttpSuccess(const String& response_body) {
  camera_http.client.stop();
  camera_http.phase = CameraHttpPhase::Idle;
  camera_http.completed = true;
  camera_http.success = true;
  camera_http.response_body = response_body;
  camera_http.error = "";
}

void completeCameraHttpFailure(const String& error_message) {
  static const uint8_t kVersionCount = 4;
  camera_http.client.stop();
  if (camera_http.version_index + 1 < kVersionCount) {
    ++camera_http.version_index;
    clearCameraHttpResult();
    prepareCameraHttpRequestText();
    camera_http.phase = CameraHttpPhase::ConnectAndSend;
    camera_http.phase_started_ms = millis();
    return;
  }

  camera_http.phase = CameraHttpPhase::Idle;
  camera_http.completed = true;
  camera_http.success = false;
  camera_http.error = error_message;
}

bool cameraHttpBusy() {
  return camera_http.phase != CameraHttpPhase::Idle;
}

void tickCameraHttp() {
  switch (camera_http.phase) {
    case CameraHttpPhase::Idle:
      return;

    case CameraHttpPhase::ConnectAndSend: {
      camera_http.client.stop();
      camera_http.client.setTimeout(HTTP_CONNECT_STEP_TIMEOUT_MS);
      if (!camera_http.client.connect(CAMERA_HOST, CAMERA_PORT, HTTP_CONNECT_STEP_TIMEOUT_MS)) {
        completeCameraHttpFailure("camera connect failed");
        return;
      }

      const size_t written = camera_http.client.print(camera_http.request_text);
      if (written != camera_http.request_text.length()) {
        completeCameraHttpFailure("camera bad http");
        return;
      }

      camera_http.phase = CameraHttpPhase::WaitResponse;
      camera_http.phase_started_ms = millis();
      return;
    }

    case CameraHttpPhase::WaitResponse:
      if (camera_http.client.available() > 0) {
        camera_http.phase = CameraHttpPhase::ReadResponse;
        return;
      }
      if (!camera_http.client.connected()) {
        completeCameraHttpFailure("camera bad http");
        return;
      }
      if (millis() - camera_http.phase_started_ms > HTTP_TIMEOUT_MS) {
        completeCameraHttpFailure("camera timeout");
      }
      return;

    case CameraHttpPhase::ReadResponse: {
      size_t bytes_read = 0;
      while (camera_http.client.available() && bytes_read < HTTP_READ_CHUNK_BYTES) {
        camera_http.raw_response += static_cast<char>(camera_http.client.read());
        ++bytes_read;
      }

      if (!camera_http.client.connected() && !camera_http.client.available()) {
        const int body_index = camera_http.raw_response.indexOf("\r\n\r\n");
        if (body_index < 0) {
          completeCameraHttpFailure("camera bad http");
          return;
        }

        const String response_body = camera_http.raw_response.substring(body_index + 4);
        String error_message;
        if (responseHasApiError(response_body, error_message)) {
          completeCameraHttpFailure(error_message);
          return;
        }

        completeCameraHttpSuccess(response_body);
        return;
      }

      if (millis() - camera_http.phase_started_ms > HTTP_TIMEOUT_MS) {
        completeCameraHttpFailure("camera timeout");
      }
      return;
    }
  }
}

CameraState parseCameraState(const String& event_body) {
  CameraState state;
  state.camera_status = extractTypedString(event_body, "cameraStatus", "cameraStatus");
  state.has_start_movie_rec = typedNamesContain(event_body, "availableApiList", "startMovieRec");
  state.has_stop_movie_rec = typedNamesContain(event_body, "availableApiList", "stopMovieRec");
  state.has_start_rec_mode = typedNamesContain(event_body, "availableApiList", "startRecMode");
  state.reachable =
      !state.camera_status.isEmpty() || state.has_start_movie_rec || state.has_stop_movie_rec ||
      state.has_start_rec_mode || findTypedObject(event_body, "availableApiList") >= 0;
  state.ready = state.camera_status == "IDLE" || state.camera_status == "MovieRecording";
  return state;
}

bool cameraJobActive() {
  return camera_job.step != CameraJobStep::Idle;
}

void clearCameraJob() {
  camera_job.kind = CameraJobKind::Idle;
  camera_job.step = CameraJobStep::Idle;
  camera_job.wait_until_ms = 0;
  camera_job.command_method = nullptr;
  camera_job.attempted_start_rec_mode = false;
  camera_job.working_state = CameraState();
}

void cancelCameraWorkflow() {
  cancelCameraHttpRequest();
  if (camera_job.kind == CameraJobKind::ToggleRecord) {
    app.button_busy = false;
  }
  clearCameraJob();
}

void finishCameraJobSuccess(const CameraState& state) {
  const CameraJobKind finished_kind = camera_job.kind;
  const bool was_recording = app.camera.camera_status == "MovieRecording";
  const bool is_recording = state.camera_status == "MovieRecording";

  if (is_recording && (!was_recording || !app.recording_timer_active)) {
    app.recording_timer_active = true;
    app.recording_started_ms = millis();
  } else if (!is_recording) {
    app.recording_timer_active = false;
    app.recording_started_ms = 0;
  }

  app.camera = state;
  if (!app.last_error.isEmpty()) {
    logLine("camera recovered");
  }
  app.last_error = "";
  last_poll_ms = millis();

  if (finished_kind == CameraJobKind::ToggleRecord) {
    app.button_busy = false;
    logLine("record toggle ok -> " + app.camera.camera_status);
  }

  clearCameraJob();
}

void failCameraJob(const String& error_message) {
  const CameraJobKind failed_kind = camera_job.kind;
  cancelCameraHttpRequest();

  if (failed_kind == CameraJobKind::ToggleRecord) {
    app.button_busy = false;
    logLine("record toggle failed: " + error_message);
  } else if (failed_kind == CameraJobKind::Poll) {
    logLine(error_message);
  }

  app.last_error = error_message;
  last_poll_ms = millis();
  clearCameraJob();
}

void beginPollJob() {
  clearCameraJob();
  camera_job.kind = CameraJobKind::Poll;
  camera_job.step = CameraJobStep::PollRequestEvent;
  camera_job.working_state = app.camera;
}

void beginToggleJob() {
  clearCameraJob();
  camera_job.kind = CameraJobKind::ToggleRecord;
  camera_job.step = CameraJobStep::ToggleEvaluate;
  camera_job.working_state = app.camera;
  app.button_busy = true;
  logLine("button A clicked");
}

void tickCameraJob() {
  switch (camera_job.step) {
    case CameraJobStep::Idle:
      return;

    case CameraJobStep::PollRequestEvent:
      startCameraHttpRequest("getEvent", "[false]");
      camera_job.step = CameraJobStep::PollWaitEvent;
      return;

    case CameraJobStep::PollWaitEvent:
      if (!camera_http.completed) {
        return;
      }
      if (!camera_http.success) {
        const String error_message = camera_http.error;
        clearCameraHttpResult();
        failCameraJob(error_message);
        return;
      }
      camera_job.working_state = parseCameraState(camera_http.response_body);
      clearCameraHttpResult();
      if (camera_job.working_state.camera_status == "NotReady" && camera_job.working_state.has_start_rec_mode &&
          !camera_job.attempted_start_rec_mode &&
          millis() - last_start_rec_mode_ms >= START_REC_MODE_COOLDOWN_MS) {
        camera_job.attempted_start_rec_mode = true;
        last_start_rec_mode_ms = millis();
        logLine("camera reported NotReady, calling startRecMode");
        camera_job.step = CameraJobStep::PollRequestStartRecMode;
        return;
      }
      finishCameraJobSuccess(camera_job.working_state);
      return;

    case CameraJobStep::PollRequestStartRecMode:
      startCameraHttpRequest("startRecMode", "[]");
      camera_job.step = CameraJobStep::PollWaitStartRecMode;
      return;

    case CameraJobStep::PollWaitStartRecMode:
      if (!camera_http.completed) {
        return;
      }
      if (!camera_http.success) {
        const String error_message = camera_http.error;
        clearCameraHttpResult();
        failCameraJob(error_message);
        return;
      }
      clearCameraHttpResult();
      camera_job.wait_until_ms = millis() + 1000;
      camera_job.step = CameraJobStep::PollWaitAfterStartRecMode;
      return;

    case CameraJobStep::PollWaitAfterStartRecMode:
      if (static_cast<int32_t>(millis() - camera_job.wait_until_ms) < 0) {
        return;
      }
      camera_job.step = CameraJobStep::PollRequestEvent;
      return;

    case CameraJobStep::ToggleEvaluate:
      camera_job.working_state = app.camera;
      if (camera_job.working_state.camera_status == "MovieRecording") {
        camera_job.command_method = "stopMovieRec";
        camera_job.step = CameraJobStep::ToggleRequestCommand;
        return;
      }
      if (camera_job.working_state.camera_status == "IDLE") {
        camera_job.command_method = "startMovieRec";
        camera_job.step = CameraJobStep::ToggleRequestCommand;
        return;
      }
      if (camera_job.working_state.has_start_rec_mode) {
        camera_job.step = CameraJobStep::ToggleRequestStartRecMode;
        return;
      }
      failCameraJob("bad camera state");
      return;

    case CameraJobStep::ToggleRequestStartRecMode:
      startCameraHttpRequest("startRecMode", "[]");
      camera_job.step = CameraJobStep::ToggleWaitStartRecMode;
      return;

    case CameraJobStep::ToggleWaitStartRecMode:
      if (!camera_http.completed) {
        return;
      }
      if (!camera_http.success) {
        const String error_message = camera_http.error;
        clearCameraHttpResult();
        failCameraJob(error_message);
        return;
      }
      clearCameraHttpResult();
      camera_job.wait_until_ms = millis() + 1000;
      camera_job.step = CameraJobStep::ToggleWaitAfterStartRecMode;
      return;

    case CameraJobStep::ToggleWaitAfterStartRecMode:
      if (static_cast<int32_t>(millis() - camera_job.wait_until_ms) < 0) {
        return;
      }
      camera_job.step = CameraJobStep::ToggleRequestEventAfterStartRecMode;
      return;

    case CameraJobStep::ToggleRequestEventAfterStartRecMode:
      startCameraHttpRequest("getEvent", "[false]");
      camera_job.step = CameraJobStep::ToggleWaitEventAfterStartRecMode;
      return;

    case CameraJobStep::ToggleWaitEventAfterStartRecMode:
      if (!camera_http.completed) {
        return;
      }
      if (!camera_http.success) {
        const String error_message = camera_http.error;
        clearCameraHttpResult();
        failCameraJob(error_message);
        return;
      }
      camera_job.working_state = parseCameraState(camera_http.response_body);
      clearCameraHttpResult();
      if (camera_job.working_state.camera_status == "IDLE") {
        camera_job.command_method = "startMovieRec";
        camera_job.step = CameraJobStep::ToggleRequestCommand;
        return;
      }
      failCameraJob("bad camera state");
      return;

    case CameraJobStep::ToggleRequestCommand:
      if (camera_job.command_method == nullptr) {
        failCameraJob("bad camera state");
        return;
      }
      startCameraHttpRequest(camera_job.command_method, "[]");
      camera_job.step = CameraJobStep::ToggleWaitCommand;
      return;

    case CameraJobStep::ToggleWaitCommand:
      if (!camera_http.completed) {
        return;
      }
      if (!camera_http.success) {
        const String error_message = camera_http.error;
        clearCameraHttpResult();
        failCameraJob(error_message);
        return;
      }
      clearCameraHttpResult();
      camera_job.wait_until_ms = millis() + RECORD_SETTLE_MS;
      camera_job.step = CameraJobStep::ToggleWaitAfterCommand;
      return;

    case CameraJobStep::ToggleWaitAfterCommand:
      if (static_cast<int32_t>(millis() - camera_job.wait_until_ms) < 0) {
        return;
      }
      camera_job.step = CameraJobStep::ToggleRequestEventAfterCommand;
      return;

    case CameraJobStep::ToggleRequestEventAfterCommand:
      startCameraHttpRequest("getEvent", "[false]");
      camera_job.step = CameraJobStep::ToggleWaitEventAfterCommand;
      return;

    case CameraJobStep::ToggleWaitEventAfterCommand:
      if (!camera_http.completed) {
        return;
      }
      if (!camera_http.success) {
        const String error_message = camera_http.error;
        clearCameraHttpResult();
        failCameraJob(error_message);
        return;
      }
      camera_job.working_state = parseCameraState(camera_http.response_body);
      clearCameraHttpResult();
      finishCameraJobSuccess(camera_job.working_state);
      return;
  }
}

void refreshReadyState() {
  if (!app.have_wifi) {
    updateReadyState(app.camera_visible ? ReadyState::CameraVisible : ReadyState::NoCamera);
    return;
  }

  if (!app.camera.reachable) {
    updateReadyState(ReadyState::WifiConnected);
    return;
  }

  if (!app.camera.ready) {
    updateReadyState(ReadyState::CameraContact);
    return;
  }

  updateReadyState(ReadyState::Ready);
}

String wifiLabel() {
  if (app.have_wifi) {
    return "LINKED";
  }
  if (app.camera_visible) {
    return "CAMERA";
  }
  return "SEARCH";
}

String cameraLabel() {
  if (!app.camera_visible) {
    return "NO CAM";
  }
  if (!app.have_wifi) {
    return "SEEN";
  }
  if (!app.camera.reachable) {
    return "NO API";
  }
  if (app.camera.camera_status == "MovieRecording") {
    return "REC";
  }
  if (app.camera.camera_status == "IDLE") {
    return "IDLE";
  }
  if (app.camera.camera_status == "NotReady") {
    return "WAIT";
  }
  if (app.camera.camera_status.length() > 0) {
    return app.camera.camera_status;
  }
  return "CONTACT";
}

String recordingDurationLabel() {
  if (!SHOW_RECORDING_TIME || !app.recording_timer_active) {
    return "";
  }

  const uint32_t elapsed_seconds = (millis() - app.recording_started_ms) / 1000UL;
  const uint32_t hours = elapsed_seconds / 3600UL;
  const uint8_t minutes = static_cast<uint8_t>((elapsed_seconds / 60UL) % 60UL);
  const uint8_t seconds = static_cast<uint8_t>(elapsed_seconds % 60UL);

  char buffer[10];
  if (hours > 0) {
    snprintf(buffer, sizeof(buffer), "%lu:%02u:%02u", static_cast<unsigned long>(hours), minutes, seconds);
  } else {
    snprintf(buffer, sizeof(buffer), "%u:%02u", minutes, seconds);
  }
  return String(buffer);
}

String readyLabel() {
  switch (app.ready_state) {
    case ReadyState::Boot:
      return "BOOT";
    case ReadyState::NoCamera:
      return "NO CAM";
    case ReadyState::CameraVisible:
      return "CAM SEEN";
    case ReadyState::WifiConnected:
      return "WIFI OK";
    case ReadyState::CameraContact:
      return "API OK";
    case ReadyState::Ready:
      if (app.camera.camera_status == "MovieRecording") {
        const String duration = recordingDurationLabel();
        return duration.length() > 0 ? "RECORD " + duration : "RECORD";
      }
      return "READY";
    case ReadyState::Error:
      return "ERROR";
  }
  return "ERROR";
}

String errorLabel() {
  if (hasPlaceholders()) {
    return "EDIT CFG";
  }
  if (app.last_error.length() == 0) {
    return "DBG";
  }
  if (app.last_error.indexOf("timeout") >= 0) {
    return "TIMEOUT";
  }
  if (app.last_error.indexOf("connect") >= 0) {
    return "NO API";
  }
  if (app.last_error.indexOf("http") >= 0) {
    return "BAD HTTP";
  }
  if (app.last_error.indexOf("camera error") >= 0) {
    return "API ERR";
  }
  return "ERROR";
}

uint16_t currentBackgroundColor() {
  return THEME_OPTIONS[app.theme_index].background;
}

uint16_t contrastTextColor(uint16_t background) {
  const uint8_t red = static_cast<uint8_t>(((background >> 11) & 0x1F) * 255 / 31);
  const uint8_t green = static_cast<uint8_t>(((background >> 5) & 0x3F) * 255 / 63);
  const uint8_t blue = static_cast<uint8_t>((background & 0x1F) * 255 / 31);
  const uint32_t luminance = 299UL * red + 587UL * green + 114UL * blue;
  return luminance < 128000UL ? COLOR_WHITE : COLOR_BLACK;
}

uint16_t currentForegroundColor() {
  return contrastTextColor(currentBackgroundColor());
}

uint8_t batteryPercentFromMillivolts(uint16_t millivolts) {
  if (millivolts <= 3200) {
    return 0;
  }
  if (millivolts >= 4150) {
    return 100;
  }
  const uint32_t scaled = static_cast<uint32_t>(millivolts - 3200) * 100 + 475;
  return static_cast<uint8_t>(scaled / 950);
}

void refreshBatteryStatus(bool force = false) {
  const uint32_t now = millis();
  if (!force && now - last_battery_refresh_ms < BATTERY_REFRESH_MS) {
    return;
  }
  last_battery_refresh_ms = now;

  battery_status.charging = pmu.isCharging();
  if (!pmu.batteryConnected()) {
    battery_status.valid = false;
    battery_status.millivolts = 0;
    battery_status.percent = 0;
    battery_status.discharge_tenths_milliamps = 0;
    return;
  }

  const uint16_t millivolts = pmu.batteryMillivolts();
  if (millivolts == 0) {
    battery_status.valid = false;
    battery_status.millivolts = 0;
    battery_status.percent = 0;
    battery_status.discharge_tenths_milliamps = 0;
    return;
  }

  battery_status.valid = true;
  battery_status.millivolts = millivolts;
  battery_status.percent = batteryPercentFromMillivolts(millivolts);
  battery_status.discharge_tenths_milliamps =
      battery_status.charging ? 0 : pmu.batteryDischargeTenthsMilliamps();
}

uint16_t readyTextColor(const String& label) {
  if (label.startsWith("RECORD")) {
    return COLOR_RED;
  }
  if (label == "READY") {
    return COLOR_GREEN;
  }
  if (label == "ERROR") {
    return COLOR_ORANGE;
  }
  return currentForegroundColor();
}

uint16_t batteryFillColor() {
  if (!battery_status.valid) {
    return COLOR_GREEN;
  }
  if (battery_status.percent < 10) {
    return COLOR_RED;
  }
  if (battery_status.percent < 20) {
    return COLOR_YELLOW;
  }
  return COLOR_GREEN;
}

String batteryRuntimeLabel() {
  if (!battery_status.valid) {
    return "--M";
  }
  if (battery_status.charging) {
    return "CHG";
  }
  if (battery_status.discharge_tenths_milliamps == 0) {
    return "--M";
  }

  const uint32_t remaining_tenths_milliamps_hour =
      (static_cast<uint32_t>(BATTERY_CAPACITY_MAH) * battery_status.percent + 5) / 10;
  uint32_t estimated_minutes =
      (remaining_tenths_milliamps_hour * 60UL + battery_status.discharge_tenths_milliamps / 2) /
      battery_status.discharge_tenths_milliamps;
  if (estimated_minutes > BATTERY_RUNTIME_MAX_MINUTES) {
    estimated_minutes = BATTERY_RUNTIME_MAX_MINUTES;
  }
  return String(estimated_minutes) + "M";
}

String infoLineLabel() {
  if (!SHOW_INFO_LINE) {
    return "";
  }
  return errorLabel() + " " + batteryRuntimeLabel();
}

void drawChargeSocketGlyph(int16_t x, int16_t y, uint16_t color) {
  display.fillRect(x + 2, y + 0, 1, 2, color);
  display.fillRect(x + 5, y + 0, 1, 2, color);
  display.fillRect(x + 1, y + 2, 6, 4, color);
  display.fillRect(x + 3, y + 6, 2, 2, color);
  display.fillRect(x + 2, y + 8, 1, 1, color);
  display.fillRect(x + 5, y + 8, 1, 1, color);
}

void drawBatteryIndicator(const DisplaySnapshot& snapshot) {
  constexpr int16_t CHARGE_SOCKET_WIDTH = 8;
  constexpr int16_t CHARGE_SOCKET_HEIGHT = 9;
  constexpr int16_t CHARGE_SOCKET_GAP = 2;

  const int16_t total_width = BATTERY_BODY_WIDTH + BATTERY_CAP_WIDTH;
  const int16_t body_x = display.width() - 4 - total_width;
  const int16_t body_y = BATTERY_Y;
  const int16_t cap_x = body_x + BATTERY_BODY_WIDTH;
  const int16_t cap_y = body_y + (BATTERY_BODY_HEIGHT - BATTERY_CAP_HEIGHT) / 2;

  display.fillRect(body_x, body_y, BATTERY_BODY_WIDTH, 1, snapshot.battery_outline_color);
  display.fillRect(body_x, body_y + BATTERY_BODY_HEIGHT - 1, BATTERY_BODY_WIDTH, 1, snapshot.battery_outline_color);
  display.fillRect(body_x, body_y, 1, BATTERY_BODY_HEIGHT, snapshot.battery_outline_color);
  display.fillRect(body_x + BATTERY_BODY_WIDTH - 1, body_y, 1, BATTERY_BODY_HEIGHT, snapshot.battery_outline_color);
  display.fillRect(cap_x, cap_y, BATTERY_CAP_WIDTH, BATTERY_CAP_HEIGHT, snapshot.battery_outline_color);

  const int16_t inner_x = body_x + 1;
  const int16_t inner_y = body_y + 1;
  const int16_t inner_width = BATTERY_BODY_WIDTH - 2;
  const int16_t inner_height = BATTERY_BODY_HEIGHT - 2;
  display.fillRect(inner_x, inner_y, inner_width, inner_height, snapshot.background);

  if (snapshot.battery_valid) {
    const int16_t fill_width = (static_cast<int32_t>(inner_width) * snapshot.battery_percent + 99) / 100;
    if (fill_width > 0) {
      display.fillRect(inner_x, inner_y, fill_width, inner_height, snapshot.battery_fill_color);
    }
  }

  if (snapshot.battery_charging) {
    const int16_t socket_x = body_x - CHARGE_SOCKET_GAP - CHARGE_SOCKET_WIDTH;
    const int16_t socket_y = body_y + (BATTERY_BODY_HEIGHT - CHARGE_SOCKET_HEIGHT) / 2;
    display.fillRect(socket_x, socket_y, CHARGE_SOCKET_WIDTH, CHARGE_SOCKET_HEIGHT, snapshot.background);
    drawChargeSocketGlyph(socket_x, socket_y, snapshot.battery_charge_color);
  }
}

void drawTopRow(const DisplaySnapshot& snapshot) {
  display.fillRect(0, 0, display.width(), TOP_ROW_HEIGHT, snapshot.background);
  drawUiText(4, titleTextY(), snapshot.title, snapshot.title_color);
  drawBatteryIndicator(snapshot);
}

void drawStatusRow(uint8_t row_index, const String& text, uint16_t color, uint16_t background) {
  display.fillRect(0, statusRowFillY(row_index), display.width(), statusRowFillHeight(), background);
  drawUiText(4, statusTextY(row_index), text, color);
}

bool fieldChanged(
    const String& current_text,
    uint16_t current_color,
    const String& previous_text,
    uint16_t previous_color) {
  return current_color != previous_color || current_text != previous_text;
}

bool batteryIndicatorChanged(const DisplaySnapshot& current, const DisplaySnapshot& previous) {
  return current.battery_valid != previous.battery_valid ||
         current.battery_percent != previous.battery_percent ||
         current.battery_charging != previous.battery_charging ||
         current.battery_outline_color != previous.battery_outline_color ||
         current.battery_fill_color != previous.battery_fill_color ||
         current.battery_charge_color != previous.battery_charge_color;
}

DisplaySnapshot captureDisplaySnapshot() {
  DisplaySnapshot snapshot;
  snapshot.background = currentBackgroundColor();
  snapshot.title = "SONY REMOTE";
  snapshot.title_color = currentForegroundColor();
  snapshot.battery_valid = battery_status.valid;
  snapshot.battery_percent = battery_status.valid ? battery_status.percent : 0;
  snapshot.battery_charging = battery_status.charging;
  snapshot.battery_outline_color = currentForegroundColor();
  snapshot.battery_fill_color = batteryFillColor();
  snapshot.battery_charge_color = COLOR_YELLOW;
  snapshot.ready = readyLabel();
  snapshot.ready_color = readyTextColor(snapshot.ready);
  snapshot.net = "NET " + wifiLabel();
  snapshot.net_color = app.have_wifi ? COLOR_MAGENTA : COLOR_CYAN;
  snapshot.camera = "CAM " + cameraLabel();
  snapshot.camera_color = currentForegroundColor();
  snapshot.footer = infoLineLabel();
  snapshot.footer_color = app.last_error.length() == 0 ? currentForegroundColor() : COLOR_ORANGE;
  return snapshot;
}

void renderDisplay(bool force = false) {
  if (!app.display_enabled) {
    return;
  }

  const DisplaySnapshot snapshot = captureDisplaySnapshot();
  const bool background_changed =
      force || !have_last_display || snapshot.background != last_display.background;
  if (background_changed) {
    display.fillScreen(snapshot.background);
  }

  if (background_changed || !have_last_display ||
      fieldChanged(snapshot.title, snapshot.title_color, last_display.title, last_display.title_color) ||
      batteryIndicatorChanged(snapshot, last_display)) {
    drawTopRow(snapshot);
  }
  if (background_changed || !have_last_display ||
      fieldChanged(snapshot.ready, snapshot.ready_color, last_display.ready, last_display.ready_color)) {
    drawStatusRow(0, snapshot.ready, snapshot.ready_color, snapshot.background);
  }
  if (background_changed || !have_last_display ||
      fieldChanged(snapshot.net, snapshot.net_color, last_display.net, last_display.net_color)) {
    drawStatusRow(1, snapshot.net, snapshot.net_color, snapshot.background);
  }
  if (background_changed || !have_last_display ||
      fieldChanged(snapshot.camera, snapshot.camera_color, last_display.camera, last_display.camera_color)) {
    drawStatusRow(2, snapshot.camera, snapshot.camera_color, snapshot.background);
  }
  if (background_changed || !have_last_display ||
      fieldChanged(snapshot.footer, snapshot.footer_color, last_display.footer, last_display.footer_color)) {
    drawStatusRow(3, snapshot.footer, snapshot.footer_color, snapshot.background);
  }

  last_display = snapshot;
  have_last_display = true;
}

void cycleTheme() {
  app.theme_index = static_cast<uint8_t>((app.theme_index + 1) % THEME_OPTION_COUNT);
  preferences.putUChar(THEME_PREF_KEY, app.theme_index);
  logLine("theme -> " + String(THEME_OPTIONS[app.theme_index].name));
  renderDisplay(true);
}

void setDisplayEnabled(bool enabled) {
  app.display_enabled = enabled;
  pmu.screenSwitch(enabled);
  if (enabled) {
    renderDisplay(true);
  }
}

void ensureWifi() {
  if (hasPlaceholders()) {
    app.have_wifi = false;
    app.camera_visible = false;
    wifi_connect_started_ms = 0;
    wifi_scan_in_progress = false;
    WiFi.scanDelete();
    return;
  }

  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == WIFI_SSID) {
    app.have_wifi = true;
    app.camera_visible = true;
    wifi_connect_started_ms = 0;
    wifi_scan_in_progress = false;
    return;
  }

  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() != WIFI_SSID) {
    WiFi.disconnect();
    wifi_scan_in_progress = false;
  }

  app.have_wifi = false;

  if (wifi_connect_started_ms != 0 && millis() - wifi_connect_started_ms > WIFI_CONNECT_TIMEOUT_MS) {
    logLine("wifi connect timeout, retrying");
    WiFi.disconnect();
    wifi_connect_started_ms = 0;
    wifi_scan_in_progress = false;
  }

  if (wifi_connect_started_ms != 0) {
    return;
  }

  if (wifi_scan_in_progress) {
    const int network_count = WiFi.scanComplete();
    if (network_count == -1) {
      return;
    }

    wifi_scan_in_progress = false;
    bool found_target = false;
    if (network_count >= 0) {
      for (int index = 0; index < network_count; ++index) {
        if (WiFi.SSID(index) == WIFI_SSID) {
          found_target = true;
          break;
        }
      }
    }
    WiFi.scanDelete();

    app.camera_visible = found_target;
    if (!found_target) {
      return;
    }

    logLine("camera wifi visible, connecting");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    wifi_connect_started_ms = millis();
    return;
  }

  if (millis() - last_wifi_scan_ms < WIFI_SCAN_INTERVAL_MS) {
    return;
  }

  last_wifi_scan_ms = millis();
  WiFi.scanDelete();
  app.camera_visible = false;
  wifi_scan_in_progress = true;
  WiFi.scanNetworks(true, false);
}

void appSetup() {
  Serial.begin(115200);
  delay(200);

  pinMode(REC_LED_PIN, OUTPUT);
  setRecordLed(false);

  button_a.begin();
  button_b.begin();
  pmu.begin();
  preferences.begin(SETTINGS_NAMESPACE, false);
  app.theme_index = preferences.getUChar(THEME_PREF_KEY, 0);
  if (app.theme_index >= THEME_OPTION_COUNT) {
    app.theme_index = 0;
  }
  display.begin();
  refreshBatteryStatus(true);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect();
  WiFi.setSleep(false);

  logLine("sony remote tally boot on M5StickC");
  logLine(String("config source: ") + CONFIG_SOURCE);
  if (hasPlaceholders()) {
    app.last_error = "edit config.h";
  }
  refreshReadyState();
  renderDisplay(true);
}

void appLoop() {
  serviceButtons();
  if (hasPlaceholders()) {
    refreshReadyState();
    setRecordLed(false);
    renderDisplay();
    responsiveDelay(MAIN_LOOP_DELAY_MS);
    return;
  }

  ensureWifi();

  if (!app.have_wifi) {
    record_toggle_requested = false;
    if (cameraJobActive() || cameraHttpBusy()) {
      failCameraJob("camera connect failed");
    }
  }

  if (record_toggle_requested && camera_job.kind == CameraJobKind::Poll) {
    cancelCameraWorkflow();
  }

  if (record_toggle_requested && app.have_wifi && !cameraJobActive() && !cameraHttpBusy() && !app.button_busy) {
    record_toggle_requested = false;
    beginToggleJob();
  }

  if (!record_toggle_requested && app.have_wifi && !cameraJobActive() && !cameraHttpBusy() &&
      millis() - last_poll_ms >= POLL_INTERVAL_MS) {
    beginPollJob();
  }

  tickCameraJob();
  tickCameraHttp();
  tickCameraJob();

  refreshBatteryStatus();
  refreshReadyState();
  setRecordLed(app.have_wifi && app.camera.camera_status == "MovieRecording");
  renderDisplay();
  responsiveDelay(MAIN_LOOP_DELAY_MS);
}

}  // namespace

void setup() {
  appSetup();
}

void loop() {
  appLoop();
}
