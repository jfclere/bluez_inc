//
// Created by martijn on 12/9/21.
//

#ifndef TEST_CHARACTERISTIC_H
#define TEST_CHARACTERISTIC_H

#include <gio/gio.h>
#include "service.h"
//#include "device.h"

/*
 * GATT Characteristic Property bit field
 * Reference: Core SPEC 4.1 page 2183 (Table 3.5: Characteristic Properties
 * bit field) defines how the Characteristic Value can be used, or how the
 * characteristic descriptors (see Section 3.3.3 - page 2184) can be accessed.
 * In the core spec, regular properties are included in the characteristic
 * declaration, and the extended properties are defined as descriptor.
 */
#define GATT_CHR_PROP_BROADCAST                0x01
#define GATT_CHR_PROP_READ                    0x02
#define GATT_CHR_PROP_WRITE_WITHOUT_RESP    0x04
#define GATT_CHR_PROP_WRITE                    0x08
#define GATT_CHR_PROP_NOTIFY                0x10
#define GATT_CHR_PROP_INDICATE                0x20
#define GATT_CHR_PROP_AUTH                    0x40
#define GATT_CHR_PROP_EXT_PROP                0x80

typedef struct binc_characteristic Characteristic;
typedef struct sDevice Device;

typedef enum WriteType {
    WITH_RESPONSE = 0, WITHOUT_RESPONSE = 1
} WriteType;

typedef void (*NotifyingStateChangedCallback)(Characteristic *characteristic, GError *error);

typedef void (*OnNotifyCallback)(Characteristic *characteristic, GByteArray *byteArray);

typedef void (*OnReadCallback)(Characteristic *characteristic, GByteArray *byteArray, GError *error);

typedef void (*OnWriteCallback)(Characteristic *characteristic, GError *error);


/**
 * Create a characteristic
 *
 * @param connection dbus connection
 * @param path the path of the object on the dbus
 * @return the newly create characteristic. Free by calling binc_characteristic_free()
 */
Characteristic *binc_characteristic_create(Device *device, const char *path);

/**
 * Free a characteristic object
 * @param characteristic
 */
void binc_characteristic_free(Characteristic *characteristic);

void binc_characteristic_set_read_callback(Characteristic *characteristic, OnReadCallback callback);

void binc_characteristic_set_write_callback(Characteristic *characteristic, OnWriteCallback callback);

void binc_characteristic_set_notify_callback(Characteristic *characteristic, OnNotifyCallback callback);

void binc_characteristic_set_notifying_state_change_callback(Characteristic *characteristic,
                                                             NotifyingStateChangedCallback callback);

void binc_characteristic_read(Characteristic *characteristic);

void binc_characteristic_write(Characteristic *characteristic, GByteArray *byteArray, WriteType writeType);

void binc_characteristic_start_notify(Characteristic *characteristic);

void binc_characteristic_stop_notify(Characteristic *characteristic);

Device* binc_characteristic_get_device(Characteristic *characteristic);

const char* binc_characteristic_get_uuid(Characteristic *characteristic);

void binc_characteristic_set_uuid(Characteristic *characteristic, const char* uuid);

const char* binc_characteristic_get_service_uuid(Characteristic *characteristic);

void binc_characteristic_set_service_uuid(Characteristic *characteristic, const char* service_uuid);

const char* binc_characteristic_get_service_path(Characteristic *characteristic);

void binc_characteristic_set_service_path(Characteristic *characteristic, const char* service_path);

GList* binc_characteristic_get_flags(Characteristic *characteristic);

void binc_characteristic_set_flags(Characteristic *characteristic, GList* flags);

guint binc_characteristic_get_properties(Characteristic *characteristic);

void binc_characteristic_set_properties(Characteristic *characteristic, guint properties);

gboolean binc_characteristic_is_notifying(Characteristic *characteristic);

/**
 * Get a string representation of the characteristic
 * @param characteristic
 * @return string representation of characteristic, caller must free
 */
char *binc_characteristic_to_string(Characteristic *characteristic);

#endif //TEST_CHARACTERISTIC_H