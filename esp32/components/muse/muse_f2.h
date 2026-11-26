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

/*
 * The flash-bus integrity check: a boot on
 * battery, at the clocks that boot runs at, reads the whole running app back
 * (its image SHA-256, three passes) and every NVS entry (their CRCs), and
 * keeps the verdict in NVS for the next USB session. Armed from the console
 * (">f2.arm"), it runs once, 5 s into the next boot on battery (a boot on
 * USB keeps it armed); ">f2.result" prints it.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* Arms the check; restart_s > 0 also restarts in that many seconds (1-120). */
void muse_f2_arm(int restart_s);
void muse_f2_print_result(void);
/* At start: runs the check if it was armed. */
void muse_f2_boot(void);

#ifdef __cplusplus
}
#endif
