/*
 * opcua_server.c - the OPC UA server and its address space.
 *
 * Address space (namespace "urn:m5stack:core2:opcua", string NodeIds):
 *
 *   Objects
 *   └─ M5Core2
 *      ├─ Battery     Level, Voltage, Current, IsCharging, UsbVoltage
 *      ├─ IMU         AccelX/Y/Z, GyroX/Y/Z, Temperature, Pitch, Roll
 *      ├─ Microphone  SoundLevel, Enabled*
 *      ├─ Touch       ButtonA/B/C, ButtonA/B/CCount, IsTouched, X, Y
 *      ├─ Clock       RtcTime
 *      ├─ System      Uptime, FreeHeap, WifiRssi, IpAddress, ClientCount
 *      ├─ Display     Value*, Label*, Unit*, Brightness*, Page, IdleTimeout*, Asleep
 *      ├─ Lights      On*, Red*, Green*, Blue*, Brightness*  (also set from the screen)
 *      └─ Outputs     Vibrate(DurationMs)  <- method
 *   (* = writable by clients; every NodeId is "<Folder>.<Name>", e.g. ns=2;s=Battery.Level)
 *
 * How values move (all in updateCallback, every UPDATE_INTERVAL_MS):
 *   1. Read the writable nodes. A value that differs from g_state (or, for
 *      Lights, from what was last published) was written by a client: copy
 *      it into g_state and flag wake_request (it counts as activity for idle
 *      sleep).
 *   2. Publish the latest sensor readings and status from g_state into the
 *      read-only nodes, and the current Lights state (which the on-screen
 *      buttons may have changed).
 * The hw task in main.cpp picks up anything new in g_state and applies it.
 * The Vibrate method writes straight into g_state.
 *
 * This file is plain C and does not touch the hardware, so it can also be
 * compiled and tested on a PC.
 */
#include <string.h>
#include <stdio.h>

#include "open62541.h"
#include "opcua_server.h"
#include "device_state.h"
#include "ns0_fixups.h"

#define OPCUA_PORT          4840     /* standard OPC UA TCP port */
#define UPDATE_INTERVAL_MS  250      /* how often g_state and the nodes are synced */
#define NAMESPACE_URI       "urn:m5stack:core2:opcua"
#define APPLICATION_URI     "urn:m5stack:core2:opcua:server"
#define FIRMWARE_VERSION    "1.0.1"  /* keep in step with CHANGELOG.md */

/* Namespace index of NAMESPACE_URI. Set when the namespace is registered in
 * opcua_server_run() (it comes out as 2); every node in this file uses it. */
static UA_UInt16 ns = 1;

/* String NodeId in our namespace, e.g. nid("Battery.Level") -> ns=2;s=Battery.Level.
 * The string is not copied: only pass string literals or other long-lived strings. */
static UA_NodeId nid(const char *s) { return UA_NODEID_STRING(ns, (char *)s); }

/* ------------------------------------------------------------------------ */
/* Helpers to build the address space                                        */
/* ------------------------------------------------------------------------ */

/* Add a folder-like object node. parent = NULL puts it directly under the
 * standard Objects folder (Organizes reference); otherwise it becomes a
 * component (HasComponent) of the parent object. Errors are logged, not fatal. */
static void addObject(UA_Server *server, const char *parent, const char *nodeId,
                      const char *name, const char *description) {
    UA_ObjectAttributes attr = UA_ObjectAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT("en-US", (char *)name);
    attr.description = UA_LOCALIZEDTEXT("en-US", (char *)description);

    UA_NodeId parentId = parent ? nid(parent) : UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    UA_NodeId refType  = parent ? UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT)
                                : UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);

    UA_StatusCode rc = UA_Server_addObjectNode(
        server, nid(nodeId), parentId, refType, UA_QUALIFIEDNAME(ns, (char *)name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE), attr, NULL, NULL);
    if(rc != UA_STATUSCODE_GOOD)
        printf("[opcua] addObject %s failed: %s\n", nodeId, UA_StatusCode_name(rc));
}

