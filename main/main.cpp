/*
 * main.cpp - M5Stack Core2 OPC UA server
 *
 * Two tasks:
 *   "hw"    (core 1) reads every sensor, handles touch, drives the outputs
 *                    (vibration, backlight, side LEDs, microphone) and draws
 *                    the screen. It is the only code that talks to the
 *                    Core2 hardware.
 *   "opcua" (core 0) waits for Wi-Fi, syncs the clock and runs the OPC UA
 *                    server (see opcua_server.c).
 * They share data through g_state (see device_state.h).
 *
 * Screen: battery in the top bar on every page. Touch buttons below the
 * screen pick the page:  A = Main   B = Motion   C = System
 * The Main page also has on-screen buttons: Light on/off, Color, Buzz.
 *
 * Battery saving: after idle_timeout_s without a touch or a client change,
 * the screen and side LEDs turn off ("asleep") while the server keeps
 * running. The CPU also scales down to 80 MHz when idle (app_main), and
 * Wi-Fi uses modem sleep (wifi_net.c).
 */
#include <M5Unified.h>
#include <utility/led/LED_Strip_Class.hpp> /* for the M5GO Bottom2 side LEDs */

#include <math.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "device_state.h"
#include "opcua_server.h"
#include "wifi_config.h"
#include "wifi_net.h"

static const char *TAG = "main";

/* ------------------------------------------------------------------ */
/* Shared state                                                        */
/* ------------------------------------------------------------------ */
device_state_t g_state;                 /* see device_state.h */
static SemaphoreHandle_t s_state_mutex; /* guards g_state, created in app_main */

/* Take / release the g_state mutex. Used by both tasks (and the C code in
 * opcua_server.c, hence extern "C"). Hold it only long enough to copy fields
 * in or out: never call hardware or OPC UA functions while holding it. */
extern "C" void state_lock(void)   { xSemaphoreTake(s_state_mutex, portMAX_DELAY); }
extern "C" void state_unlock(void) { xSemaphoreGive(s_state_mutex); }

/* ------------------------------------------------------------------ */
/* Clock helpers - the RTC chip is kept in UTC                         */
/* ------------------------------------------------------------------ */

/* Days between 1970-01-01 and the given calendar date (y, m = 1..12, d = 1..31).
 * Howard Hinnant's "days_from_civil" algorithm: exact for any Gregorian date
 * and needs no time zone, unlike mktime(), which applies the local TZ. */
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/* Read the BM8563 RTC chip and return the time as Unix seconds (UTC).
 * Returns 0 when there is no RTC or it has never been set (year < 2024). */
static int64_t rtc_read_unix(void) {
    if(!M5.Rtc.isEnabled()) return 0;
    auto dt = M5.Rtc.getDateTime();
    if(dt.date.year < 2024 || dt.date.month < 1 || dt.date.date < 1) return 0; /* not set */
    return days_from_civil(dt.date.year, dt.date.month, dt.date.date) * 86400 +
           dt.time.hours * 3600 + dt.time.minutes * 60 + dt.time.seconds;
}

/* Copy the system clock (just set from NTP) into the RTC chip as UTC, so the
 * correct time survives a reboot without internet. Uses I2C, so only the hw
 * task calls it (see s_rtc_needs_update). */
static void rtc_write_from_system_time(void) {
    time_t now = time(nullptr);
    struct tm utc;
    gmtime_r(&now, &utc);
    M5.Rtc.setDateTime(&utc);
}

/* ------------------------------------------------------------------ */
/* OPC UA task                                                         */
/* ------------------------------------------------------------------ */
/* Set by the OPC UA task after a successful NTP sync; the hw task then writes
 * the RTC (it owns the I2C bus) and clears the flag. */
static volatile bool s_rtc_needs_update = false;

/* FreeRTOS task (core 0): wait for Wi-Fi, set the clock from NTP, then run
 * the OPC UA server. opcua_server_run() only returns if start-up fails. */
static void opcua_task(void *) {
    wifi_wait_connected();
    if(time_sync())
        s_rtc_needs_update = true; /* hw task writes the RTC (it owns I2C) */
    opcua_server_run();            /* runs forever */
    ESP_LOGE(TAG, "OPC UA server stopped");
    vTaskDelete(nullptr);
}

/* ------------------------------------------------------------------ */
/* Sensors                                                             */
/* ------------------------------------------------------------------ */

