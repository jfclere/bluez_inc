/*
 *   Copyright (c) 2022 Martijn van Welie
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 *
 */

#include <glib.h>
#include <stdio.h>
#include <signal.h>
#include <stdint.h>
#include <fcntl.h>
#include "adapter.h"
#include "device.h"
#include "logger.h"
#include "agent.h"
#include "parser.h"

#define TAG "Main"
#define HTS_SERVICE_UUID "0000181a-0000-1000-8000-00805f9b34fb"
/* we use 2A6E */
#define TEMPERATURE_CHAR_UUID "00002a6e-0000-1000-8000-00805f9b34fb"
#define PRESSURE_CHAR_UUID "00002a6d-0000-1000-8000-00805f9b34fb"
#define HUMIDITY_CHAR_UUID "00002a6f-0000-1000-8000-00805f9b34fb"
#define DIS_SERVICE "0000180a-0000-1000-8000-00805f9b34fb"
#define DIS_MANUFACTURER_CHAR "00002a29-0000-1000-8000-00805f9b34fb"
#define DIS_MODEL_CHAR "00002a24-0000-1000-8000-00805f9b34fb"
#define CUD_CHAR "00002901-0000-1000-8000-00805f9b34fb"

GMainLoop *loop = NULL;
Adapter *default_adapter = NULL;
Agent *agent = NULL;
Device *default_device = NULL;

struct info {
   double temp;
   double pres;
   double humi;
};

/* place to store the temperature, pressure and humidity */
struct info info;
int tries = 0;
int done = 0;
#define HAS_TEMP 0x01
#define HAS_PRES 0x02
#define HAS_HUMI 0x04
#define IS_DONE  0x07


void on_connection_state_changed(Device *device, ConnectionState state, const GError *error) {
    if (error != NULL) {
        log_debug(TAG, "(dis)connect failed (error %d: %s)", error->code, error->message);
        return;
    }

    log_debug(TAG, "'%s' (%s) state: %s (%d)", binc_device_get_name(device), binc_device_get_address(device),
              binc_device_get_connection_state_name(device), state);

    if (state == BINC_DISCONNECTED) {
        // Remove devices immediately of they are not bonded
        if (binc_device_get_bonding_state(device) != BINC_BONDED) {
            binc_adapter_remove_device(default_adapter, device);
        }
    }
}

void on_bonding_state_changed(Device *device, BondingState new_state, BondingState old_state, const GError *error) {
    log_debug(TAG, "bonding state changed from %d to %d", old_state, new_state);
}

void on_notification_state_changed(Device *device, Characteristic *characteristic, const GError *error) {
    const char *uuid = binc_characteristic_get_uuid(characteristic);

    if (error != NULL) {
        log_debug(TAG, "notifying <%s> failed (error %d: %s)", uuid, error->code, error->message);
        return;
    }

    log_debug(TAG, "<%s> notifying %s", uuid, binc_characteristic_is_notifying(characteristic) ? "true" : "false");
}

void on_notify(Device *device, Characteristic *characteristic, const GByteArray *byteArray) {
    const char *uuid = binc_characteristic_get_uuid(characteristic);
    Parser *parser = parser_create(byteArray, LITTLE_ENDIAN);
    parser_set_offset(parser, 1);
    if (g_str_equal(uuid, TEMPERATURE_CHAR_UUID)) {
        double temperature = parser_get_float(parser);
        log_debug(TAG, "temperature %.1f", temperature);
    }
    parser_free(parser);
}

