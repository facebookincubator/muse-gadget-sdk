/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stddef.h>

typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;
    int type;
    int valueint;
    double valuedouble;
    char *valuestring;
    char *string;
} cJSON;

enum {
    cJSON_False = 1,
    cJSON_True = 2,
    cJSON_NULL = 4,
    cJSON_String = 16,
    cJSON_Array = 32,
    cJSON_Object = 64,
    cJSON_Number = 128,
};

#define cJSON_ArrayForEach(element, array) \
    for ((element) = ((array) ? (array)->child : NULL); (element); (element) = (element)->next)

cJSON *cJSON_ParseWithLength(const char *value, size_t buffer_length);
void cJSON_Delete(cJSON *item);

cJSON *cJSON_CreateObject(void);
cJSON *cJSON_CreateBool(int boolean);
cJSON *cJSON_CreateNumber(double number);
cJSON *cJSON_AddItemToObject(cJSON *object, const char *name, cJSON *item);
cJSON *cJSON_AddStringToObject(cJSON *object, const char *name, const char *string);
cJSON *cJSON_AddBoolToObject(cJSON *object, const char *name, int boolean);
cJSON *cJSON_AddNumberToObject(cJSON *object, const char *name, double number);
char *cJSON_PrintUnformatted(const cJSON *item);
int cJSON_PrintPreallocated(cJSON *item, char *buffer, const int length, const int format);
void cJSON_free(void *object);

int cJSON_IsArray(const cJSON *item);
int cJSON_IsFalse(const cJSON *item);
int cJSON_IsNull(const cJSON *item);
int cJSON_IsObject(const cJSON *item);
int cJSON_IsString(const cJSON *item);
int cJSON_IsTrue(const cJSON *item);
cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string);