/* Record 512 microphone samples at 16 kHz (32 ms) and return their loudness in
 * dBFS: 0 = full scale, about -90 = silence. The DC offset (mean) is removed
 * before taking the RMS. Returns -96 if the microphone is off or fails.
 * Blocks the calling task for ~32 ms while recording. */
static float read_sound_level_dbfs(void) {
    static int16_t buf[512];
    if(!M5.Mic.isEnabled()) return -96.0f;
    if(!M5.Mic.record(buf, 512, 16000)) return -96.0f;
    int guard = 100;
    while(M5.Mic.isRecording() && guard-- > 0) vTaskDelay(1);

    double mean = 0;
    for(int i = 0; i < 512; i++) mean += buf[i];
    mean /= 512;
    double sum = 0;
    for(int i = 0; i < 512; i++) { double s = buf[i] - mean; sum += s * s; }
    double rms = sqrt(sum / 512);
    if(rms < 1) return -96.0f;
    return (float)(20.0 * log10(rms / 32768.0));
}

/* Read every sensor into the sensor fields of s: power chip, IMU (plus tilt),
 * microphone (only if s.mic_enabled), RTC and system stats. s is a private
 * copy of g_state: the caller copies the results back under the lock, so the
 * slow I2C/I2S reads happen without holding the mutex. */
static void read_sensors(device_state_t &s) {
    /* Power */
    s.battery_level    = M5.Power.getBatteryLevel();
    s.battery_voltage  = M5.Power.getBatteryVoltage() / 1000.0f;
    s.battery_current  = (float)M5.Power.getBatteryCurrent();
    s.battery_charging = M5.Power.isCharging() == m5::Power_Class::is_charging;
    s.usb_voltage      = M5.Power.getVBUSVoltage() / 1000.0f;
    if(s.usb_voltage < 0) s.usb_voltage = 0;

    /* IMU */
    s.imu_ok = M5.Imu.isEnabled();
    if(s.imu_ok) {
        M5.Imu.update();
        auto d = M5.Imu.getImuData();
        s.accel[0] = d.accel.x; s.accel[1] = d.accel.y; s.accel[2] = d.accel.z;
        s.gyro[0]  = d.gyro.x;  s.gyro[1]  = d.gyro.y;  s.gyro[2]  = d.gyro.z;
        float t;
        if(M5.Imu.getTemp(&t)) s.imu_temperature = t;
        /* The accelerometer always feels gravity, so tilting the device
         * shows up as a steady reading. Turn that into tilt angles. */
        const float rad2deg = 57.29578f;
        s.pitch = atan2f(-d.accel.x, sqrtf(d.accel.y * d.accel.y + d.accel.z * d.accel.z)) * rad2deg;
        s.roll  = atan2f(d.accel.y, d.accel.z) * rad2deg;
    }

    /* Microphone */
    s.sound_level = s.mic_enabled ? read_sound_level_dbfs() : -96.0f;

    /* RTC */
    s.rtc_unix = rtc_read_unix();

    /* System */
    s.uptime_s  = (uint32_t)(esp_timer_get_time() / 1000000);
    s.free_heap = (uint32_t)esp_get_free_heap_size();
    s.wifi_rssi = s.ip_address[0] ? wifi_rssi() : 0;
}

/* ------------------------------------------------------------------ */
/* Screen                                                              */
/*                                                                    */
/* The 320x240 screen is drawn into an off-screen canvas (in PSRAM)   */
/* and pushed in one go, so it never flickers. Layout, top to bottom: */
/*   y   0-26   top bar (IP, client count, battery)                   */
/*   y  26-210  page content (Main / Motion / System)                 */
/*   y 216-238  page tabs, drawn above the touch buttons A/B/C        */
/* Colours are RGB565.                                                */
/* ------------------------------------------------------------------ */
static M5Canvas canvas(&M5.Display);

static const uint16_t BG      = 0x0000;                  /* black */
static const uint16_t FG      = TFT_WHITE;
static const uint16_t DIM     = 0x8410;                  /* grey */
static const uint16_t ACCENT  = 0x05FF;                  /* cyan-ish */
static const uint16_t BAR_BG  = 0x2104;                  /* dark grey */

/* Battery icon in the top-right corner: fill colour green > 50 %, yellow
 * > 20 %, red below, with the percentage to its left ("+" = charging). */
