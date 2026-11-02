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

// The host's addrinfo and sockaddr_in, with name lookups answered by the
// harness's scripted home network instead of real DNS.

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>

int lan_fake_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                         struct addrinfo **res);
void lan_fake_freeaddrinfo(struct addrinfo *res);

#define getaddrinfo lan_fake_getaddrinfo
#define freeaddrinfo lan_fake_freeaddrinfo