/* Add a scalar variable of the given type under the parent object.
 *   initial  : starting value (copied by the server, so a local is fine)
 *   writable : allow clients to write it (read-only otherwise)
 * minimumSamplingInterval tells clients the value changes at most every
 * UPDATE_INTERVAL_MS, so subscriptions faster than that are pointless. */
static void addVariable(UA_Server *server, const char *parent, const char *nodeId,
                        const char *name, const char *description,
                        const UA_DataType *type, const void *initial, bool writable) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT("en-US", (char *)name);
    attr.description = UA_LOCALIZEDTEXT("en-US", (char *)description);
    attr.dataType = type->typeId;
    attr.valueRank = UA_VALUERANK_SCALAR;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ | (writable ? UA_ACCESSLEVELMASK_WRITE : 0);
    attr.userAccessLevel = attr.accessLevel;
    attr.minimumSamplingInterval = UPDATE_INTERVAL_MS;
    UA_Variant_setScalar(&attr.value, (void *)initial, type);

    UA_StatusCode rc = UA_Server_addVariableNode(
        server, nid(nodeId), nid(parent), UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
        UA_QUALIFIEDNAME(ns, (char *)name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, NULL, NULL);
    if(rc != UA_STATUSCODE_GOOD)
        printf("[opcua] addVariable %s failed: %s\n", nodeId, UA_StatusCode_name(rc));
}

/* Short-hands for the common types. Arguments: server, parent, nodeId,
 * name, description. They start at 0/false and are read-only (addString
 * takes an initial value and a writable flag). */
static void addFloat(UA_Server *s, const char *p, const char *id, const char *n, const char *d) {
    UA_Float v = 0; addVariable(s, p, id, n, d, &UA_TYPES[UA_TYPES_FLOAT], &v, false);
}
static void addBool(UA_Server *s, const char *p, const char *id, const char *n, const char *d) {
    UA_Boolean v = false; addVariable(s, p, id, n, d, &UA_TYPES[UA_TYPES_BOOLEAN], &v, false);
}
static void addInt32(UA_Server *s, const char *p, const char *id, const char *n, const char *d) {
    UA_Int32 v = 0; addVariable(s, p, id, n, d, &UA_TYPES[UA_TYPES_INT32], &v, false);
}
static void addUInt32(UA_Server *s, const char *p, const char *id, const char *n, const char *d) {
    UA_UInt32 v = 0; addVariable(s, p, id, n, d, &UA_TYPES[UA_TYPES_UINT32], &v, false);
}
static void addInt16(UA_Server *s, const char *p, const char *id, const char *n, const char *d) {
    UA_Int16 v = 0; addVariable(s, p, id, n, d, &UA_TYPES[UA_TYPES_INT16], &v, false);
}
static void addString(UA_Server *s, const char *p, const char *id, const char *n, const char *d,
                      const char *initial, bool writable) {
    UA_String v = UA_STRING((char *)initial);
    addVariable(s, p, id, n, d, &UA_TYPES[UA_TYPES_STRING], &v, writable);
}

/* ------------------------------------------------------------------------ */
/* Helpers to write / read node values                                       */
/* ------------------------------------------------------------------------ */

/* Set a variable node's value from the server side (the value is copied).
 * The type must match the node's DataType or the write is rejected. The w*()
 * short-hands below wrap it for each type used in the address space. */
