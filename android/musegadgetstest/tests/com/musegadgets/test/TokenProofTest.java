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

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import javax.crypto.spec.SecretKeySpec;

/** The token proof (TokenProof) replaying protocols/test-vectors/mg-token-proof-v1.json. */
public final class TokenProofTest {
    private static int failed;

    private static void check(boolean ok, String what) {
        if (!ok) {
            failed++;
            System.out.println("FAIL: " + what);
        }
    }

    private static byte[] hex(String s) {
        byte[] out = new byte[s.length() / 2];
        for (int i = 0; i < out.length; i++) {
            out[i] = (byte) Integer.parseInt(s.substring(2 * i, 2 * i + 2), 16);
        }
        return out;
    }

    /* Each vector's string fields, its frames' included (their names don't repeat within a vector). */
    private static List<Map<String, String>> vectors(String json) {
        List<Map<String, String>> out = new ArrayList<>();
        int at = json.indexOf("\"vectors\"");
        if (at < 0) {
            return out;
        }
        Matcher m = Pattern.compile("\"(\\w+)\"\\s*:\\s*\"([^\"]*)\"").matcher(json);
        m.region(at, json.length());
        Map<String, String> v = null;
        while (m.find()) {
            if (m.group(1).equals("name")) {
                v = new HashMap<>();
                out.add(v);
            }
            if (v != null) {
                v.put(m.group(1), m.group(2));
            }
        }
        return out;
    }

    public static void main(String[] args) throws IOException {
        String path = args.length > 0 ? args[0] : "";
        List<Map<String, String>> vs = path.isEmpty() ? new ArrayList<>()
                : vectors(new String(Files.readAllBytes(Paths.get(path)), StandardCharsets.UTF_8));
        check(!vs.isEmpty(), "no vectors at " + path);
        for (Map<String, String> v : vs) {
            String name = v.get("name");
            byte[] k = hex(v.get("K")), cn = hex(v.get("client_nonce")), dn = hex(v.get("device_nonce"));
            SecretKeySpec key = new SecretKeySpec(k, "HmacSHA256");
            check(Arrays.equals(TokenProof.mac(key, true, cn, dn), hex(v.get("device_mac"))), name + ": device MAC");
            check(Arrays.equals(TokenProof.mac(key, false, cn, dn), hex(v.get("client_mac"))), name + ": client MAC");
            check(Arrays.equals(TokenProof.parseKey(v.get("K")), k), name + ": parseKey");

            /* The exchange, frame for frame. */
            List<byte[]> sent = new ArrayList<>();
            TokenProof tp = new TokenProof(key, sent::add, () -> cn.clone());
            tp.start(1000);
            check(sent.size() == 1 && Arrays.equals(sent.get(0), hex(v.get("challenge"))), name + ": challenge frame");
            check(tp.onControl(hex(v.get("response")), 1100), name + ": response not taken");
            check(sent.size() == 2 && Arrays.equals(sent.get(1), hex(v.get("confirm"))), name + ": confirm frame");
            check(tp.pending(), name + ": done before the result");
            tp.onControl(hex(v.get("result")), 1200);
            check(tp.outcome() == TokenProof.Outcome.MATCH, name + ": " + tp.outcome());

            /* Another key: the gadget's MAC doesn't check, and nothing is confirmed. */
            byte[] other = k.clone();
            other[0] ^= 1;
            List<byte[]> sent2 = new ArrayList<>();
            TokenProof t2 = new TokenProof(new SecretKeySpec(other, "HmacSHA256"), sent2::add, () -> cn.clone());
            t2.start(1000);
            t2.onControl(hex(v.get("response")), 1100);
            check(t2.outcome() == TokenProof.Outcome.MISMATCH && sent2.size() == 1, name + ": another key");
        }

        if (failed > 0) {
            System.out.println("TokenProofTest: " + failed + " failed");
            System.exit(1);
        }
        System.out.println("TokenProofTest: ok (" + vs.size() + " vectors)");
    }
}
