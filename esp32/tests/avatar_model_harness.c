/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "avatar_identity.h"
#include "avatar_cache.h"
#include "vm_file_path.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int parse(const char *json, avatar_identity_t *id) { return avatar_identity_parse(json, strlen(json), id); }
static bool stop(void *ctx) { return --*(int *)ctx > 0; }
int main(int argc, char **argv) {
    assert(argc == 2);
    avatar_identity_t id = {0};
    const char *valid = "{\"ok\":true,\"result\":{\"name\":\"fixture\",\"avatar\":{\"image_exists\":true,\"image_path\":\"avatars/today.webp\",\"assets_revision\":\"r1\",\"future_field\":[1,2]}}}";
    assert(parse(valid, &id) == 1 && !strcmp(id.path, "avatars/today.webp") && !strcmp(id.revision,"r1"));
    avatar_identity_t original = id;
    assert(parse("{\"ok\":true,\"result\":{\"avatar\":{\"image_path\":\"x\"}}}", &id) == -1);
    assert(!memcmp(&id, &original, sizeof(id)));
    assert(parse("{\"ok\":false,\"result\":{\"avatar\":{\"image_exists\":false}}}",&id) == -1);
    assert(parse("{\"ok\":true,\"result\":{\"avatar\":{\"image_exists\":false}}}",&id) == 0 && !id.path[0]);
    assert(parse("{\"ok\":true,\"result\":{\"avatar\":{\"image_exists\":true,\"image_path\":\"a\\u0000b\"}}}",&id) == -1);
    assert(parse("{} trailing",&id) == -1);
    assert(avatar_identity_parse(valid, 8193, &id) == -1);
    char many[1024]; memset(many, '[', 13); memset(many+13, ']', 13); many[26]=0;
    assert(parse(many,&id) == -1);
    assert(parse(valid,&id) == 1);
    char *url = vm_file_request_path("/avatars/a b?x#y.webp");
    assert(!strcmp(url,"/fs/raw/avatars/a%20b%3Fx%23y.webp")); free(url);

    char path[2048], candidate[2048];
    snprintf(path,sizeof(path),"%s/a.dat",argv[1]);
    snprintf(candidate,sizeof(candidate),"%s/b.dat",argv[1]);
    uint8_t binding[32] = {1}, other[32] = {2};
    uint16_t pixels[256]; for (unsigned i=0;i<256;i++) pixels[i]=i*123;
    avatar_cache_header_t h, read;
    avatar_cache_header_init(&h,1,16,16,binding,&id,pixels);
    assert(avatar_cache_crc("123456789",9) == 0xcbf43926u);
    assert(avatar_cache_header_valid(&h,binding));
    assert(!avatar_cache_header_valid(&h,other));
    assert(avatar_cache_write(path,&h,pixels));
    uint16_t out[256];
    assert(avatar_cache_read(path,binding,&read,out,sizeof(out)) && !memcmp(out,pixels,sizeof(out)));
    assert(!avatar_cache_read(path,other,&read,out,sizeof(out)));
    assert(!avatar_cache_read(path,binding,&read,out,2));
    FILE *f=fopen(path,"r+b"); assert(f); assert(!fseek(f,sizeof(h)+4,SEEK_SET)); fputc(0xff,f); fclose(f);
    assert(!avatar_cache_read(path,binding,&read,out,sizeof(out)));
    assert(avatar_cache_write(path,&h,pixels));
    avatar_cache_header_t broken=h; broken.width=9999;
    assert(!avatar_cache_header_valid(&broken,binding));
    uint16_t *big=calloc(128*128,sizeof(uint16_t));assert(big);
    avatar_cache_header_t big_header;
    avatar_cache_header_init(&big_header,2,128,128,binding,&id,big);
    int remaining=3; /* header and first block succeed; stop before the second */
    assert(!avatar_cache_write_while(candidate,&big_header,big,stop,&remaining));
    free(big);
    assert(access(candidate,F_OK) != 0);
    assert(avatar_cache_read(path,binding,&read,out,sizeof(out)));
    char metadata[2048],temporary[2048];
    snprintf(metadata,sizeof(metadata),"%s/metadata.dat",argv[1]);
    snprintf(temporary,sizeof(temporary),"%s/metadata.tmp",argv[1]);
    avatar_identity_t updated=id;strcpy(updated.revision,"r2");
    avatar_cache_header_t update;
    avatar_cache_header_init(&update,1,16,16,binding,&updated,pixels);
    assert(avatar_cache_identity_write(metadata,temporary,&update));
    read=h;
    assert(avatar_cache_identity_read(metadata,&read) && !strcmp(read.identity.revision,"r2"));
    avatar_cache_header_init(&read,2,16,16,binding,&id,pixels);
    assert(!avatar_cache_identity_read(metadata,&read));
    assert(!strcmp(read.identity.revision,"r1"));
    assert(truncate(metadata,8)==0);
    read=h;assert(!avatar_cache_identity_read(metadata,&read));
    assert(!strcmp(read.identity.revision,"r1") && avatar_cache_read(path,binding,&read,out,sizeof(out)));
    assert(avatar_cache_write(candidate,&h,pixels));
    assert(truncate(candidate,sizeof(h)+8)==0);
    assert(!avatar_cache_read(candidate,binding,&read,out,sizeof(out)));
    assert(avatar_cache_read(path,binding,&read,out,sizeof(out)));
    puts("avatar model: schema, bounded JSON, URL encoding, cache integrity, pairing isolation, interrupted writes passed");
}