static void writeScalar(UA_Server *server, const char *nodeId,
                        const void *value, const UA_DataType *type) {
    UA_Variant v;
    UA_Variant_setScalar(&v, (void *)value, type);
    UA_Server_writeValue(server, nid(nodeId), v);
}
static void wFloat(UA_Server *s, const char *id, float x)    { UA_Float v = x;   writeScalar(s, id, &v, &UA_TYPES[UA_TYPES_FLOAT]); }
static void wBool(UA_Server *s, const char *id, bool x)      { UA_Boolean v = x; writeScalar(s, id, &v, &UA_TYPES[UA_TYPES_BOOLEAN]); }
static void wInt32(UA_Server *s, const char *id, int32_t x)  { UA_Int32 v = x;   writeScalar(s, id, &v, &UA_TYPES[UA_TYPES_INT32]); }
static void wUInt32(UA_Server *s, const char *id, uint32_t x){ UA_UInt32 v = x;  writeScalar(s, id, &v, &UA_TYPES[UA_TYPES_UINT32]); }
static void wInt16(UA_Server *s, const char *id, int16_t x)  { UA_Int16 v = x;   writeScalar(s, id, &v, &UA_TYPES[UA_TYPES_INT16]); }
static void wByte(UA_Server *s, const char *id, uint8_t x)   { UA_Byte v = x;    writeScalar(s, id, &v, &UA_TYPES[UA_TYPES_BYTE]); }
static void wString(UA_Server *s, const char *id, const char *x) {
    UA_String v = UA_STRING((char *)x); writeScalar(s, id, &v, &UA_TYPES[UA_TYPES_STRING]);
}

/* read*(): get a node's current value into *out. Each returns false (and
 * leaves *out unchanged) if the read fails or the node holds a different
 * type, so the caller keeps its old value instead of using garbage. */
static bool readDouble(UA_Server *server, const char *nodeId, double *out) {
    UA_Variant v; UA_Variant_init(&v);
    bool ok = UA_Server_readValue(server, nid(nodeId), &v) == UA_STATUSCODE_GOOD &&
              UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_DOUBLE]);
    if(ok) *out = *(UA_Double *)v.data;
    UA_Variant_clear(&v);
    return ok;
}
static bool readByte(UA_Server *server, const char *nodeId, uint8_t *out) {
    UA_Variant v; UA_Variant_init(&v);
    bool ok = UA_Server_readValue(server, nid(nodeId), &v) == UA_STATUSCODE_GOOD &&
              UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_BYTE]);
    if(ok) *out = *(UA_Byte *)v.data;
    UA_Variant_clear(&v);
    return ok;
}
static bool readUInt32(UA_Server *server, const char *nodeId, uint32_t *out) {
    UA_Variant v; UA_Variant_init(&v);
    bool ok = UA_Server_readValue(server, nid(nodeId), &v) == UA_STATUSCODE_GOOD &&
              UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_UINT32]);
    if(ok) *out = *(UA_UInt32 *)v.data;
    UA_Variant_clear(&v);
    return ok;
}
static bool readBool(UA_Server *server, const char *nodeId, bool *out) {
    UA_Variant v; UA_Variant_init(&v);
    bool ok = UA_Server_readValue(server, nid(nodeId), &v) == UA_STATUSCODE_GOOD &&
              UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_BOOLEAN]);
    if(ok) *out = *(UA_Boolean *)v.data;
    UA_Variant_clear(&v);
    return ok;
}
/* Copies the string into out (outSize bytes) as a C string, cutting it off if
 * it is too long. OPC UA strings are not NUL-terminated, hence the copy. */
static bool readString(UA_Server *server, const char *nodeId, char *out, size_t outSize) {
    UA_Variant v; UA_Variant_init(&v);
    bool ok = UA_Server_readValue(server, nid(nodeId), &v) == UA_STATUSCODE_GOOD &&
              UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_STRING]);
    if(ok) {
        const UA_String *s = (const UA_String *)v.data;
        size_t n = s->length < outSize - 1 ? s->length : outSize - 1;
        if(n > 0) memcpy(out, s->data, n);
        out[n] = '\0';
    }
    UA_Variant_clear(&v);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Periodic sync between g_state and the address space                       */
/* ------------------------------------------------------------------------ */

/* The lights can also be changed on the screen, so they sync both ways:
 * a node value that differs from what we last published came from a client. */
static const char *LIGHT_RGB_NODES[3] = {"Lights.Red", "Lights.Green", "Lights.Blue"};
static bool    lastLightsOn;
static uint8_t lastLightsRgb[3];
static uint8_t lastLightsBrightness;