static void draw_battery(const device_state_t &s) {
    const int x = 262, y = 5, w = 36, h = 16;
    int level = s.battery_level < 0 ? 0 : (s.battery_level > 100 ? 100 : s.battery_level);
    uint16_t col = level > 50 ? TFT_GREEN : (level > 20 ? TFT_YELLOW : TFT_RED);
    canvas.drawRect(x, y, w, h, FG);
    canvas.fillRect(x + w, y + 4, 3, h - 8, FG);
    canvas.fillRect(x + 2, y + 2, (w - 4) * level / 100, h - 4, col);
    canvas.setFont(&fonts::DejaVu12);
    canvas.setTextColor(FG);
    canvas.setTextDatum(textdatum_t::middle_right);
    char txt[16];
    if(s.battery_level < 0) snprintf(txt, sizeof(txt), "--%%");
    else snprintf(txt, sizeof(txt), "%s%d%%", s.battery_charging ? "+" : "", (int)s.battery_level);
    canvas.drawString(txt, x - 4, y + h / 2);
}

/* Top bar on every page: IP address and OPC UA client count (or "Wi-Fi
 * connecting..." while there is no IP), plus the battery icon. */
static void draw_top_bar(const device_state_t &s) {
    canvas.fillRect(0, 0, 320, 26, BAR_BG);
    canvas.setFont(&fonts::DejaVu12);
    canvas.setTextDatum(textdatum_t::middle_left);
    if(s.ip_address[0]) {
        canvas.setTextColor(FG);
        canvas.drawString(s.ip_address, 6, 13);
        canvas.setTextColor(s.client_count ? TFT_GREEN : DIM);
        char c[20];
        snprintf(c, sizeof(c), "%u client%s", (unsigned)s.client_count, s.client_count == 1 ? "" : "s");
        canvas.drawString(c, 118, 13);
    } else {
        canvas.setTextColor(TFT_ORANGE);
        canvas.drawString("Wi-Fi connecting...", 6, 13);
    }
    draw_battery(s);
}

/* Page tabs at the bottom, one above each touch button (A/B/C). The current
 * page is highlighted. */
static void draw_bottom_bar(uint8_t page) {
    static const char *names[3] = {"Main", "Motion", "System"};
    canvas.setFont(&fonts::DejaVu12);
    canvas.setTextDatum(textdatum_t::middle_center);
    for(int i = 0; i < 3; i++) {
        int cx = 55 + i * 105;  /* centred over the three touch buttons */
        bool on = i == page;
        canvas.fillRoundRect(cx - 45, 216, 90, 22, 6, on ? ACCENT : BAR_BG);
        canvas.setTextColor(on ? BG : FG);
        canvas.drawString(names[i], cx, 227);
    }
}

/* On-screen buttons on the Main page (above the touch buttons A/B/C).
 * CTRL_Y/H/W give the row's top edge, height and each button's width. */
enum { CTRL_LIGHT, CTRL_COLOR, CTRL_BUZZ, CTRL_COUNT };
static const int CTRL_Y = 172, CTRL_H = 36, CTRL_W = 96;
/* Horizontal centre of on-screen button i (lined up with the A/B/C tabs) */
static int ctrl_cx(int i) { return 55 + i * 105; }

/* Colours the Color button steps through */
static const uint8_t LIGHT_COLORS[][3] = {
    {255, 255, 255}, {255, 0, 0}, {255, 96, 0}, {255, 200, 0},
    {0, 255, 0}, {0, 255, 255}, {0, 0, 255}, {255, 0, 255},
};
static const int LIGHT_COLOR_COUNT = sizeof(LIGHT_COLORS) / sizeof(LIGHT_COLORS[0]);

/* Draw the Light / Color / Buzz buttons. Light is highlighted while the
 * lights are on; Color shows a swatch of the current colour. */
static void draw_controls(const device_state_t &s) {
    canvas.setFont(&fonts::DejaVu12);
    canvas.setTextDatum(textdatum_t::middle_center);
    for(int i = 0; i < CTRL_COUNT; i++) {
        int x = ctrl_cx(i) - CTRL_W / 2, cy = CTRL_Y + CTRL_H / 2;
        bool lit = i == CTRL_LIGHT && s.lights_on;
        canvas.fillRoundRect(x, CTRL_Y, CTRL_W, CTRL_H, 8, lit ? ACCENT : BAR_BG);
        canvas.drawRoundRect(x, CTRL_Y, CTRL_W, CTRL_H, 8, DIM);
        canvas.setTextColor(lit ? BG : FG);
        if(i == CTRL_LIGHT) {
            canvas.drawString(s.lights_on ? "Light ON" : "Light OFF", ctrl_cx(i), cy);
        } else if(i == CTRL_COLOR) {
            canvas.fillCircle(x + 16, cy, 8, canvas.color565(s.lights_rgb[0], s.lights_rgb[1], s.lights_rgb[2]));
            canvas.drawCircle(x + 16, cy, 8, FG);
            canvas.drawString("Color", ctrl_cx(i) + 10, cy);
        } else {
            canvas.drawString("Buzz", ctrl_cx(i), cy);
        }
    }
}