void on_read(Device *device, Characteristic *characteristic, const GByteArray *byteArray, const GError *error) {
    const char *uuid = binc_characteristic_get_uuid(characteristic);
    if (error != NULL) {
        log_debug(TAG, "failed to read '%s' (error %d: %s)", uuid, error->code, error->message);
        return;
    }
    log_debug(TAG, "on_read %s", uuid);

    if (byteArray == NULL) return;

    Parser *parser = parser_create(byteArray, LITTLE_ENDIAN);
    if (g_str_equal(uuid, DIS_MANUFACTURER_CHAR)) {
        GString *manufacturer = parser_get_string(parser);
        log_debug(TAG, "manufacturer = %s", manufacturer->str);
        g_string_free(manufacturer, TRUE);
    } else if (g_str_equal(uuid, DIS_MODEL_CHAR)) {
        GString *model = parser_get_string(parser);
        log_debug(TAG, "model = %s", model->str);
        g_string_free(model, TRUE);
    } else if (g_str_equal(uuid, TEMPERATURE_CHAR_UUID)) {
        log_debug(TAG, "Temperature to parse...");
        int16_t temp = parser_get_sint16(parser);
        log_debug(TAG, "temp = %d", temp);
        info.temp = temp / 100.0;
        done = done | HAS_TEMP;
    } else if (g_str_equal(uuid, PRESSURE_CHAR_UUID)) {
        log_debug(TAG, "Pressure to parse...");
        uint32_t pres = parser_get_uint32(parser);
        log_debug(TAG, "pres = %d", pres);
        info.pres = pres / 1000.0;
        done = done | HAS_PRES;
    } else if (g_str_equal(uuid, HUMIDITY_CHAR_UUID)) {
        log_debug(TAG, "Humidity to parse...");
        uint16_t humi = parser_get_uint16(parser);
        log_debug(TAG, "hum = %d", humi);
        info.humi = humi / 100.0;
        done = done | HAS_HUMI;
    }
    if (done == IS_DONE) {
        char mess[100];
        char *tmpname = "/tmp/data.txt";
        sprintf(mess, "%d %4.2f %6.2f %4.2f", 0, info.temp, info.pres, info.humi);
        int fd = open(tmpname, O_WRONLY | O_APPEND | O_CREAT | O_TRUNC, 0644);
        write(fd, mess, strlen(mess));
        close(fd);
    }
    parser_free(parser);
    if (done == IS_DONE)
        tries = 10; /* Done exit in the next loop */
}

void on_write(Device *device, Characteristic *characteristic, const GByteArray *byteArray, const GError *error) {
    log_debug(TAG, "on write");
}

void on_desc_read(Device *device, Descriptor *descriptor, const GByteArray *byteArray, const GError *error) {
    log_debug(TAG, "on descriptor read");
    Parser *parser = parser_create(byteArray, LITTLE_ENDIAN);
    GString *parsed_string = parser_get_string(parser);
    log_debug(TAG, "CUD %s", parsed_string->str);
    parser_free(parser);
}

void on_services_resolved(Device *device) {
    log_debug(TAG, "'%s' services resolved", binc_device_get_name(device));

    binc_device_read_char(device, DIS_SERVICE, DIS_MANUFACTURER_CHAR);
    binc_device_read_char(device, DIS_SERVICE, DIS_MODEL_CHAR);
    // binc_device_start_notify(device, HTS_SERVICE_UUID, TEMPERATURE_CHAR_UUID);
    binc_device_read_char(device, HTS_SERVICE_UUID, TEMPERATURE_CHAR_UUID);
    binc_device_read_char(device, HTS_SERVICE_UUID, PRESSURE_CHAR_UUID);
    binc_device_read_char(device, HTS_SERVICE_UUID, HUMIDITY_CHAR_UUID);
    // binc_device_read_desc(device, HTS_SERVICE_UUID, TEMPERATURE_CHAR_UUID, CUD_CHAR);
}

gboolean on_request_authorization(Device *device) {
    log_debug(TAG, "requesting authorization for '%s", binc_device_get_name(device));
    return TRUE;
}

guint32 on_request_passkey(Device *device) {
    guint32 pass = 000000;
    log_debug(TAG, "requesting passkey for '%s", binc_device_get_name(device));
    log_debug(TAG, "Enter 6 digit pin code: ");
    int result = fscanf(stdin, "%d", &pass);
    if (result != 1) {
        log_debug(TAG, "didn't read a pin code");
    }
    return pass;
}

void on_scan_result(Adapter *adapter, Device *device) {
    /* Only our device */
    const char* name = binc_device_get_name(device);
    if (name != NULL && g_str_has_prefix(name, "blecenv")) {
        char *deviceToString = binc_device_to_string(device);
        log_debug(TAG, deviceToString);
        g_free(deviceToString);
        binc_adapter_stop_discovery(adapter);

        binc_device_set_connection_state_change_cb(device, &on_connection_state_changed);
        binc_device_set_services_resolved_cb(device, &on_services_resolved);
        binc_device_set_bonding_state_changed_cb(device, &on_bonding_state_changed);
        binc_device_set_read_char_cb(device, &on_read);
        binc_device_set_write_char_cb(device, &on_write);
        binc_device_set_notify_char_cb(device, &on_notify);
        binc_device_set_notify_state_cb(device, &on_notification_state_changed);
        binc_device_set_read_desc_cb(device, &on_desc_read);
        binc_device_connect(device);
        default_device = device;
    } else {
        log_debug(TAG,"ignoring...");
    }
}

