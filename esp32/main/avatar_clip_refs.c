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
#include "avatar_clip_refs.h"
#include "cJSON.h"
#include <string.h>
bool avatar_clip_refs_parse(const char *json,size_t len,avatar_clip_refs_t *out) {
    if (!out || avatar_identity_parse(json,len,&out->identity)!=1) return false;
    cJSON *root=cJSON_ParseWithLength(json,len);
    cJSON *a=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root,"result"),"avatar");
    cJSON *v=cJSON_GetObjectItemCaseSensitive(a,"video_variants");
    static const char *const names[]={"idle","waking","working","making_something","waiting_for_subagents","milestone_level_up"};
    bool valid=cJSON_IsObject(v);
    memset(out->path,0,sizeof(out->path));
    /* Each clip is {"light": URL, "dark": URL} of Gadget API avatar URLs. */
    for (unsigned i=0;valid && i<MUSE_AVATAR_CLIPS;i++) {
        const cJSON *p=cJSON_GetObjectItemCaseSensitive(v,names[i]);
        if (!p || cJSON_IsNull(p)) continue;
        const char *url=cJSON_IsObject(p)?avatar_identity_pick_url(p,sizeof(out->path[i])):NULL;
        valid=url!=NULL;
        if (valid) strcpy(out->path[i],url);
    }
    cJSON_Delete(root); return valid && out->path[MUSE_CLIP_IDLE][0];
}
