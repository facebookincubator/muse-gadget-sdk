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
package com.musegadgets.test;

import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.util.Arrays;

/** report_capabilities parsing (GadgetLink.parseCaps): malformed reports are rejected and change nothing. */
public final class CapsTest {
    private static int failed;

    private static void check(boolean ok, String what) {
        if (!ok) {
            failed++;
            System.out.println("FAIL: " + what);
        }
    }

    private static byte[] b(int... v) {
        byte[] r = new byte[v.length];
        for (int i = 0; i < v.length; i++) {
            r[i] = (byte) v[i];
        }
        return r;
    }

    public static void main(String[] args) {
        /* Every fixed-width capability with its value cut short. */
        byte[][] cut = {
            b(0x84, 0x01, 0x01, 0x04),
            b(0x84, 0x01, 0x01, 0x01),
            b(0x84, 0x01, 0x01, 0x01, 0x02, 0x18),
            b(0x84, 0x01, 0x01, 0x02, 0xe8, 0x01, 0xe8, 0x01),
            b(0x84, 0x01, 0x01, 0x03, 0xf0, 0x00, 0xf0),
            b(0x84, 0x01, 0x01, 0x05, 0x00, 0x00, 0x01),
            b(0x84, 0x01, 0x01, 0x06, 0xe8, 0x00, 0xe8, 0x00, 0x18, 0xc2),
            b(0x84, 0x01, 0x01, 0x07, 0x80),
            b(0x84, 0x01, 0x01, 0x08, 0x10),
            b(0x84, 0x01, 0x01, 0x09),
            b(0x84, 0x01, 0x01, 0x0a),
            b(0x84, 0x01, 0x01, 0x0b, 0xd0, 0x01, 0xd0, 0x01, 0x0f, 0x00, 0x40, 0x00, 0x00),
            b(0x84, 0x01, 0x01, 0x0c),
        };
        for (byte[] v : cut) {
            check(GadgetLink.parseCaps(v, new VideoLink.Caps()) == GadgetLink.CAPS_MALFORMED, "short report accepted: " + Arrays.toString(v));
        }

        /* A complete ui capability, then a max_fps without its value: the
         * link's caps keep their earlier values (huge sizes never land). */
        VideoLink.Caps link = new VideoLink.Caps();
        link.uiW = link.uiH = 400;
        link.maxFps = 20;
        byte[] bad = b(0x84, 0x01, 0x02, 0x0b, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x00, 0x00, 0x01, 0x00, 0x01, 0x04);
        check(GadgetLink.applyCapsReport(bad, link, 0, 0) == GadgetLink.CAPS_MALFORMED, "truncated ui+max_fps accepted");
        check(link.uiW == 400 && link.uiH == 400 && link.maxFps == 20 && !link.video, "link caps changed by a rejected report");

        /* A good report is clamped before the link's caps see it, with the bench overrides. */
        byte[] big = b(0x84, 0x01, 0x02, 0x0b, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x00, 0x00, 0x01, 0x00, 0x01,
                0x05, 0x00, 0x00, 0x01, 0x00);
        check(GadgetLink.applyCapsReport(big, link, 8192, 10) == GadgetLink.CAPS_NO_PSM, "big report rejected");
        check(link.uiW == 720 && link.uiH == 720 && link.buffer == 8192 && link.uiMaxFrame == 4096 && link.maxFps == 10
                && link.video, "big report not clamped: " + link.uiW + "x" + link.uiH + " buffer " + link.buffer);

        /* Fields a report doesn't carry keep their values, except codecs, ui and ui_still. */
        VideoLink.Caps kept = new VideoLink.Caps();
        kept.vp8 = true;
        kept.uiW = kept.uiH = 400;
        kept.uiStill = 2;
        kept.maxFps = 20;
        kept.buffer = 8192;
        check(GadgetLink.parseCaps(b(0x84, 0x01, 0x01, 0x03, 0x40, 0x01, 0x40, 0x01), kept) == GadgetLink.CAPS_NO_PSM, "max_size only");
        check(!kept.vp8 && kept.uiW == 0 && kept.uiH == 0 && kept.uiStill == 0, "codecs/ui/ui_still not cleared");
        check(kept.maxFps == 20 && kept.buffer == 8192 && kept.maxW == 320 && kept.maxH == 320, "fields not kept");

        /* A well-formed report parses, and clamp keeps it in range. */
        VideoLink.Caps c = new VideoLink.Caps();
        int psm = GadgetLink.parseCaps(b(0x84, 0x01, 0x05,
                0x01, 0x02, 0x18, 0x19,
                0x03, 0xf0, 0x00, 0xf0, 0x00,
                0x07, 0x80, 0x00,
                0x0b, 0xff, 0xff, 0xd0, 0x01, 0x0f, 0x00, 0x40, 0x00, 0x00, 0x02,
                0x04, 0x1e), c);
        check(psm == 0x80, "psm " + psm);
        check(c.vp8, "vp8");
        check(c.maxW == 240 && c.maxH == 240, "max size");
        check(c.maxFps == 30, "max fps");
        c.clamp();
        check(c.uiW == 720 && c.uiH == 464 && c.uiBands == 2, "ui clamped: " + c.uiW + "x" + c.uiH);

        /* Fewer entries than counted, or no count: malformed. An unknown id up to 16 ends the report
         * there; from 17 on it carries a length and is skipped. */
        check(GadgetLink.parseCaps(b(0x84, 0x01, 0x03, 0x04, 0x1e), new VideoLink.Caps()) == GadgetLink.CAPS_MALFORMED, "short count accepted");
        VideoLink.Caps u = new VideoLink.Caps();
        check(GadgetLink.parseCaps(b(0x84, 0x01, 0x03, 0x04, 0x1e, 0x10, 0x01, 0x09, 0x02), u) == GadgetLink.CAPS_NO_PSM
                && u.maxFps == 30 && u.maxTiles == 1, "unknown id 16 didn't end the report");
        VideoLink.Caps s17 = new VideoLink.Caps();
        check(GadgetLink.parseCaps(b(0x84, 0x01, 0x03, 0x04, 0x1e, 0x11, 0x02, 0xaa, 0xbb, 0x09, 0x02), s17) == GadgetLink.CAPS_NO_PSM
                && s17.maxFps == 30 && s17.maxTiles == 2, "id 17 not skipped by its length");
        check(GadgetLink.parseCaps(b(0x84, 0x01, 0x02, 0x04, 0x1e, 0x7f, 0x05, 0xaa), new VideoLink.Caps()) == GadgetLink.CAPS_MALFORMED,
                "id 127 past the end accepted");
        check(GadgetLink.parseCaps(b(0x84, 0x01, 0x02, 0x04, 0x1e, 0x7f), new VideoLink.Caps()) == GadgetLink.CAPS_MALFORMED,
                "id 127 without a length accepted");
        VideoLink.Caps z17 = new VideoLink.Caps();
        check(GadgetLink.parseCaps(b(0x84, 0x01, 0x03, 0x11, 0x00, 0x04, 0x1e, 0x09, 0x02), z17) == GadgetLink.CAPS_NO_PSM
                && z17.maxFps == 30 && z17.maxTiles == 2, "id 17 of length 0 not skipped");
        byte[] lng = new byte[7 + 0x90];
        System.arraycopy(b(0x84, 0x01, 0x02, 0x04, 0x1e, 0x20, 0x90), 0, lng, 0, 7);
        VideoLink.Caps l17 = new VideoLink.Caps();
        check(GadgetLink.parseCaps(lng, l17) == GadgetLink.CAPS_NO_PSM && l17.maxFps == 30,
                "id 32 of length 0x90, ending the report exactly, not skipped");
        check(GadgetLink.parseCaps(Arrays.copyOf(lng, lng.length - 1), new VideoLink.Caps()) == GadgetLink.CAPS_MALFORMED,
                "id 32 one byte short accepted");

        /* Error codes as mgcommands.h names them. */
        check("unsupported".equals(Mg.errorName(1)) && "invalid length".equals(Mg.errorName(2))
                && "invalid value".equals(Mg.errorName(3)) && "busy".equals(Mg.errorName(4))
                && "not found".equals(Mg.errorName(5)) && "proof required".equals(Mg.errorName(6))
                && "insecure".equals(Mg.errorName(7)) && "recovery".equals(Mg.errorName(8))
                && "code 0".equals(Mg.errorName(0)) && "code 99".equals(Mg.errorName(99)),
                "error names");
        VideoLink.Caps z = new VideoLink.Caps();
        check(GadgetLink.parseCaps(b(0x84, 0x01, 0x02, 0x01, 0x00, 0x04, 0x0f), z) == GadgetLink.CAPS_NO_PSM && !z.vp8 && z.maxFps == 15,
                "codecs with no entries");
        check(GadgetLink.parseCaps(b(0x84, 0x01), new VideoLink.Caps()) == GadgetLink.CAPS_MALFORMED, "report without a count accepted");
        check(GadgetLink.parseCaps(b(0x84, 0x01, 0x00), new VideoLink.Caps()) == GadgetLink.CAPS_NO_PSM, "empty report");
        check(GadgetLink.parseCaps(b(0x84, 0x01, 0x01, 0x07, 0x00, 0x00), new VideoLink.Caps()) == 0, "PSM 0 carried");
        VideoLink.Caps two = new VideoLink.Caps();
        GadgetLink.parseCaps(b(0x84, 0x01, 0x02, 0x01, 0x01, 0x19, 0x01, 0x01, 0x18), two);
        check(two.vp8, "VP8 in the first of two codecs entries lost");

        /* A reader's snapshot is clamped, whatever the link's caps hold mid-update (a hello arriving). */
        VideoLink.Caps raw = new VideoLink.Caps();
        raw.maxW = 1 << 30; raw.maxH = -5; raw.uiW = 1 << 30; raw.uiH = 1 << 30; raw.dispW = 15; raw.dispH = 1 << 30;
        raw.sugW = 1 << 30; raw.buffer = 1L << 50; raw.uiMaxFrame = 1 << 30; raw.maxFps = 0;
        VideoLink.Caps snap = raw.snapshot();
        check(snap.maxW == 720 && snap.maxH == 64 && snap.uiW == 720 && snap.uiH == 720 && snap.dispW == 0
                && snap.dispH == 2048 && snap.sugW == 720 && snap.buffer == 1 << 20 && snap.uiMaxFrame == 1 << 19
                && snap.maxFps == 1, "snapshot not clamped: " + snap);
        check(raw.maxW == 1 << 30, "snapshot changed the caps it copied");

        /* copy() carries every field. */
        try {
            VideoLink.Caps all = new VideoLink.Caps();
            int k = 1;
            for (Field f : VideoLink.Caps.class.getDeclaredFields()) {
                if (Modifier.isStatic(f.getModifiers())) {
                    continue;
                }
                f.setAccessible(true);
                if (f.getType() == int.class) {
                    f.setInt(all, 1000 + k++);
                } else if (f.getType() == long.class) {
                    f.setLong(all, 1000 + k++);
                } else if (f.getType() == boolean.class) {
                    f.setBoolean(all, !f.getBoolean(all));
                } else {
                    check(false, "Caps field of an unexpected type: " + f);
                }
            }
            VideoLink.Caps copy = all.copy();
            for (Field f : VideoLink.Caps.class.getDeclaredFields()) {
                if (!Modifier.isStatic(f.getModifiers())) {
                    check(f.get(copy).equals(f.get(all)), "copy() misses " + f.getName());
                }
            }
        } catch (IllegalAccessException e) {
            check(false, "reflection: " + e);
        }

        if (failed > 0) {
            System.out.println("CapsTest: " + failed + " failed");
            System.exit(1);
        }
        System.out.println("CapsTest: ok");
    }
}