/* Which on-screen button is at screen position (x, y): CTRL_LIGHT,
 * CTRL_COLOR, CTRL_BUZZ, or -1 for none. The caller only acts on it while
 * the Main page is shown. */
static int ctrl_hit(int x, int y) {
    if(y < CTRL_Y || y >= CTRL_Y + CTRL_H) return -1;
    for(int i = 0; i < CTRL_COUNT; i++)
        if(abs(x - ctrl_cx(i)) <= CTRL_W / 2) return i;
    return -1;
}

/* Page A (Main): the label, value and unit that OPC UA clients write to the
 * Display.* nodes, a status line, and the on-screen buttons. Very large or
 * very small values switch to %g format, and long values to a smaller font,
 * so they always fit. */
static void draw_page_main(const device_state_t &s) {
    canvas.setTextColor(DIM);
    canvas.setFont(&fonts::DejaVu18);
    canvas.setTextDatum(textdatum_t::top_center);
    canvas.drawString(s.display_label[0] ? s.display_label : "Display Value", 160, 32);

    char val[32];
    double v = s.display_value;
    if(fabs(v) >= 100000 || (v != 0 && fabs(v) < 0.01)) snprintf(val, sizeof(val), "%.3g", v);
    else snprintf(val, sizeof(val), "%.2f", v);
    canvas.setTextColor(FG);
    canvas.setFont(&fonts::DejaVu56);
    canvas.setTextDatum(textdatum_t::middle_center);
    if(canvas.textWidth(val) > 300) canvas.setFont(&fonts::DejaVu40);
    canvas.drawString(val, 160, 90);

    if(s.display_unit[0]) {
        canvas.setTextColor(ACCENT);
        canvas.setFont(&fonts::DejaVu24);
        canvas.drawString(s.display_unit, 160, 132);
    }

    canvas.setFont(&fonts::DejaVu12);
    canvas.setTextColor(DIM);
    char line[64];
    char sound[16];
    if(s.mic_enabled) snprintf(sound, sizeof(sound), "%.0f dB", s.sound_level);
    else              snprintf(sound, sizeof(sound), "mic off");
    snprintf(line, sizeof(line), "Sound %s   IMU %.1f C   Batt %.2f V",
             sound, s.imu_temperature, s.battery_voltage);
    canvas.drawString(line, 160, 157);

    draw_controls(s);
}

/* One row on the Motion page centred at height y: axis name, the value
 * printed with fmt, and a bar growing from the centre (right/cyan when
 * positive, left/orange when negative). The bar reaches the end at
 * +/- full_scale and is clamped beyond that. */
static void draw_axis_row(int y, const char *name, float v, float full_scale, const char *fmt) {
    canvas.setFont(&fonts::DejaVu18);
    canvas.setTextDatum(textdatum_t::middle_left);
    canvas.setTextColor(FG);
    canvas.drawString(name, 10, y);
    char txt[24];
    snprintf(txt, sizeof(txt), fmt, v);
    canvas.setTextDatum(textdatum_t::middle_right);
    canvas.drawString(txt, 150, y);
    /* centre-zero bar */
    const int bx = 162, bw = 148, bh = 12, mid = bx + bw / 2;
    canvas.fillRect(bx, y - bh / 2, bw, bh, BAR_BG);
    float f = v / full_scale; if(f > 1) f = 1; if(f < -1) f = -1;
    int len = (int)(f * (bw / 2));
    if(len >= 0) canvas.fillRect(mid, y - bh / 2, len, bh, ACCENT);
    else         canvas.fillRect(mid + len, y - bh / 2, -len, bh, TFT_ORANGE);
    canvas.drawFastVLine(mid, y - bh / 2 - 2, bh + 4, FG);
}