void on_discovery_state_changed(Adapter *adapter, DiscoveryState state, const GError *error) {
    if (error != NULL) {
        log_debug(TAG, "discovery error (error %d: %s)", error->code, error->message);
        return;
    }
    log_debug(TAG, "discovery '%s' (%s)", binc_adapter_get_discovery_state_name(adapter),
              binc_adapter_get_path(adapter));
}

void start_scanning(Adapter *adapter) {
    // Build UUID array so we can use it in the discovery filter
    GPtrArray *service_uuids = g_ptr_array_new();
    g_ptr_array_add(service_uuids, HTS_SERVICE_UUID);

    // Set discovery callbacks and start discovery
    log_info(TAG, "start_scanning");

    GList *result = binc_adapter_get_connected_devices(adapter);
    for (GList *iterator = result; iterator; iterator = iterator->next) {
        Device *device = (Device *) iterator->data;
        log_debug(TAG, "start_scanning connected %s", binc_device_get_name(device)); 
        binc_device_disconnect(device);
    }
    g_list_free(result);

    binc_adapter_set_discovery_cb(adapter, &on_scan_result);
    binc_adapter_set_discovery_state_cb(adapter, &on_discovery_state_changed);
    // binc_adapter_set_discovery_filter(adapter, -100, service_uuids, NULL);
    g_ptr_array_free(service_uuids, TRUE);

    binc_adapter_start_discovery(default_adapter);
}

void on_powered_state_changed(Adapter *adapter, gboolean state) {
    log_debug(TAG, "powered '%s' (%s)", state ? "on" : "off", binc_adapter_get_path(adapter));
    if (state) {
        start_scanning(default_adapter);
    }
}

gboolean callback(gpointer data) {
    if (tries < 10) {
        tries++;
        return TRUE;
    }
    if (agent != NULL) {
        binc_agent_free(agent);
        agent = NULL;
    }

    if (default_device != NULL) {
        binc_device_disconnect(default_device);
        default_device = NULL;
    }

    if (default_adapter != NULL) {
        binc_adapter_free(default_adapter);
        default_adapter = NULL;
    }

    g_main_loop_quit((GMainLoop *) data);
    return FALSE;
}

static void cleanup_handler(int signo) {
    if (signo == SIGINT) {
        log_error(TAG, "received SIGINT");
        tries = 10;
        callback(loop);
    }
}

int main(void) {
    log_enabled(TRUE);
    log_set_level(LOG_DEBUG);

    // Get a DBus connection
    GDBusConnection *dbusConnection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);

    // Setup signal handler
    if (signal(SIGINT, cleanup_handler) == SIG_ERR)
        log_error(TAG, "can't catch SIGINT");

    // Setup mainloop
    loop = g_main_loop_new(NULL, FALSE);

    // Get the default adapter
    default_adapter = binc_adapter_get_default(dbusConnection);

    if (default_adapter != NULL) {
        log_info(TAG, "using adapter '%s'", binc_adapter_get_name(default_adapter));

        // Register an agent and set callbacks
        agent = binc_agent_create(default_adapter, "/org/bluez/BincAgent", KEYBOARD_DISPLAY);
        binc_agent_set_request_authorization_cb(agent, &on_request_authorization);
        binc_agent_set_request_passkey_cb(agent, &on_request_passkey);

        // Make sure the adapter is on before starting scanning
        binc_adapter_set_powered_state_cb(default_adapter, &on_powered_state_changed);
        if (!binc_adapter_get_powered_state(default_adapter)) {
            binc_adapter_power_on(default_adapter);
        } else {
            log_info(TAG, "using adapter start_scanning");
            start_scanning(default_adapter);
        }
    } else {
        log_error("MAIN", "No default_adapter found");
    }

    // Bail out after some time
    g_timeout_add_seconds(6, callback, loop);

    // Start the mainloop
    g_main_loop_run(loop);

    // Disconnect from DBus
    g_dbus_connection_close_sync(dbusConnection, NULL, NULL);
    g_object_unref(dbusConnection);

    // Clean up mainloop
    g_main_loop_unref(loop);
    if (done != IS_DONE) {
        log_error("MAIN", "Not all the value were discovered\n");
        exit(1);
    }
    return 0;
}