/* Repeated server callback (every UPDATE_INTERVAL_MS, on the OPC UA task):
 * syncs g_state and the address space as described at the top of the file.
 * Node reads and writes happen outside state_lock(); only the copying into
 * and out of g_state happens under it. */
static void updateCallback(UA_Server *server, void *data) {
    (void)data;

    /* 1. Read every writable node (Lights, Display, Microphone.Enabled) */
    bool lightsOn = false; uint8_t lightsRgb[3] = {0}, lightsBright = 0;
    bool okLightsOn = readBool(server, "Lights.On", &lightsOn);
    bool okRgb[3];
    for(int i = 0; i < 3; i++) okRgb[i] = readByte(server, LIGHT_RGB_NODES[i], &lightsRgb[i]);
    bool okLightsBright = readByte(server, "Lights.Brightness", &lightsBright);

    double value = 0; uint8_t brightness = 0;
    char label[sizeof(g_state.display_label)];
    char unit[sizeof(g_state.display_unit)];
    bool okValue = readDouble(server, "Display.Value", &value);
    bool okBright = readByte(server, "Display.Brightness", &brightness);
    bool okLabel = readString(server, "Display.Label", label, sizeof(label));
    bool okUnit = readString(server, "Display.Unit", unit, sizeof(unit));
    uint32_t idleTimeout = 0;
    bool okIdle = readUInt32(server, "Display.IdleTimeout", &idleTimeout);
    bool micEnabled = false;
    bool okMic = readBool(server, "Microphone.Enabled", &micEnabled);
    UA_ServerStatistics stats = UA_Server_getStatistics(server);

    /* 2. Copy client changes into g_state and take a snapshot of it (keep the
     *    lock short). Any value a client changed also counts as activity
     *    (wakes the screen). Display/Microphone settings are only ever changed
     *    by clients, so they are compared with g_state; Lights are compared
     *    with what was last published, because the screen changes them too. */
    device_state_t s;
    bool changed = false;
    state_lock();
    if(okValue && memcmp(&value, &g_state.display_value, sizeof(value)) != 0) { /* NaN-safe */
        g_state.display_value = value; changed = true;
    }
    if(okBright && brightness != g_state.brightness) {
        g_state.brightness = brightness; changed = true;
    }
    if(okLabel && strcmp(label, g_state.display_label) != 0) {
        memcpy(g_state.display_label, label, sizeof(label)); changed = true;
    }
    if(okUnit && strcmp(unit, g_state.display_unit) != 0) {
        memcpy(g_state.display_unit, unit, sizeof(unit)); changed = true;
    }
    if(okIdle && idleTimeout != g_state.idle_timeout_s) {
        g_state.idle_timeout_s = idleTimeout; changed = true;
    }
    if(okMic && micEnabled != g_state.mic_enabled) {
        g_state.mic_enabled = micEnabled; changed = true;
    }
    g_state.client_count = (uint32_t)stats.ss.currentSessionCount;
    if(okLightsOn && lightsOn != lastLightsOn) {
        g_state.lights_on = lightsOn; changed = true;
    }
    for(int i = 0; i < 3; i++)
        if(okRgb[i] && lightsRgb[i] != lastLightsRgb[i]) {
            g_state.lights_rgb[i] = lightsRgb[i]; changed = true;
        }
    if(okLightsBright && lightsBright != lastLightsBrightness) {
        g_state.lights_brightness = lightsBright; changed = true;
    }
    if(changed) g_state.wake_request = true;
    s = g_state;
    state_unlock();

    /* 3. Publish the latest sensor readings and status (read-only nodes) */
    wInt32 (server, "Battery.Level",      s.battery_level);
    wFloat (server, "Battery.Voltage",    s.battery_voltage);
    wFloat (server, "Battery.Current",    s.battery_current);
    wBool  (server, "Battery.IsCharging", s.battery_charging);
    wFloat (server, "Battery.UsbVoltage", s.usb_voltage);

    if(s.imu_ok) {
        wFloat(server, "IMU.AccelX", s.accel[0]);
        wFloat(server, "IMU.AccelY", s.accel[1]);
        wFloat(server, "IMU.AccelZ", s.accel[2]);
        wFloat(server, "IMU.GyroX",  s.gyro[0]);
        wFloat(server, "IMU.GyroY",  s.gyro[1]);
        wFloat(server, "IMU.GyroZ",  s.gyro[2]);
        wFloat(server, "IMU.Temperature", s.imu_temperature);
        wFloat(server, "IMU.Pitch", s.pitch);
        wFloat(server, "IMU.Roll",  s.roll);
    }

    wFloat (server, "Microphone.SoundLevel", s.sound_level);

    wBool  (server, "Touch.ButtonA", s.button[0]);
    wBool  (server, "Touch.ButtonB", s.button[1]);
    wBool  (server, "Touch.ButtonC", s.button[2]);
    wUInt32(server, "Touch.ButtonACount", s.button_count[0]);
    wUInt32(server, "Touch.ButtonBCount", s.button_count[1]);
    wUInt32(server, "Touch.ButtonCCount", s.button_count[2]);
    wBool  (server, "Touch.IsTouched", s.touched);
    wInt16 (server, "Touch.X", s.touch_x);
    wInt16 (server, "Touch.Y", s.touch_y);

    if(s.rtc_unix > 0) {
        UA_DateTime dt = (UA_DateTime)s.rtc_unix * UA_DATETIME_SEC + UA_DATETIME_UNIX_EPOCH;
        writeScalar(server, "Clock.RtcTime", &dt, &UA_TYPES[UA_TYPES_DATETIME]);
    }

    wUInt32(server, "System.Uptime",      s.uptime_s);
    wUInt32(server, "System.FreeHeap",    s.free_heap);
    wInt32 (server, "System.WifiRssi",    s.wifi_rssi);
    wString(server, "System.IpAddress",   s.ip_address);
    wUInt32(server, "System.ClientCount", s.client_count);

    wByte  (server, "Display.Page", s.page);
    wBool  (server, "Display.Asleep", s.asleep);

    /* Lights: publish the current state (it may have come from the screen) */
    wBool(server, "Lights.On", s.lights_on);
    lastLightsOn = s.lights_on;
    for(int i = 0; i < 3; i++) {
        wByte(server, LIGHT_RGB_NODES[i], s.lights_rgb[i]);
        lastLightsRgb[i] = s.lights_rgb[i];
    }
    wByte(server, "Lights.Brightness", s.lights_brightness);
    lastLightsBrightness = s.lights_brightness;
}