/* Page B (Motion): acceleration (includes gravity, bars span +/-2 g), tilt
 * angles, and rotation rate (bars span +/-250 deg/s). */
static void draw_page_motion(const device_state_t &s) {
    if(!s.imu_ok) {
        canvas.setTextDatum(textdatum_t::middle_center);
        canvas.setFont(&fonts::DejaVu18);
        canvas.setTextColor(TFT_RED);
        canvas.drawString("IMU not found", 160, 110);
        return;
    }
    canvas.setFont(&fonts::DejaVu12);
    canvas.setTextColor(DIM);
    canvas.setTextDatum(textdatum_t::top_left);
    canvas.drawString("Accel + gravity (g)", 10, 32);
    char tilt[48];
    snprintf(tilt, sizeof(tilt), "Tilt %+.0f / %+.0f deg", s.pitch, s.roll);
    canvas.setTextColor(ACCENT);
    canvas.setTextDatum(textdatum_t::top_right);
    canvas.drawString(tilt, 310, 32);
    draw_axis_row(58,  "X", s.accel[0], 2.0f, "%+.2f");
    draw_axis_row(80,  "Y", s.accel[1], 2.0f, "%+.2f");
    draw_axis_row(102, "Z", s.accel[2], 2.0f, "%+.2f");
    canvas.setFont(&fonts::DejaVu12);
    canvas.setTextColor(DIM);
    canvas.setTextDatum(textdatum_t::top_left);
    canvas.drawString("Rotation (deg/s)", 10, 120);
    draw_axis_row(146, "X", s.gyro[0], 250.0f, "%+.0f");
    draw_axis_row(168, "Y", s.gyro[1], 250.0f, "%+.0f");
    draw_axis_row(190, "Z", s.gyro[2], 250.0f, "%+.0f");
}

/* One small-caption / large-value pair on the System page, placed in a
 * 2-column grid (col 0..1, row 0..3). */
static void draw_kv(int col, int row, const char *k, const char *v) {
    int x = col ? 165 : 10, y = 38 + row * 34;
    canvas.setTextDatum(textdatum_t::top_left);
    canvas.setFont(&fonts::DejaVu9);
    canvas.setTextColor(DIM);
    canvas.drawString(k, x, y);
    canvas.setFont(&fonts::DejaVu18);
    canvas.setTextColor(FG);
    canvas.drawString(v, x, y + 11);
}

/* Page C (System): power, local time, uptime, Wi-Fi signal, free memory and
 * the endpoint URL to type into an OPC UA client. Local time uses the TZ set
 * from LOCAL_TIMEZONE; it shows "not set" until the clock is past Nov 2023. */
static void draw_page_system(const device_state_t &s) {
    char b[48];
    snprintf(b, sizeof(b), "%.2f V", s.battery_voltage);           draw_kv(0, 0, "BATTERY", b);
    snprintf(b, sizeof(b), "%+.0f mA", s.battery_current);         draw_kv(1, 0, "CURRENT", b);
    snprintf(b, sizeof(b), "%.2f V", s.usb_voltage);               draw_kv(0, 1, "USB", b);
    draw_kv(1, 1, "CHARGING", s.battery_charging ? "Yes" : "No");

    time_t now = time(nullptr);
    if(now > 1700000000) {
        struct tm lt; localtime_r(&now, &lt);
        strftime(b, sizeof(b), "%H:%M:%S", &lt);
    } else snprintf(b, sizeof(b), "not set");
    draw_kv(0, 2, "LOCAL TIME", b);
    uint32_t up = s.uptime_s;
    snprintf(b, sizeof(b), "%uh %02um", (unsigned)(up / 3600), (unsigned)(up / 60 % 60));
    draw_kv(1, 2, "UPTIME", b);
    snprintf(b, sizeof(b), "%d dBm", (int)s.wifi_rssi);             draw_kv(0, 3, "WI-FI", b);
    snprintf(b, sizeof(b), "%u kB", (unsigned)(s.free_heap / 1024)); draw_kv(1, 3, "FREE MEMORY", b);

    canvas.setTextDatum(textdatum_t::top_left);
    canvas.setFont(&fonts::DejaVu12);
    canvas.setTextColor(ACCENT);
    if(s.ip_address[0]) snprintf(b, sizeof(b), "opc.tcp://%s:4840", s.ip_address);
    else snprintf(b, sizeof(b), "waiting for Wi-Fi (%s)", WIFI_SSID);
    canvas.drawString(b, 10, 176);
    canvas.setTextColor(DIM);
    canvas.drawString("Security: None (test mode)", 10, 194);
}

