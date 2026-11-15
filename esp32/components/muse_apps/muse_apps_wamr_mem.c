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

/*
 * WAMR's memory-map layer in apps builds (CMakeLists.txt builds this into
 * WAMR's library in place of its ESP-IDF espidf_memmap.c). WAMR maps an app's
 * linear memory through it rather than through the runtime's allocator, so
 * ESP-IDF's took it outside the app's budget: from PSRAM on the S3 (its
 * dual-bus build), and with heap_caps_malloc(MALLOC_CAP_8BIT), internal RAM
 * first, elsewhere. Here it's PSRAM on every target, counted with the rest of
 * WAMR's memory (muse_apps.c). Interpreter only: nothing maps code.
 */

#include "platform_api_vmcore.h"

#include "muse_apps_wamr_mem.h"

void *os_mmap(void *hint, size_t size, int prot, int flags, os_file_handle file)
{
    (void)hint;
    (void)flags;
    (void)file;
    return prot & MMAP_PROT_EXEC ? NULL : muse_apps_linear_map(size);
}

void *os_mremap(void *old_addr, size_t old_size, size_t new_size)
{
    return muse_apps_linear_remap(old_addr, old_size, new_size);
}

void os_munmap(void *addr, size_t size)
{
    (void)size;
    muse_apps_linear_unmap(addr);
}

int os_mprotect(void *addr, size_t size, int prot)
{
    (void)addr;
    (void)size;
    (void)prot;
    return 0;
}

void os_dcache_flush(void) {}

void os_icache_flush(void *start, size_t len)
{
    (void)start;
    (void)len;
}

#if WASM_MEM_DUAL_BUS_MIRROR != 0
void *os_get_dbus_mirror(void *ibus)
{
    return ibus;   /* no code is mapped: nothing has a mirror */
}
#endif
