//
// Created by martijn on 12/9/21.
//

#ifndef TEST_SERVICE_H
#define TEST_SERVICE_H

#include <gio/gio.h>

typedef struct {
    const char *path;
    const char* uuid;
} Service;

Service* binc_service_create(const char* path, const char* uuid);
void binc_service_free(Service *service);
#endif //TEST_SERVICE_H