/* ------------------------------------------------------------------------ */
/* Method: Outputs.Vibrate(DurationMs)                                       */
/* ------------------------------------------------------------------------ */

/* Called by open62541 when a client calls Outputs.Vibrate. Expects one UInt32
 * argument (milliseconds, capped at 5000) and hands the request to the hw task
 * through g_state.vibrate_request_ms; it returns at once without waiting for
 * the motor. Returns BadInvalidArgument if the argument is missing or not a
 * UInt32. The signature is fixed by UA_MethodCallback. */
static UA_StatusCode
vibrateMethod(UA_Server *server, const UA_NodeId *sessionId, void *sessionContext,
              const UA_NodeId *methodId, void *methodContext,
              const UA_NodeId *objectId, void *objectContext,
              size_t inputSize, const UA_Variant *input,
              size_t outputSize, UA_Variant *output) {
    (void)server; (void)sessionId; (void)sessionContext; (void)methodId;
    (void)methodContext; (void)objectId; (void)objectContext;
    (void)outputSize; (void)output;
    if(inputSize < 1 || !UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_UINT32]))
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    UA_UInt32 ms = *(UA_UInt32 *)input[0].data;
    if(ms > 5000) ms = 5000; /* be kind to the motor and the battery */
    state_lock();
    g_state.vibrate_request_ms = ms;
    g_state.wake_request = true;
    state_unlock();
    return UA_STATUSCODE_GOOD;
}

/* ------------------------------------------------------------------------ */
/* Address space                                                             */
/* ------------------------------------------------------------------------ */

