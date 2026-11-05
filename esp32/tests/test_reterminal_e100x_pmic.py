"""Run the actual SY6974B driver with real host threads and fault injection.

This verifies software ownership, not I2C electrical timing or charger safety.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class E100xPmicTest(unittest.TestCase):
    def test_concurrent_initialization_retry_and_register_sequences(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            headers = {
                "sdkconfig.h": "#define CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_IT8951 1\n",
                "esp_log.h": "#pragma once\n",
                "driver/i2c_master.h": """#pragma once
#include <stddef.h>
#include <stdint.h>
#define ESP_OK 0
#define I2C_ADDR_BIT_LEN_7 7
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct { int dev_addr_length, device_address, scl_speed_hz; } i2c_device_config_t;
int i2c_master_probe(void *,int,int);
int i2c_master_bus_add_device(void *,const i2c_device_config_t *,void **);
int i2c_master_bus_rm_device(void *);
int i2c_master_transmit_receive(void *,const uint8_t *,size_t,uint8_t *,size_t,int);
""",
                "freertos/FreeRTOS.h": """#pragma once
#include <pthread.h>
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(p) pthread_mutex_lock(p)
#define portEXIT_CRITICAL(p) pthread_mutex_unlock(p)
""",
                "freertos/task.h": "void vTaskDelay(int);\n",
                "freertos/semphr.h": """#pragma once
#include <pthread.h>
typedef pthread_mutex_t *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t,int);
void xSemaphoreGive(SemaphoreHandle_t);
""",
            }
            for name, data in headers.items():
                p = out / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(data)
            (out / "check.c").write_text(r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include "e1003_pmic.h"
#include "reterminal_e100x_board.h"
#include "freertos/semphr.h"
static atomic_int probes, adds, removed, fail_probe, fail_add, fail_alloc, fail_read;
static atomic_int takes, gives, deny_take;
static atomic_int fail_register = -1;
static unsigned char trace[10000];
static int trace_n;
void vTaskDelay(int ms) {
    struct timespec t = {0, ms * 1000000L}; nanosleep(&t, NULL);
}
void *reterminal_e100x_i2c_bus_get(void) { return (void *)1; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    if (atomic_exchange(&fail_alloc, 0)) return NULL;
    pthread_mutex_t *p = malloc(sizeof(*p)); assert(p);
    pthread_mutex_init(p, NULL); return p;
}
int xSemaphoreTake(SemaphoreHandle_t p, int timeout) {
    assert(p && timeout > 0); if(atomic_exchange(&deny_take,0)) return 0;
    pthread_mutex_lock(p); takes++; return 1;
}
void xSemaphoreGive(SemaphoreHandle_t p) { gives++; pthread_mutex_unlock(p); }
int i2c_master_probe(void *b,int addr,int timeout) {
    assert(b && addr == 0x6b && timeout == 100); probes++;
    vTaskDelay(5); return atomic_exchange(&fail_probe,0) ? -1 : 0;
}
int i2c_master_bus_add_device(void *b,const i2c_device_config_t *c,void **d) {
    assert(b && c->device_address == 0x6b); adds++;
    if (atomic_exchange(&fail_add,0)) return -1;
    *d = malloc(1); assert(*d); return 0;
}
int i2c_master_bus_rm_device(void *d) { removed++; free(d); return 0; }
int i2c_master_transmit_receive(void *d,const uint8_t *r,size_t n,uint8_t *v,size_t z,int t) {
    assert(d && n == 1 && z == 1 && t == 100);
    if (atomic_exchange(&fail_read,0)) return -1;
    if(*r == atomic_load(&fail_register)) { fail_register=-1;return -1; }
    assert(trace_n < (int)sizeof(trace)); trace[trace_n++] = *r;
    *v = *r == 8 ? 180 : *r == 10 ? 160 : 0;
    vTaskDelay(1); return 0;
}
static void *worker(void *arg) {
    for (int i=0;i<20;i++) {
        if ((size_t)arg & 1) {
            bool c,p; assert(e1003_pmic_read_power(&c,&p) && c && p);
        } else {
            e1003_pmic_status_t s; assert(e1003_pmic_read(&s) && s.charging && s.input_present);
        }
    }
    return NULL;
}
int main(void) {
    e1003_pmic_status_t s;
    fail_probe=1; assert(!e1003_pmic_read(&s));
    fail_add=1; assert(!e1003_pmic_read(&s));
    fail_alloc=1; assert(!e1003_pmic_read(&s) && removed == 1);
    pthread_t threads[8];
    for (size_t i=0;i<8;i++) assert(!pthread_create(&threads[i],NULL,worker,(void *)i));
    for (int i=0;i<8;i++) pthread_join(threads[i],NULL);
    assert(probes == 4 && adds == 3);
    for (int i=0;i<trace_n;) {
        assert(trace[i++] == 8);
        if (trace[i] == 9) { assert(trace[i++] == 9); assert(trace[i++] == 9); }
        assert(trace[i++] == 10);
    }
    fail_read=1; assert(!e1003_pmic_read(&s));
    assert(e1003_pmic_read(&s));
    int at=trace_n; bool c,p; assert(e1003_pmic_read_power(&c,&p));
    assert(trace_n == at+2 && trace[at] == 8 && trace[at+1] == 10);
    for(int r=8;r<=10;r++) {
        fail_register=r;assert(!e1003_pmic_read(&s));assert(e1003_pmic_read(&s));
    }
    deny_take=1;assert(!e1003_pmic_read(&s));
    deny_take=1;assert(!e1003_pmic_read_power(&c,&p));
    assert(e1003_pmic_read(&s));
    assert(takes == gives);
    return 0;
}
''')
            cc = shlex.split(os.environ.get("CC", "gcc"))
            exe = out / "pmic.exe"
            r = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                                "-I", str(out), "-I", str(ROOT / "main"),
                                str(out / "check.c"), str(ROOT / "main/e1003_pmic.c"),
                                "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout+r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(r.returncode, 0, r.stdout+r.stderr)
