#include <cassert>
#include <cstdlib>
#include <cstring>
#include "cJSON.h"
#include "noise_result.inc"

// Exercise the actual production transport wrapper, not a fake serializer.
extern "C" void check_wire_reply(cJSON *result) {
    cJSON *copy = cJSON_Duplicate(result, true);
    char *wire = wrap_result_json("test-id", copy);
    assert(wire);
    cJSON *decoded = cJSON_Parse(wire);
    assert(decoded);
    assert(strcmp(cJSON_GetObjectItem(decoded, "method")->valuestring, "link.result") == 0);
    cJSON *expected = cJSON_GetObjectItem(result, "payload");
    cJSON *actual = cJSON_GetObjectItem(decoded, "payload");
    if (expected) {
        assert(actual);
        assert(cJSON_Compare(expected, actual, true));
    }
    cJSON_Delete(decoded);
    free(wire);
}