/* Create every object, variable and method listed at the top of the file.
 * Writable nodes start from the defaults already in g_state (set in
 * app_main); sensor nodes start at 0 until the first updateCallback.
 * To add a value: add a field to device_state_t, a node here, and a line in
 * updateCallback that publishes it (or reads it back if writable). */
static void buildAddressSpace(UA_Server *server) {
    device_state_t s;
    state_lock(); s = g_state; state_unlock();

    addObject(server, NULL, "M5Core2", "M5Core2", "M5Stack Core2 device");

    addObject(server, "M5Core2", "Battery", "Battery", "Battery and power management");
    addInt32 (server, "Battery", "Battery.Level",      "Level",      "Battery charge in percent");
    addFloat (server, "Battery", "Battery.Voltage",    "Voltage",    "Battery voltage in V");
    addFloat (server, "Battery", "Battery.Current",    "Current",    "Battery current in mA (+ charging, - discharging)");
    addBool  (server, "Battery", "Battery.IsCharging", "IsCharging", "True while the battery is charging");
    addFloat (server, "Battery", "Battery.UsbVoltage", "UsbVoltage", "USB (VBUS) voltage in V");

    addObject(server, "M5Core2", "IMU", "IMU", "6-axis motion sensor");
    addFloat (server, "IMU", "IMU.AccelX", "AccelX", "Acceleration X in g");
    addFloat (server, "IMU", "IMU.AccelY", "AccelY", "Acceleration Y in g");
    addFloat (server, "IMU", "IMU.AccelZ", "AccelZ", "Acceleration Z in g");
    addFloat (server, "IMU", "IMU.GyroX",  "GyroX",  "Rotation rate X in deg/s");
    addFloat (server, "IMU", "IMU.GyroY",  "GyroY",  "Rotation rate Y in deg/s");
    addFloat (server, "IMU", "IMU.GyroZ",  "GyroZ",  "Rotation rate Z in deg/s");
    addFloat (server, "IMU", "IMU.Temperature", "Temperature", "IMU chip temperature in degC (runs warmer than the room)");
    addFloat (server, "IMU", "IMU.Pitch", "Pitch", "Tilt forward/back in degrees, worked out from gravity");
    addFloat (server, "IMU", "IMU.Roll",  "Roll",  "Tilt left/right in degrees, worked out from gravity");

    addObject(server, "M5Core2", "Microphone", "Microphone", "Built-in PDM microphone");
    addFloat (server, "Microphone", "Microphone.SoundLevel", "SoundLevel",
              "Sound level in dBFS (0 = loudest, about -90 = silence; -96 while the microphone is off)");
    UA_Boolean micOn = s.mic_enabled;
    addVariable(server, "Microphone", "Microphone.Enabled", "Enabled",
                "Turn the microphone on or off (off saves battery)",
                &UA_TYPES[UA_TYPES_BOOLEAN], &micOn, true);

    addObject(server, "M5Core2", "Touch", "Touch", "Touch screen and the three touch buttons");
    addBool  (server, "Touch", "Touch.ButtonA", "ButtonA", "Left touch button pressed");
    addBool  (server, "Touch", "Touch.ButtonB", "ButtonB", "Middle touch button pressed");
    addBool  (server, "Touch", "Touch.ButtonC", "ButtonC", "Right touch button pressed");
    addUInt32(server, "Touch", "Touch.ButtonACount", "ButtonACount", "Times button A was pressed since boot");
    addUInt32(server, "Touch", "Touch.ButtonBCount", "ButtonBCount", "Times button B was pressed since boot");
    addUInt32(server, "Touch", "Touch.ButtonCCount", "ButtonCCount", "Times button C was pressed since boot");
    addBool  (server, "Touch", "Touch.IsTouched", "IsTouched", "Screen is being touched");
    addInt16 (server, "Touch", "Touch.X", "X", "Last touch X position in pixels");
    addInt16 (server, "Touch", "Touch.Y", "Y", "Last touch Y position in pixels");

    addObject(server, "M5Core2", "Clock", "Clock", "Real-time clock");
    UA_DateTime dt = 0;
    addVariable(server, "Clock", "Clock.RtcTime", "RtcTime", "Time from the on-board RTC (UTC)",
                &UA_TYPES[UA_TYPES_DATETIME], &dt, false);

    addObject(server, "M5Core2", "System", "System", "Device status");
    addUInt32(server, "System", "System.Uptime",      "Uptime",      "Seconds since boot");
    addUInt32(server, "System", "System.FreeHeap",    "FreeHeap",    "Free memory in bytes");
    addInt32 (server, "System", "System.WifiRssi",    "WifiRssi",    "Wi-Fi signal strength in dBm");
    addString(server, "System", "System.IpAddress",   "IpAddress",   "Current IP address", s.ip_address, false);
    addUInt32(server, "System", "System.ClientCount", "ClientCount", "Number of connected OPC UA sessions");
    addString(server, "System", "System.FirmwareVersion", "FirmwareVersion", "Firmware version",
              FIRMWARE_VERSION, false);

    addObject(server, "M5Core2", "Display", "Display", "What the screen shows (writable by clients)");
    UA_Double dv = s.display_value;
    addVariable(server, "Display", "Display.Value", "Value",
                "Big number shown on the screen - write to it from your client",
                &UA_TYPES[UA_TYPES_DOUBLE], &dv, true);
    addString(server, "Display", "Display.Label", "Label", "Caption shown above the value",
              s.display_label, true);
    addString(server, "Display", "Display.Unit", "Unit", "Unit shown after the value",
              s.display_unit, true);
    UA_Byte bright = s.brightness;
    addVariable(server, "Display", "Display.Brightness", "Brightness", "Backlight 0..255",
                &UA_TYPES[UA_TYPES_BYTE], &bright, true);
    UA_Byte page = s.page;
    addVariable(server, "Display", "Display.Page", "Page", "Screen page currently shown (0..2)",
                &UA_TYPES[UA_TYPES_BYTE], &page, false);
    UA_UInt32 idle = s.idle_timeout_s;
    addVariable(server, "Display", "Display.IdleTimeout", "IdleTimeout",
                "Screen and lights turn off after this many seconds without activity (0 = never)",
                &UA_TYPES[UA_TYPES_UINT32], &idle, true);
    addBool(server, "Display", "Display.Asleep", "Asleep",
            "True while the screen and lights are off because of the idle timeout");

    addObject(server, "M5Core2", "Lights", "Lights", "Side LEDs on the M5GO Bottom2 base (also set from the screen)");
    UA_Boolean lightsOn = s.lights_on;
    addVariable(server, "Lights", "Lights.On", "On", "Lights on or off",
                &UA_TYPES[UA_TYPES_BOOLEAN], &lightsOn, true);
    static const char *rgbNames[3] = {"Red", "Green", "Blue"};
    for(int i = 0; i < 3; i++) {
        UA_Byte c = s.lights_rgb[i];
        char desc[48];
        snprintf(desc, sizeof(desc), "%s part of the colour, 0..255", rgbNames[i]);
        addVariable(server, "Lights", LIGHT_RGB_NODES[i], rgbNames[i], desc,
                    &UA_TYPES[UA_TYPES_BYTE], &c, true);
        lastLightsRgb[i] = c;
    }
    UA_Byte lightsBright = s.lights_brightness;
    addVariable(server, "Lights", "Lights.Brightness", "Brightness", "LED brightness 0..255",
                &UA_TYPES[UA_TYPES_BYTE], &lightsBright, true);
    /* Start the "last published" copies equal to the nodes, so the first
     * updateCallback doesn't mistake the initial values for a client write */
    lastLightsOn = s.lights_on;
    lastLightsBrightness = s.lights_brightness;

    addObject(server, "M5Core2", "Outputs", "Outputs", "Things a client can make the device do");
    UA_Argument arg;
    UA_Argument_init(&arg);
    arg.name = UA_STRING("DurationMs");
    arg.description = UA_LOCALIZEDTEXT("en-US", "How long to vibrate in milliseconds (max 5000)");
    arg.dataType = UA_TYPES[UA_TYPES_UINT32].typeId;
    arg.valueRank = UA_VALUERANK_SCALAR;
    UA_MethodAttributes ma = UA_MethodAttributes_default;
    ma.displayName = UA_LOCALIZEDTEXT("en-US", "Vibrate");
    ma.description = UA_LOCALIZEDTEXT("en-US", "Run the vibration motor");
    ma.executable = true;
    ma.userExecutable = true;
    UA_StatusCode rc = UA_Server_addMethodNode(
        server, nid("Outputs.Vibrate"), nid("Outputs"),
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT), UA_QUALIFIEDNAME(ns, "Vibrate"),
        ma, vibrateMethod, 1, &arg, 0, NULL, NULL, NULL);
    if(rc != UA_STATUSCODE_GOOD)
        printf("[opcua] addMethod Vibrate failed: %s\n", UA_StatusCode_name(rc));
}

