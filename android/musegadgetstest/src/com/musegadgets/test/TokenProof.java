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

import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.Key;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.Arrays;
import java.util.function.Consumer;
import java.util.function.Supplier;

import javax.crypto.Mac;

/**
 * The token proof from the client's side (protocols/mgcommands.h, Token
 * proof): challenge, check the gadget's MAC, confirm, result. A match says
 * the gadget holds the proof key K this phone has for it. busy answers
 * restart the exchange about 200 ms later; 5 s of silence, or 20 s in all,
 * is a timeout. Plain Java (no Android API). Not thread-safe: the caller
 * serializes start, onControl and tick.
 */
final class TokenProof {
    enum Outcome {
        MATCH,
        MISMATCH,     /* the gadget's MAC is wrong, or it said 0: another key */
        NOT_FOUND,    /* the gadget holds no token */
        REFUSED,      /* another error (e.g. recovery mode, unsupported): refusedCode() */
        RECOVERY,     /* the gadget declared a recovery mode: no proof with it */
        TIMEOUT,
    }

    static final int CMD = 34;
    static final int CHALLENGE = 0, RESPONSE = 1, CONFIRM = 2, RESULT = 3;
    static final int NONCE_LEN = 16, MAC_LEN = 32;
    static final int ERROR_CMD = 25, ERROR_BUSY = 4, ERROR_NOT_FOUND = 5;
    static final long BUSY_RETRY_MS = 200, SILENCE_MS = 5000, TOTAL_MS = 20000;
    private static final byte[] DEVICE_LABEL = "mg token proof v1 device".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] CLIENT_LABEL = "mg token proof v1 client".getBytes(StandardCharsets.US_ASCII);
    private static final SecureRandom RANDOM = new SecureRandom();

    private final Key key;
    private final Consumer<byte[]> send;
    private final Supplier<byte[]> nonce;
    private byte[] clientNonce;
    private boolean started, awaitingResult;
    private long firstMs, lastMs, retryAtMs = -1;
    private Outcome outcome;
    private int refusedCode = -1;

    /**
     * key: K as an HMAC-SHA256 key (a SecretKeySpec, or a key the platform
     * keeps). send: one Control message. nonce: a fresh client nonce.
     */
    TokenProof(Key key, Consumer<byte[]> send, Supplier<byte[]> nonce) {
        this.key = key;
        this.send = send;
        this.nonce = nonce;
    }

    TokenProof(Key key, Consumer<byte[]> send) {
        this(key, send, TokenProof::randomNonce);
    }

    static byte[] randomNonce() {
        byte[] n = new byte[NONCE_LEN];
        RANDOM.nextBytes(n);
        return n;
    }

    /** HMAC-SHA256(K, "mg token proof v1 device|client" || client_nonce || device_nonce). */
    static byte[] mac(Key key, boolean device, byte[] cn, byte[] dn) {
        try {
            Mac m = Mac.getInstance("HmacSHA256");
            m.init(key);
            m.update(device ? DEVICE_LABEL : CLIENT_LABEL);
            m.update(cn);
            m.update(dn);
            return m.doFinal();
        } catch (GeneralSecurityException e) {
            throw new IllegalStateException("HMAC-SHA256 with the proof key", e);
        }
    }

    Outcome outcome() {
        return outcome;
    }

    /** The error code of a REFUSED outcome, -1 if none. */
    int refusedCode() {
        return refusedCode;
    }

    boolean pending() {
        return started && outcome == null;
    }

    void start(long nowMs) {
        started = true;
        firstMs = nowMs;
        sendChallenge(nowMs);
    }

    private void sendChallenge(long nowMs) {
        clientNonce = nonce.get();
        awaitingResult = false;
        retryAtMs = -1;
        lastMs = nowMs;
        byte[] m = new byte[2 + NONCE_LEN];
        m[0] = (byte) CMD;
        m[1] = (byte) CHALLENGE;
        System.arraycopy(clientNonce, 0, m, 2, NONCE_LEN);
        send.accept(m);
    }

    /** A Control notification: true when it was the proof's (token_proof, or an error for it). */
    boolean onControl(byte[] v, long nowMs) {
        if (!pending() || v.length == 0) {
            return false;
        }
        int c = v[0] & 0xff;
        if (c == ERROR_CMD && v.length > 1 && (v[1] & 0xff) == CMD) {
            int code = errorCode(v);
            if (code == ERROR_BUSY) {
                lastMs = nowMs;
                retryAtMs = nowMs + BUSY_RETRY_MS;
            } else if (code == ERROR_NOT_FOUND) {
                outcome = Outcome.NOT_FOUND;
            } else {
                refusedCode = code;
                outcome = Outcome.REFUSED;
            }
            return true;
        }
        if (c != CMD || v.length < 2) {
            return false;
        }
        int sub = v[1] & 0xff;
        if (sub == RESPONSE && !awaitingResult && clientNonce != null) {
            if (v.length < 2 + NONCE_LEN + MAC_LEN) {
                outcome = Outcome.MISMATCH;
                return true;
            }
            byte[] dn = Arrays.copyOfRange(v, 2, 2 + NONCE_LEN);
            byte[] got = Arrays.copyOfRange(v, 2 + NONCE_LEN, 2 + NONCE_LEN + MAC_LEN);
            if (!MessageDigest.isEqual(got, mac(key, true, clientNonce, dn))) {
                outcome = Outcome.MISMATCH;   /* the gadget doesn't hold this key: no confirm */
                return true;
            }
            awaitingResult = true;
            retryAtMs = -1;
            lastMs = nowMs;
            byte[] cm = mac(key, false, clientNonce, dn);
            byte[] m = new byte[2 + MAC_LEN];
            m[0] = (byte) CMD;
            m[1] = (byte) CONFIRM;
            System.arraycopy(cm, 0, m, 2, MAC_LEN);
            send.accept(m);
        } else if (sub == RESULT && awaitingResult) {
            outcome = v.length >= 3 && v[2] == 1 ? Outcome.MATCH : Outcome.MISMATCH;
        }
        return true;
    }

    /** Call every 100 ms or so while pending: busy retries and timeouts. */
    void tick(long nowMs) {
        if (!pending()) {
            return;
        }
        if (nowMs - firstMs > TOTAL_MS || (retryAtMs < 0 && nowMs - lastMs > SILENCE_MS)) {
            outcome = Outcome.TIMEOUT;
        } else if (retryAtMs >= 0 && nowMs >= retryAtMs) {
            sendChallenge(nowMs);
        }
    }

    /** The code in an error's mg_error_data_t list ([error, command, (type, value)...]), -1 if none. */
    static int errorCode(byte[] v) {
        int i = 2;
        while (i < v.length) {
            switch (v[i] & 0xff) {
                case 1:   /* code: uint16 */
                    return i + 2 < v.length ? (v[i + 1] & 0xff) | (v[i + 2] & 0xff) << 8 : -1;
                case 2:   /* log: uint16 size, then the string */
                    if (i + 2 >= v.length) {
                        return -1;
                    }
                    i += 3 + ((v[i + 1] & 0xff) | (v[i + 2] & 0xff) << 8);
                    break;
                default:
                    return -1;
            }
        }
        return -1;
    }

    /** A 64-hex-digit key, as mg_ble_client keeps it (proof_key); null if it isn't one. */
    static byte[] parseKey(String s) {
        String h = s == null ? "" : s.trim();
        if (h.length() != 64) {
            return null;
        }
        byte[] out = new byte[32];
        for (int i = 0; i < 32; i++) {
            int hi = hexDigit(h.charAt(2 * i)), lo = hexDigit(h.charAt(2 * i + 1));
            if (hi < 0 || lo < 0) {
                Arrays.fill(out, (byte) 0);
                return null;
            }
            out[i] = (byte) (hi << 4 | lo);
        }
        return out;
    }

    private static int hexDigit(char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    }

    /** The first 4 bytes of SHA-256(K), to tell keys apart in logs and the UI without showing one. */
    static String fingerprint(byte[] k) {
        try {
            byte[] d = MessageDigest.getInstance("SHA-256").digest(k);
            return String.format(java.util.Locale.US, "%02x%02x%02x%02x", d[0], d[1], d[2], d[3]);
        } catch (GeneralSecurityException e) {
            throw new IllegalStateException(e);
        }
    }
}
