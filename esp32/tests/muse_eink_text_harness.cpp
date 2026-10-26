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
#include <FreeInkUIDisplayTarget.h>
#include <assert.h>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "muse_eink_text.h"
using namespace freeink::ui;
using namespace muse_eink;
static DisplayTarget *s_target;
int text_width(FontId, const char *, int = 0);
void draw_text(int, int, FontId, const char *, Color, int = 0);
#define strlcat muse_test_strlcat
static size_t muse_test_strlcat(char *out, const char *src, size_t cap) {
    size_t n = strlen(out), len = strlen(src);
    if (n < cap) {
        size_t copy = len < cap-n-1 ? len : cap-n-1;
        memcpy(out+n,src,copy); out[n+copy] = 0;
    }
    return n+len;
}
#include "drawing_pipeline.inc"
#include "drawing_fit.inc"
static void check(const std::string &input) {
    std::unique_ptr<char[]> exact(new char[input.size()+1]);
    memcpy(exact.get(),input.c_str(),input.size()+1);
    std::string fixed = input;
    sanitize_utf8(fixed.data());
    assert(fixed.size() == input.size());
    size_t first_line = fixed.find('\n');
    std::string measured = fixed.substr(0, first_line);
    int expected = s_target->measureText(0,measured.c_str(),TextStyle{}).width;
    assert(text_width(0,exact.get()) == expected);
    draw_text(0,0,0,exact.get(),Color::Black);
    draw_text(0,0,0,exact.get(),Color::Black,1);
    draw_wrapped(0,0,128,0,exact.get(),3,0,false,Color::Black);
    assert(memcmp(exact.get(),input.c_str(),input.size()+1) == 0); // no credential/SSID storage mutation
    for (size_t cap = 1; cap < 16; cap++) {
        char out[32]; memset(out,0x5A,sizeof(out));
        fit(out,cap,exact.get(),0,64);
        assert(strlen(out) < cap && out[cap] == 0x5A);
        std::string validated = out;
        sanitize_utf8(validated.data());
        assert(validated == out);
        s_target->measureText(0,out,TextStyle{});
    }
}
int main(int argc, char **argv) {
    assert(argc == 2);
    uint8_t fb[256]{};
    DisplayTarget target(fb,64,32,8);
    s_target = &target;
    const std::string name = argv[1];
    if (name == "truncated") {
        for (const std::string &s : {std::string("\xF0"),std::string("ssid-\xF0"),std::string("\xF0\x90"),
                                   std::string("\xF0\x90\x80"),std::string("\xC2"),std::string("\xE0\xA0")}) check(s);
    } else if (name == "invalid") {
        for (const std::string &s : {std::string("\x80"),std::string("\xC0\x80"),std::string("\xE0\x80\x80"),
                                   std::string("\xED\xA0\x80"),std::string("\xF4\x90\x80\x80"),
                                   std::string("\xF5\x80\x80\x80"),std::string("\xF0" "ABC")}) check(s);
    } else if (name == "long") {
        check(std::string(500,static_cast<char>(0x80)));
        check(std::string(500,static_cast<char>(0xF0)));
    } else if (name == "valid") {
        for (const std::string &s : {std::string("ASCII"),std::string("éЖ中文😀"),std::string("\xF4\x8F\xBF\xBF")}) {
            std::string fixed = s;
            sanitize_utf8(fixed.data());
            assert(fixed == s);
            check(s);
        }
    } else if (name == "bytes") {
        for (int c = 1; c <= 255; c++) {
            for (int n = 0; n <= 3; n++) {
                std::string s(1,static_cast<char>(c));
                s.append(n,static_cast<char>(0x80));
                check(s);
            }
        }
    } else assert(false);
}