/* Redraw the whole screen from a snapshot of the state, then push it to the
 * display in one transfer. */
static void draw_screen(const device_state_t &s) {
    canvas.fillScreen(BG);
    draw_top_bar(s);
    switch(s.page) {
        case 1:  draw_page_motion(s); break;
        case 2:  draw_page_system(s); break;
        default: draw_page_main(s);   break;
    }
    draw_bottom_bar(s.page);
    canvas.pushSprite(0, 0);
}

/* ------------------------------------------------------------------ */
/* Hardware task: sensors, buttons, outputs, screen                    */
/* ------------------------------------------------------------------ */

/* FreeRTOS task (core 1), the only code that touches the Core2 hardware.
 * Each pass of the loop (every 20 ms, 50 ms while asleep):
 *   1. reads touch and the A/B/C buttons, and handles on-screen buttons
 *   2. works out idle sleep (screen + lights off after idle_timeout_s)
 *   3. applies outputs that changed in g_state (from the screen or from
 *      OPC UA): microphone on/off, vibration, backlight, side lights
 *   4. reads the sensors every 200 ms (1 s while asleep)
 *   5. redraws the screen at ~5 fps (not while asleep)
 * The applied_* variables remember what was last sent to the hardware, so
 * each output is only touched when its wanted value actually changes. */
