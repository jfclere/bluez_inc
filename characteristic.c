//
// Created by martijn on 12/9/21.
//

#include <stdint-gcc.h>
#include "characteristic.h"
#include "logger.h"
#include "utility.h"
#include "device.h"

#define TAG "Characteristic"

struct binc_characteristic {
    Device *device;
    GDBusConnection *connection;
    const char *path;
    const char *uuid;
    const char *service_path;
    const char *service_uuid;
    gboolean notifying;
    GList *flags;
    guint properties;

    guint notify_signal;
    NotifyingStateChangedCallback notify_state_callback;
    OnReadCallback on_read_callback;
    OnWriteCallback on_write_callback;
    OnNotifyCallback on_notify_callback;
} ;

Characteristic *binc_characteristic_create(Device *device, const char *path) {
    Characteristic *characteristic = g_new0(Characteristic, 1);
    characteristic->device = device;
    characteristic->connection = device->connection;
    characteristic->path = path;
    characteristic->uuid = NULL;
    characteristic->service_path = NULL;
    characteristic->service_uuid = NULL;
    characteristic->on_write_callback = NULL;
    characteristic->on_read_callback = NULL;
    characteristic->on_notify_callback = NULL;
    characteristic->notify_state_callback = NULL;
    return characteristic;
}

static void binc_characteristic_free_flags(Characteristic *characteristic) {
    if (characteristic->flags != NULL) {
        if (g_list_length(characteristic->flags) > 0) {
            for (GList *iterator = characteristic->flags; iterator; iterator = iterator->next) {
                g_free((char *) iterator->data);
            }
        }
        g_list_free(characteristic->flags);
    }
}

void binc_characteristic_free(Characteristic *characteristic) {
    g_assert(characteristic != NULL);

    // Unsubscribe signal
    if (characteristic->notify_signal != 0) {
        g_dbus_connection_signal_unsubscribe(characteristic->connection, characteristic->notify_signal);
    }

    g_free((char *) characteristic->uuid);
    g_free((char *) characteristic->path);
    g_free((char *) characteristic->service_path);
    g_free((char *) characteristic->service_uuid);

    // Free flags
    binc_characteristic_free_flags(characteristic);

    g_free(characteristic);
}

char *binc_characteristic_to_string(Characteristic *characteristic) {
    g_assert(characteristic != NULL);

    // Build up flags
    GString *flags = g_string_new("[");
    if (g_list_length(characteristic->flags) > 0) {
        for (GList *iterator = characteristic->flags; iterator; iterator = iterator->next) {
            g_string_append_printf(flags, "%s, ", (char *) iterator->data);
        }
        g_string_truncate(flags, flags->len - 2);
    }
    g_string_append(flags, "]");

    char *result = g_strdup_printf(
            "Characteristic{uuid='%s', flags='%s', properties=%d, service_uuid='%s'}",
            characteristic->uuid,
            flags->str,
            characteristic->properties,
            characteristic->service_uuid);

    g_string_free(flags, TRUE);
    return result;
}

static GByteArray *g_variant_get_byte_array_for_read(GVariant *variant) {
    g_assert(variant != NULL);

    const gchar *type = g_variant_get_type_string(variant);
    if (!g_str_equal(type, "(ay)")) return NULL;

    GByteArray *byteArray = g_byte_array_new();
    uint8_t val;
    GVariantIter *iter;

    g_variant_get(variant, "(ay)", &iter);
    while (g_variant_iter_loop(iter, "y", &val)) {
        byteArray = g_byte_array_append(byteArray, &val, 1);
    }

    g_variant_iter_free(iter);
    return byteArray;
}

static void binc_internal_char_read_callback(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GError *error = NULL;
    Characteristic *characteristic = (Characteristic *) user_data;
    g_assert(characteristic != NULL);

    GVariant *value = g_dbus_connection_call_finish(characteristic->connection, res, &error);

    if (error != NULL) {
        log_debug(TAG, "failed to call '%s' (error %d: %s)", "ReadValue", error->code, error->message);
        g_clear_error(&error);
    }

    GByteArray *byteArray = NULL;
    if (value != NULL) {
        byteArray = g_variant_get_byte_array_for_read(value);
        if (characteristic->on_read_callback != NULL) {
            characteristic->on_read_callback(characteristic, byteArray, error);
        }
        g_byte_array_free(byteArray, TRUE);
        g_variant_unref(value);
    }
}

