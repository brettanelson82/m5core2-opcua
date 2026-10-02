/*
 * device_state.h - the one shared "whiteboard" between the hardware task
 * (sensors, outputs, screen), the OPC UA server task and the Wi-Fi events.
 * No module calls another directly; they only meet here.
 *
 *   hw task        --sensor values, page, asleep-->      g_state --> OPC UA nodes
 *   OPC UA clients --Display / Lights / Mic settings-->  g_state --> hw task (applies them)
 *   screen buttons --Lights, vibrate-->                  g_state --> hw task and OPC UA nodes
 *   Wi-Fi events   --ip_address-->                       g_state --> screen and OPC UA
 *
 * Always wrap access in state_lock() / state_unlock(), and keep it short:
 * copy what you need, unlock, then do the slow work. Defaults are set in
 * app_main (main.cpp) before the tasks start.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* ---------- Sensors (written by the hardware task) ---------- */
    /* Battery / power management chip (AXP192 or AXP2101) */
    int32_t battery_level;      /* %  (0..100, -1 = unknown) */
    float   battery_voltage;    /* V  */
    float   battery_current;    /* mA (+ charging / - discharging) */
    bool    battery_charging;
    float   usb_voltage;        /* V  on the 5 V bus: USB input, or the Core2's own
                                   5 V output while the side lights are on */

    /* IMU (MPU6886 / BMI270, auto-detected) */
    bool    imu_ok;
    float   accel[3];           /* g      X, Y, Z */
    float   gyro[3];            /* deg/s  X, Y, Z */
    float   imu_temperature;    /* degC   */
    float   pitch, roll;        /* deg, tilt worked out from gravity (accel) */

    /* Microphone */
    bool    mic_enabled;        /* set by OPC UA clients (default off, saves battery) */
    float   sound_level;        /* dBFS (0 = loudest, about -90 = silence; -96 while off) */

    /* Touch screen and the three touch buttons below the screen */
    bool     button[3];         /* A, B, C currently pressed */
    uint32_t button_count[3];   /* how many times each was pressed */
    bool     touched;
    int16_t  touch_x, touch_y;

    /* Real-time clock (BM8563), as Unix seconds UTC; 0 = not valid */
    int64_t  rtc_unix;

    /* System */
    uint32_t uptime_s;
    uint32_t free_heap;         /* bytes, internal RAM + PSRAM */
    int32_t  wifi_rssi;         /* dBm, 0 while disconnected */
    char     ip_address[16];    /* e.g. "192.168.1.20"; "" while disconnected (written by wifi_net.c) */
    uint32_t client_count;      /* OPC UA sessions currently open (written by the OPC UA task) */

    /* ---------- Display ---------- */
    /* Set by OPC UA clients: */
    double   display_value;     /* big number on the Main page */
    char     display_label[32]; /* caption above it */
    char     display_unit[16];  /* unit below it */
    uint8_t  brightness;        /* backlight 0..255 */
    uint32_t idle_timeout_s;    /* screen + lights off after this long without activity (0 = never) */
    /* Set by the hw task: */
    uint8_t  page;              /* screen page currently shown: 0 Main, 1 Motion, 2 System */
    bool     asleep;            /* screen + lights are off because of the idle timeout */

    /* ---------- Lights (M5GO Bottom2 side LEDs, set from screen or OPC UA) ---------- */
    bool     lights_on;
    uint8_t  lights_rgb[3];     /* R, G, B 0..255 */
    uint8_t  lights_brightness; /* 0..255 */

    /* ---------- One-shot commands: set by the requester, cleared by the hw task ---------- */
    uint32_t vibrate_request_ms; /* >0 : vibrate for this long (Vibrate method or Buzz button) */
    bool     wake_request;       /* a client changed something: counts as activity */
} device_state_t;

/* The single instance, defined in main.cpp */
extern device_state_t g_state;

/* Take / release the mutex that guards g_state (FreeRTOS mutex, blocks until free) */
void state_lock(void);
void state_unlock(void);

#ifdef __cplusplus
}
#endif