static void hw_task(void *) {
    uint8_t  applied_brightness = 0;
    bool     applied_lights_on = true;   /* differs from the start state, forces a first update */
    uint8_t  applied_rgb[3] = {0, 0, 0};
    uint8_t  applied_lights_bright = 0;
    bool     applied_mic = false;        /* the microphone starts off */
    int64_t  vibrate_until_us = 0;       /* when to stop the motor (0 = not running) */
    uint32_t last_sensor_ms = 0, last_draw_ms = 0;
    uint32_t last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000); /* last touch or client change */
    bool     asleep = false;             /* screen + lights off because of idle timeout */

    for(;;) {
        M5.update(); /* touch + buttons */
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        /* --- buttons choose the page --- */
        int pressed = -1;
        if(M5.BtnA.wasPressed()) pressed = 0;
        if(M5.BtnB.wasPressed()) pressed = 1;
        if(M5.BtnC.wasPressed()) pressed = 2;

        auto t = M5.Touch.getDetail();
        bool touched = M5.Touch.getCount() > 0;
        int ctrl = (touched && t.wasPressed()) ? ctrl_hit(t.x, t.y) : -1;

        /* --- idle sleep: any touch is activity. The touch that wakes the
         *     screen only wakes it, so it can't change page or press a button. --- */
        if(touched) {
            last_activity_ms = now_ms;
            if(asleep) { pressed = -1; ctrl = -1; }
        }

        state_lock();
        if(g_state.wake_request) {  /* a client changed something */
            g_state.wake_request = false;
            last_activity_ms = now_ms;
        }
        uint32_t idle_timeout_s = g_state.idle_timeout_s;
        /* --- on-screen buttons (Main page only) --- */
        if(g_state.page == 0 && ctrl == CTRL_LIGHT) {
            g_state.lights_on = !g_state.lights_on;
        } else if(g_state.page == 0 && ctrl == CTRL_COLOR) {
            int next = 0;
            for(int i = 0; i < LIGHT_COLOR_COUNT; i++)
                if(memcmp(g_state.lights_rgb, LIGHT_COLORS[i], 3) == 0) { next = (i + 1) % LIGHT_COLOR_COUNT; break; }
            memcpy(g_state.lights_rgb, LIGHT_COLORS[next], 3);
            g_state.lights_on = true;
        } else if(g_state.page == 0 && ctrl == CTRL_BUZZ) {
            g_state.vibrate_request_ms = 300;
        }
        g_state.button[0] = M5.BtnA.isPressed();
        g_state.button[1] = M5.BtnB.isPressed();
        g_state.button[2] = M5.BtnC.isPressed();
        if(pressed >= 0) {
            g_state.button_count[pressed]++;
            g_state.page = (uint8_t)pressed;
        }
        g_state.touched = touched;
        if(touched) { g_state.touch_x = t.x; g_state.touch_y = t.y; }
        uint32_t vib_req = g_state.vibrate_request_ms;
        g_state.vibrate_request_ms = 0;
        uint8_t want_brightness = g_state.brightness;
        bool    want_lights_on = g_state.lights_on;
        uint8_t want_rgb[3];
        memcpy(want_rgb, g_state.lights_rgb, 3);
        uint8_t want_lights_bright = g_state.lights_brightness;
        bool    want_mic = g_state.mic_enabled;
        state_unlock();

        /* --- microphone on/off (Microphone.Enabled over OPC UA) --- */
        if(want_mic != applied_mic) {
            if(want_mic) M5.Mic.begin(); else M5.Mic.end();
            applied_mic = want_mic;
            ESP_LOGI(TAG, "microphone %s", want_mic ? "on" : "off");
        }

        /* --- idle sleep: enter after idle_timeout_s without activity (0 = never) --- */
        bool want_asleep = idle_timeout_s > 0 &&
                           now_ms - last_activity_ms >= idle_timeout_s * 1000u;
        if(want_asleep != asleep) {
            asleep = want_asleep;
            if(asleep) {
                M5.Display.sleep();
            } else {
                M5.Display.wakeup();
                M5.Display.setBrightness(want_brightness);
                applied_brightness = want_brightness;
            }
            state_lock(); g_state.asleep = asleep; state_unlock();
            ESP_LOGI(TAG, "screen %s", asleep ? "asleep (idle)" : "awake");
        }
        /* Lights stay "on" in the settings while asleep, they just go dark */
        bool lights_lit = want_lights_on && !asleep;

        /* --- outputs (screen buttons or OPC UA) --- */
        /* Vibration: a request starts the motor; it is stopped on a later
         * pass once vibrate_until_us has passed (the loop never blocks). */
        if(vib_req) {
            M5.Power.setVibration(180);
            vibrate_until_us = esp_timer_get_time() + (int64_t)vib_req * 1000;
        }
        if(vibrate_until_us && esp_timer_get_time() >= vibrate_until_us) {
            M5.Power.setVibration(0);
            vibrate_until_us = 0;
        }
        if(!asleep && want_brightness != applied_brightness) {
            M5.Display.setBrightness(want_brightness);
            applied_brightness = want_brightness;
        }
        if(M5.Led.isEnabled() &&
           (lights_lit != applied_lights_on || memcmp(want_rgb, applied_rgb, 3) != 0 ||
            want_lights_bright != applied_lights_bright)) {
            /* The 5 V output to the base only powers the side LEDs here, so it
             * is switched off while they are dark (saves battery). */
            if(lights_lit && !applied_lights_on) {
                M5.Power.setExtOutput(true);
                vTaskDelay(pdMS_TO_TICKS(5)); /* let the LEDs power up */
            }
            M5.Led.setBrightness(want_lights_bright);
            if(lights_lit) M5.Led.setAllColor(want_rgb[0], want_rgb[1], want_rgb[2]);
            else           M5.Led.setAllColor(0, 0, 0);
            M5.Led.display();
            if(!lights_lit && applied_lights_on) M5.Power.setExtOutput(false);
            applied_lights_on = lights_lit;
            memcpy(applied_rgb, want_rgb, 3);
            applied_lights_bright = want_lights_bright;
        }
        /* NTP just synced in the OPC UA task: save the time to the RTC */
        if(s_rtc_needs_update) {
            rtc_write_from_system_time();
            s_rtc_needs_update = false;
        }

        /* --- sensors every 200 ms (every second while asleep, to save battery) --- */
        if(now_ms - last_sensor_ms >= (asleep ? 1000u : 200u)) {
            last_sensor_ms = now_ms;
            device_state_t tmp;
            state_lock(); tmp = g_state; state_unlock();
            read_sensors(tmp);
            state_lock();
            /* copy back only the sensor part */
            g_state.battery_level = tmp.battery_level;
            g_state.battery_voltage = tmp.battery_voltage;
            g_state.battery_current = tmp.battery_current;
            g_state.battery_charging = tmp.battery_charging;
            g_state.usb_voltage = tmp.usb_voltage;
            g_state.imu_ok = tmp.imu_ok;
            memcpy(g_state.accel, tmp.accel, sizeof(tmp.accel));
            memcpy(g_state.gyro, tmp.gyro, sizeof(tmp.gyro));
            g_state.imu_temperature = tmp.imu_temperature;
            g_state.pitch = tmp.pitch;
            g_state.roll = tmp.roll;
            g_state.sound_level = tmp.sound_level;
            g_state.rtc_unix = tmp.rtc_unix;
            g_state.uptime_s = tmp.uptime_s;
            g_state.free_heap = tmp.free_heap;
            g_state.wifi_rssi = tmp.wifi_rssi;
            state_unlock();
        }

        /* --- redraw at ~5 fps, or right away after a button or screen tap --- */
        if(!asleep && (pressed >= 0 || ctrl >= 0 || now_ms - last_draw_ms >= 200)) {
            last_draw_ms = now_ms;
            device_state_t snap;
            state_lock(); snap = g_state; state_unlock();
            draw_screen(snap);
        }

        vTaskDelay(pdMS_TO_TICKS(asleep ? 50 : 20)); /* 50 ms still wakes on touch quickly */
    }
}