void binc_characteristic_read(Characteristic *characteristic) {
    g_assert(characteristic != NULL);
    g_assert((characteristic->properties & GATT_CHR_PROP_READ) > 0);

    log_debug(TAG, "reading <%s>", characteristic->uuid);

    guint16 offset = 0;
    GVariantBuilder *builder = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(builder, "{sv}", "offset", g_variant_new_uint16(offset));

    g_dbus_connection_call(characteristic->connection,
                           "org.bluez",
                           characteristic->path,
                           "org.bluez.GattCharacteristic1",
                           "ReadValue",
                           g_variant_new("(a{sv})", builder),
                           G_VARIANT_TYPE("(ay)"),
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           NULL,
                           (GAsyncReadyCallback) binc_internal_char_read_callback,
                           characteristic);
    g_variant_builder_unref(builder);
}

static void binc_internal_char_write_callback(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GError *error = NULL;
    Characteristic *characteristic = (Characteristic *) user_data;
    g_assert(characteristic != NULL);

    GVariant *value = g_dbus_connection_call_finish(characteristic->connection, res, &error);

    if (error != NULL) {
        log_debug(TAG, "failed to call '%s' (error %d: %s)", "WriteValue", error->code, error->message);
    }

    GByteArray *byteArray = NULL;
    if (value != NULL) {
        if (characteristic->on_write_callback != NULL) {
            characteristic->on_write_callback(characteristic, error);
        }
        g_variant_unref(value);
    }
}

void binc_characteristic_write(Characteristic *characteristic, GByteArray *byteArray, WriteType writeType) {
    g_assert(characteristic != NULL);
    g_assert(byteArray != NULL);

    if (writeType == WITH_RESPONSE) {
        g_assert((characteristic->properties & GATT_CHR_PROP_WRITE) > 0);
    } else {
        g_assert((characteristic->properties & GATT_CHR_PROP_WRITE_WITHOUT_RESP) > 0);
    }

    GString *byteArrayStr = g_byte_array_as_hex(byteArray);
    log_debug(TAG, "writing <%s> to <%s>", byteArrayStr->str, characteristic->uuid);
    g_string_free(byteArrayStr, TRUE);

    guint16 offset = 0;
    const char *writeTypeString = writeType == WITH_RESPONSE ? "request" : "command";

    // Convert byte array to variant
    GVariantBuilder *builder1 = g_variant_builder_new(G_VARIANT_TYPE("ay"));
    for (int i = 0; i < byteArray->len; i++) {
        g_variant_builder_add(builder1, "y", byteArray->data[i]);
    }
    GVariant *val = g_variant_new("ay", builder1);

    // Convert options to variant
    GVariantBuilder *builder2 = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(builder2, "{sv}", "offset", g_variant_new_uint16(offset));
    g_variant_builder_add(builder2, "{sv}", "type", g_variant_new_string(writeTypeString));
    GVariant *options = g_variant_new("a{sv}", builder2);


    g_dbus_connection_call(characteristic->connection,
                           "org.bluez",
                           characteristic->path,
                           "org.bluez.GattCharacteristic1",
                           "WriteValue",
                           g_variant_new("(@ay@a{sv})", val, options),
                           NULL,
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           NULL,
                           (GAsyncReadyCallback) binc_internal_char_write_callback,
                           characteristic);

    g_variant_builder_unref(builder1);
    g_variant_builder_unref(builder2);
}

static GByteArray *g_variant_get_byte_array_for_notify(GVariant *variant) {
    g_assert(variant != NULL);

    const gchar *type = g_variant_get_type_string(variant);
    if (!g_str_equal(type, "ay")) return NULL;

    GByteArray *byteArray = g_byte_array_new();
    uint8_t val;
    GVariantIter *iter;

    g_variant_get(variant, "ay", &iter);
    while (g_variant_iter_loop(iter, "y", &val)) {
        byteArray = g_byte_array_append(byteArray, &val, 1);
    }

    g_variant_iter_free(iter);
    return byteArray;
}

