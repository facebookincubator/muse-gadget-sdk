# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
"""Exercise real speaking controller and renderer without ESP-IDF or hardware."""
import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class SpeakingAnimation(unittest.TestCase):
    def test_interruptions_pauses_silent_captions_and_long_responses(self):
        source = r'''
#include <assert.h>
#include <math.h>
#include "muse_speaking.h"
static float hands(const muse_speaking_t *s) { return s->lift[0]+s->lift[1]+s->extend[0]+s->extend[1]; }
int main(void) {
    /* Every possible cutoff settles promptly; it never finishes a queued pair. */
    for (int cut=1;cut<180;cut++) {
        muse_speaking_t s={0};
        for(int i=0;i<cut;i++) muse_speaking_update(&s,true,.04f,i*.04f,.6f);
        for(int i=0;i<15;i++) muse_speaking_update(&s,false,.04f,i*.04f,0);
        assert(!s.active && hands(&s)<.01f);
    }
    muse_speaking_t s={0};
    bool right=false, left=false, steps=false, open=false, rest=false;
    unsigned previous=MUSE_SPEAK_REST, sequence[40]={0}, actions=0;
    for(int i=0;i<1500;i++) {
        muse_speaking_update(&s,true,.04f,i*.04f,.6f);
        right |= s.action==MUSE_SPEAK_RAISE_RIGHT && s.lift[1]>.7f;
        left |= s.action==MUSE_SPEAK_RAISE_LEFT && s.lift[0]>.7f;
        if(s.action==MUSE_SPEAK_RAISE_RIGHT && s.time>.3f) assert(s.side==1 && s.lift[0]<.1f);
        if(s.action==MUSE_SPEAK_RAISE_LEFT && s.time>.3f) assert(s.side==0 && s.lift[1]<.1f);
        steps |= s.action==MUSE_SPEAK_STEPS && s.inward[s.side]>.7f;
        open |= s.action==MUSE_SPEAK_OPEN && s.extend[0]>.7f && s.extend[1]>.7f;
        if(s.action!=MUSE_SPEAK_REST && s.action!=previous && actions<40) sequence[actions++]=s.action;
        previous=s.action;
        rest |= i>50 && s.action==MUSE_SPEAK_REST && hands(&s)<.05f;
        for(int j=0;j<2;j++) {
            assert(s.lift[j]>=0 && s.lift[j]<=1); assert(s.extend[j]>=0 && s.extend[j]<=1);
            assert(s.inward[j]>=0 && s.inward[j]<=1); assert(fabsf(s.wrist[j])<=1);
        }
    }
    assert(right && left && steps && open && rest && actions>=9);
    /* Left and right raises are separate actions with other actions between. */
    for(unsigned i=0;i<actions;i++) assert(sequence[i]==MUSE_SPEAK_RAISE_RIGHT+i%4);
    /* A real pause closes down motion; sustained silent caption pacing still
       gets gestures and articulation without requiring voice energy. */
    s=(muse_speaking_t){0};
    for(int i=0;i<15;i++) muse_speaking_update(&s,true,.04f,i*.04f,.7f);
    for(int i=15;i<45;i++) muse_speaking_update(&s,true,.04f,i*.04f,0);
    assert(hands(&s)<.01f && s.energy<.001f && s.mouth<.001f);
    unsigned resumed=0;
    for(int i=45;i<95;i++) {
        muse_speaking_update(&s,true,.04f,i*.04f,.7f);
        if(!resumed && s.action!=MUSE_SPEAK_REST) resumed=s.action;
    }
    assert(resumed==MUSE_SPEAK_STEPS);
    s=(muse_speaking_t){0};
    float peak=0, mouth_min=1, mouth_max=0, brow_max=0, wide_max=0, gaze_max=0;
    for(int i=0;i<250;i++) {
        muse_speaking_update(&s,true,.04f,i*.04f,0);
        if(hands(&s)>peak) peak=hands(&s);
        if(s.mouth<mouth_min) mouth_min=s.mouth;
        if(s.mouth>mouth_max) mouth_max=s.mouth;
        if(s.brow>brow_max) brow_max=s.brow;
        if(s.eye_wide>wide_max) wide_max=s.eye_wide;
        if(fabsf(s.gaze)>gaze_max) gaze_max=fabsf(s.gaze);
    }
    assert(peak>.7f && s.energy==0);
    assert(mouth_max>.5f && mouth_min<.05f && brow_max>.5f && wide_max>.4f && gaze_max>.4f);
    /* Brief replies must rotate their openers too. */
    s=(muse_speaking_t){0};
    for(unsigned reply=0;reply<8;reply++) {
        unsigned opener=0;
        for(int i=0;i<13;i++) {
            muse_speaking_update(&s,true,.04f,i*.04f,.6f);
            if(!opener && s.action!=MUSE_SPEAK_REST) opener=s.action;
        }
        assert(opener==MUSE_SPEAK_RAISE_RIGHT+reply%4);
        for(int i=0;i<15;i++) muse_speaking_update(&s,false,.04f,i*.04f,0);
    }
    /* Preview warm-up at an unchanged mode time must not consume the opener. */
    s=(muse_speaking_t){0};
    for(int i=0;i<40;i++) muse_speaking_update(&s,true,.04f,0,0);
    assert(s.action==MUSE_SPEAK_REST && hands(&s)==0 && s.mouth==0);
    return 0;
}
'''
        self.compile_run(source)

    def test_real_renderer_stays_in_bounds_across_repeated_state_changes(self):
        source = r'''
#include "muse_pixel.h"
static uint16_t pixels[64*64];
int main(void) {
    muse_pixel_set_size(64);
    for(int i=0;i<2000;i++) {
        float mt=(i%150)*.04f;
        muse_pose_t p={.t=i*.04f+1,.mode_t=mt,.mode=(i%150)<100 ? MUSE_MODE_SPEAKING : MUSE_MODE_LISTENING,
                      .level=(i%20)<12 ? .8f : 0};
        muse_pixel_render(&p);
        muse_pixel_scale(pixels,64,0,63,0,63);
    }
    return 0;
}
'''
        self.compile_run(source, renderer=True)

    def test_avatar_prompt_allows_the_default_avatars_includes(self):
        """avatar.py sends the default renderer as the file to start from."""
        prompt = (ROOT / 'tools/muse/avatar_prompt.md').read_text()
        rule = prompt.split('Include only', 1)[1].split('\n\n', 1)[0]
        allowed = set(re.findall(r'(<[\w./]+>|"[\w./]+")', rule))
        used = set(re.findall(r'^#include\s+(<[^>]+>|"[^"]+")', (ROOT / 'avatar/muse_pixel.c').read_text(), re.M))
        self.assertLessEqual(used, allowed)

    def compile_run(self, source, renderer=False):
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory)
            (p/'test.c').write_text(source)
            cmd=shlex.split(os.environ.get('CC','cc'))+['-O1','-g','-Wall','-Wextra','-Werror',
                 '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                 '-I',str(ROOT/'components/muse'),str(p/'test.c')]
            if renderer: cmd.append(str(ROOT/'avatar/muse_pixel.c'))
            subprocess.run(cmd+['-lm','-o',str(p/'test')],check=True,capture_output=True,text=True)
            result=subprocess.run([str(p/'test')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)


if __name__ == '__main__':
    unittest.main()