/* ------------------------------------------------------------------ */
/* Start-up                                                            */
/* ------------------------------------------------------------------ */

/* ESP-IDF entry point. Sets up power management, the shared state and its
 * defaults, the Core2 hardware (M5Unified), the side LEDs, the screen canvas
 * and the clock, starts Wi-Fi, then starts the two tasks and returns. The
 * tasks keep running after app_main returns. */
extern "C" void app_main(void) {
    /* Let the CPU drop from 240 to 80 MHz when it has nothing to do (saves
     * battery). No light sleep: it would stop the microphone and touch reads. */
    esp_pm_config_t pm = {};
    pm.max_freq_mhz = 240;
    pm.min_freq_mhz = 80;
    pm.light_sleep_enable = false;
    if(esp_pm_configure(&pm) != ESP_OK) ESP_LOGW(TAG, "power management not available");

    s_state_mutex = xSemaphoreCreateMutex();
    memset(&g_state, 0, sizeof(g_state));
    g_state.battery_level = -1;
    g_state.brightness = 128;
    strcpy(g_state.display_label, "Display Value");
    g_state.lights_on = false;
    g_state.lights_rgb[0] = g_state.lights_rgb[1] = g_state.lights_rgb[2] = 255;
    g_state.lights_brightness = 64; /* 10 LEDs at full white draw ~0.6 A */
    g_state.idle_timeout_s = 60;    /* screen + lights off after 1 min idle */

    /* Hardware. The Core2 speaker and microphone share one bus, so the
     * speaker is switched off to leave the microphone free. */
    auto cfg = M5.config();
    cfg.internal_spk = false;
    cfg.internal_mic = true;
    M5.begin(cfg);
    M5.Mic.end(); /* off until a client sets Microphone.Enabled (saves battery) */
    M5.Display.setBrightness(g_state.brightness);

    /* Side lights: the M5GO Bottom2 base has 10 SK6812 RGB LEDs on GPIO25
     * (5 per side). M5Unified doesn't know about the base, so attach them. */
    {
        auto bus = std::make_shared<m5::LedBus_RMT>();
        auto bus_cfg = bus->getConfig();
        bus_cfg.pin_data = GPIO_NUM_25;
        bus->setConfig(bus_cfg);
        auto strip = std::make_shared<m5::LED_Strip_Class>();
        auto strip_cfg = strip->getConfig();
        strip_cfg.led_count = 10;
        strip->setConfig(strip_cfg);
        strip->setBus(bus);
        M5.Led.setLedInstance(strip);
        M5.Led.setAutoDisplay(false);
        if(!M5.Led.begin()) ESP_LOGW(TAG, "side LEDs could not be started");
    }

    canvas.setColorDepth(16);
    canvas.setPsram(true);
    canvas.createSprite(M5.Display.width(), M5.Display.height());

    /* Clock: start from the RTC (kept in UTC) until NTP has synced */
    setenv("TZ", LOCAL_TIMEZONE, 1);
    tzset();
    int64_t rtc = rtc_read_unix();
    if(rtc > 0) {
        struct timeval tv;
        tv.tv_sec = (time_t)rtc;
        tv.tv_usec = 0;
        settimeofday(&tv, nullptr);
    }

    wifi_start();

    /* hw on core 1, OPC UA on core 0 (with the Wi-Fi stack). The OPC UA task
     * needs the bigger stack: open62541 builds responses on the stack. */
    xTaskCreatePinnedToCore(hw_task, "hw", 8192, nullptr, 5, nullptr, 1);
    xTaskCreatePinnedToCore(opcua_task, "opcua", 24576, nullptr, 5, nullptr, 0);
}