static void binc_signal_characteristic_changed(GDBusConnection *conn,
                                               const gchar *sender,
                                               const gchar *path,
                                               const gchar *interface,
                                               const gchar *signal,
                                               GVariant *params,
                                               void *userdata) {

    Characteristic *characteristic = (Characteristic *) userdata;
    g_assert(characteristic != NULL);

    GVariantIter *properties = NULL;
    GVariantIter *unknown = NULL;
    const char *iface;
    const char *key;
    GVariant *value = NULL;

    const gchar *signature = g_variant_get_type_string(params);
    if (!g_str_equal(signature, "(sa{sv}as)")) {
        log_debug("Invalid signature for %s: %s != %s", signal, signature, "(sa{sv}as)");
        return;
    }

    g_variant_get(params, "(&sa{sv}as)", &iface, &properties, &unknown);
    while (g_variant_iter_next(properties, "{&sv}", &key, &value)) {
        if (g_str_equal(key, "Notifying")) {
            characteristic->notifying = g_variant_get_boolean(value);
            log_debug(TAG, "notifying %s <%s>", characteristic->notifying ? "true" : "false", characteristic->uuid);
            if (characteristic->notify_state_callback != NULL) {
                characteristic->notify_state_callback(characteristic, NULL);
            }
            if (characteristic->notifying == FALSE) {
                if (characteristic->notify_signal != 0) {
                    g_dbus_connection_signal_unsubscribe(characteristic->connection, characteristic->notify_signal);
                }
            }
        } else if (g_str_equal(key, "Value")) {
            GByteArray *byteArray = g_variant_get_byte_array_for_notify(value);
            GString *result = g_byte_array_as_hex(byteArray);
            log_debug(TAG, "notification <%s> on <%s>", result->str, characteristic->uuid);
            g_string_free(result, TRUE);
            if (characteristic->on_notify_callback != NULL) {
                characteristic->on_notify_callback(characteristic, byteArray);
                g_byte_array_free(byteArray, TRUE);
            }
        }
    }

    if (properties != NULL)
        g_variant_iter_free(properties);
    if (value != NULL)
        g_variant_unref(value);
}

static void binc_internal_char_start_notify(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    Characteristic *characteristic = (Characteristic *) user_data;
    g_assert(characteristic != NULL);
    g_assert((characteristic->properties & GATT_CHR_PROP_INDICATE) > 0 ||
             (characteristic->properties & GATT_CHR_PROP_NOTIFY) > 0);

    GError *error = NULL;
    GVariant *value = g_dbus_connection_call_finish(characteristic->connection, res, &error);

    if (error != NULL) {
        log_debug(TAG, "failed to call '%s' (error %d: %s)", "StartNotify", error->code, error->message);
        if (characteristic->notify_state_callback != NULL) {
            characteristic->notify_state_callback(characteristic, error);
        }
        g_clear_error(&error);
    }

    if (value != NULL) {
        g_variant_unref(value);
    }
}

void binc_characteristic_start_notify(Characteristic *characteristic) {
    g_assert(characteristic != NULL);
    g_assert((characteristic->properties & GATT_CHR_PROP_INDICATE) > 0 ||
             (characteristic->properties & GATT_CHR_PROP_NOTIFY) > 0);

    characteristic->notify_signal = g_dbus_connection_signal_subscribe(characteristic->connection,
                                                                       "org.bluez",
                                                                       "org.freedesktop.DBus.Properties",
                                                                       "PropertiesChanged",
                                                                       characteristic->path,
                                                                       "org.bluez.GattCharacteristic1",
                                                                       G_DBUS_SIGNAL_FLAGS_NONE,
                                                                       binc_signal_characteristic_changed,
                                                                       characteristic,
                                                                       NULL);


    g_dbus_connection_call(characteristic->connection,
                           "org.bluez",
                           characteristic->path,
                           "org.bluez.GattCharacteristic1",
                           "StartNotify",
                           NULL,
                           NULL,
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           NULL,
                           (GAsyncReadyCallback) binc_internal_char_start_notify,
                           characteristic);
}

static void binc_internal_char_stop_notify(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    Characteristic *characteristic = (Characteristic *) user_data;
    g_assert(characteristic != NULL);

    GError *error = NULL;
    GVariant *value = g_dbus_connection_call_finish(characteristic->connection, res, &error);

    if (error != NULL) {
        log_debug(TAG, "failed to call '%s' (error %d: %s)", "StopNotify", error->code, error->message);
        if (characteristic->notify_state_callback != NULL) {
            characteristic->notify_state_callback(characteristic, error);
        }
        g_clear_error(&error);
    }

    if (value != NULL) {
        g_variant_unref(value);
    }
}

