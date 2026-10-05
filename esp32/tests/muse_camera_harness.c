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

// Host harness for the camera's pure parts: SSCMA frame splitting and reply
// names (components/muse/muse_camera.c), and resolutions, model names and
// base64 (main/muse_hw_camera.c). The runner extracts both "host-tested"
// sections into camera_frames.inc and camera_cmds.inc.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera_frames.inc"
#include "camera_cmds.inc"

static void test_frames(void) {
    size_t start, end, drop;
    const char *one = "noise\r{\"type\": 0, \"name\": \"ID?\"}\ntrail";
    assert(find_frame(one, strlen(one), &start, &end, &drop));
    assert(drop == 5 && one[start] == '{' && one[end - 1] == '}');
    assert(end - start == strlen("{\"type\": 0, \"name\": \"ID?\"}"));

    // Incomplete: keep from the marker on, drop the noise before it.
    const char *part = "xx\r{\"type\": 1, \"data\": {\"image\": \"/9j/4AA";
    assert(!find_frame(part, strlen(part), &start, &end, &drop) && drop == 2);

    // No marker: drop all but a trailing '\r' that may start one.
    assert(!find_frame("abc\r", 4, &start, &end, &drop) && drop == 3);
    assert(!find_frame("abcd", 4, &start, &end, &drop) && drop == 4);
    assert(!find_frame("", 0, &start, &end, &drop) && drop == 0);

    // A nested "}" isn't the end; the first "}\n" is.
    const char *nested = "\r{\"a\": {\"b\": 1}, \"c\": 2}\n\r{\"d\": 3}\n";
    assert(find_frame(nested, strlen(nested), &start, &end, &drop));
    assert(end == strlen("\r{\"a\": {\"b\": 1}, \"c\": 2}") && drop == 0);
    size_t off = end + 1;
    assert(find_frame(nested + off, strlen(nested) - off, &start, &end, &drop));
    assert(!strncmp(nested + off + start, "{\"d\": 3}", end - start));

    char name[24];
    command_name("MODEL=1", name, sizeof(name));
    assert(!strcmp(name, "MODEL"));
    command_name("ID?", name, sizeof(name));
    assert(!strcmp(name, "ID?"));
    command_name("INVOKE=1,0,1", name, sizeof(name));
    assert(!strcmp(name, "INVOKE"));
    command_name("A_VERY_LONG_COMMAND_NAME_THAT_OVERFLOWS=1", name, 8);
    assert(!strcmp(name, "A_VERY_"));
}

static void test_commands(void) {
    assert(resolution_opt("240x240") == 0 && resolution_opt("640x480") == 3);
    assert(resolution_opt("1920x1080") == -1 && resolution_opt("") == -1);
    assert(model_id("person") == 1 && model_id("pet") == 2 && model_id("gesture") == 3);
    assert(model_id("custom") == 4 && model_id("2") == 2 && model_id("5") == 0 && model_id("cat") == 0);

    uint8_t out[32];
    assert(b64_decode("Zm9vYmFy", 8, out) == 6 && !memcmp(out, "foobar", 6));
    assert(b64_decode("Zm9vYg==", 8, out) == 4 && !memcmp(out, "foob", 4));
    assert(b64_decode("//4AgH8=", 8, out) == 5 && out[0] == 0xff && out[1] == 0xfe && out[4] == 0x7f);
    assert(b64_decode("", 0, out) == 0);
}

int main(void) {
    test_frames();
    test_commands();
    printf("ok\n");
    return 0;
}
