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

/* tests/test_app_store.py includes this first in each file: stat() of a path
 * ending in stat_fault_path fails with EIO, as a flash error would. */
#pragma once
#include <sys/stat.h>

extern char stat_fault_path[128];
int stat_fault(const char *path, struct stat *st);
#define stat(path, st) stat_fault(path, st)