void binc_characteristic_stop_notify(Characteristic *characteristic) {
    g_assert(characteristic != NULL);
    g_assert((characteristic->properties & GATT_CHR_PROP_INDICATE) > 0 ||
             (characteristic->properties & GATT_CHR_PROP_NOTIFY) > 0);

    g_dbus_connection_call(characteristic->connection,
                           "org.bluez",
                           characteristic->path,
                           "org.bluez.GattCharacteristic1",
                           "StopNotify",
                           NULL,
                           NULL,
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           NULL,
                           (GAsyncReadyCallback) binc_internal_char_stop_notify,
                           characteristic);
}

void binc_characteristic_set_read_callback(Characteristic *characteristic, OnReadCallback callback) {
    g_assert(characteristic != NULL);
    g_assert(callback != NULL);
    characteristic->on_read_callback = callback;
}

void binc_characteristic_set_write_callback(Characteristic *characteristic, OnWriteCallback callback) {
    g_assert(characteristic != NULL);
    g_assert(callback != NULL);
    characteristic->on_write_callback = callback;
}

void binc_characteristic_set_notify_callback(Characteristic *characteristic, OnNotifyCallback callback) {
    g_assert(characteristic != NULL);
    g_assert(callback != NULL);
    characteristic->on_notify_callback = callback;
}

void binc_characteristic_set_notifying_state_change_callback(Characteristic *characteristic,
                                                             NotifyingStateChangedCallback callback) {
    g_assert(characteristic != NULL);
    g_assert(callback != NULL);

    characteristic->notify_state_callback = callback;
}

const char* binc_characteristic_get_uuid(Characteristic *characteristic) {
    g_assert(characteristic != NULL);
    return characteristic->uuid;
}

void binc_characteristic_set_uuid(Characteristic *characteristic, const char* uuid) {
    g_assert(characteristic != NULL);
    g_assert(uuid != NULL);

    if (characteristic->uuid != NULL) {
        g_free((char*) characteristic->uuid);
    }
    characteristic->uuid = g_strdup(uuid);
}

const char* binc_characteristic_get_service_uuid(Characteristic *characteristic) {
    g_assert(characteristic != NULL);
    return characteristic->service_uuid;
}

Device* binc_characteristic_get_device(Characteristic *characteristic) {
    g_assert(characteristic != NULL);
    return characteristic->device;
}

void binc_characteristic_set_service_uuid(Characteristic *characteristic, const char* service_uuid) {
    g_assert(characteristic != NULL);
    g_assert(service_uuid != NULL);

    if (characteristic->service_uuid != NULL) {
        g_free((char*) characteristic->service_uuid);
    }
    characteristic->service_uuid = g_strdup(service_uuid);
}

const char* binc_characteristic_get_service_path(Characteristic *characteristic) {
    g_assert(characteristic != NULL);
    return characteristic->service_path;
}

void binc_characteristic_set_service_path(Characteristic *characteristic, const char* service_path) {
    g_assert(characteristic != NULL);
    g_assert(service_path != NULL);

    if (characteristic->service_path != NULL) {
        g_free((char*) characteristic->service_path);
    }
    characteristic->service_path = g_strdup(service_path);
}

GList* binc_characteristic_get_flags(Characteristic *characteristic) {
    g_assert(characteristic != NULL);
    return characteristic->flags;
}

void binc_characteristic_set_flags(Characteristic *characteristic, GList* flags) {
    g_assert(characteristic != NULL);
    g_assert(flags != NULL);

    if (characteristic->flags != NULL) {
        binc_characteristic_free_flags(characteristic);
    }
    characteristic->flags = flags;
}

guint binc_characteristic_get_properties(Characteristic *characteristic) {
    g_assert(characteristic != NULL);
    return characteristic->properties;
}

void binc_characteristic_set_properties(Characteristic *characteristic, guint properties) {
    g_assert(characteristic != NULL);
    characteristic->properties = properties;
}

gboolean binc_characteristic_is_notifying(Characteristic *characteristic) {
    g_assert(characteristic != NULL);
    return characteristic->notifying;
}