/* ------------------------------------------------------------------------ */
/* Server                                                                    */
/* ------------------------------------------------------------------------ */

/* Replace a heap-allocated string in the server config, freeing the old one */
static void setString(UA_String *dst, const char *src) {
    UA_String_clear(dst);
    *dst = UA_STRING_ALLOC(src);
}

/* Configure and start the server, build the address space, then serve
 * requests forever (see opcua_server.h). Each UA_Server_run_iterate() call
 * waits for network activity or the next timer (such as updateCallback), so
 * the task sleeps while idle. Only returns if the server can't be created or
 * started. */
void opcua_server_run(void) {
    UA_ServerConfig config;
    memset(&config, 0, sizeof(config));
    /* One endpoint on port 4840, SecurityPolicy None (certificates come later) */
    UA_ServerConfig_setMinimal(&config, OPCUA_PORT, NULL);

    /* Who we are */
    setString(&config.applicationDescription.applicationUri, APPLICATION_URI);
    setString(&config.applicationDescription.productUri, "urn:m5stack:core2");
    UA_LocalizedText_clear(&config.applicationDescription.applicationName);
    config.applicationDescription.applicationName =
        UA_LOCALIZEDTEXT_ALLOC("en-US", "M5Stack Core2 OPC UA Server");
    setString(&config.buildInfo.productUri, "urn:m5stack:core2");
    setString(&config.buildInfo.manufacturerName, "DIY");
    setString(&config.buildInfo.productName, "M5Stack Core2 OPC UA Server");
    setString(&config.buildInfo.softwareVersion, FIRMWARE_VERSION);

    /* Keep resource use sensible for a microcontroller */
    config.maxSecureChannels = 4;
    config.maxSessions = 4;
    config.maxSubscriptions = 20;
    config.maxMonitoredItems = 200;

    /* Some clients (e.g. Matrikon OPC UA Explorer) send FindServers with a
     * zero RequestHeader timestamp. The default rejects those with
     * BadInvalidTimestamp, which those clients don't handle, so accept them. */
    config.verifyRequestTimestamp = UA_RULEHANDLING_ACCEPT;

    UA_Server *server = UA_Server_newWithConfig(&config);
    if(!server) {
        printf("[opcua] could not create server (out of memory?)\n");
        return;
    }
    /* Strict clients (e.g. ABB 800xA) reject standard nodes whose value is
     * empty in this open62541 build; give them proper values (ns0_fixups.c) */
    ns0_fill_missing_values(server);

    ns = UA_Server_addNamespace(server, NAMESPACE_URI);

    buildAddressSpace(server);
    UA_Server_addRepeatedCallback(server, updateCallback, NULL, UPDATE_INTERVAL_MS, NULL);

    UA_StatusCode rc = UA_Server_run_startup(server);
    if(rc != UA_STATUSCODE_GOOD) {
        printf("[opcua] startup failed: %s\n", UA_StatusCode_name(rc));
        UA_Server_delete(server);
        return;
    }
    printf("[opcua] server running on port %d\n", OPCUA_PORT);

    for(;;)
        UA_Server_run_iterate(server, true);
}
