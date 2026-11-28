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

import android.content.Context;
import android.content.SharedPreferences;
import android.security.keystore.KeyProperties;
import android.security.keystore.KeyProtection;
import android.util.Log;

import java.security.KeyStore;
import java.util.Arrays;

import javax.crypto.SecretKey;
import javax.crypto.spec.SecretKeySpec;

/**
 * Token proof keys (TokenProof), one per gadget by its advertised name, as
 * mg_ble_client's setup keeps them (proof_key). Each is imported into the
 * Android Keystore as a non-exportable HMAC-SHA256 key: the app computes
 * MACs with it but can't read it back, and it never leaves this phone (no
 * backup). Only its fingerprint (TokenProof.fingerprint) is kept beside it,
 * for the UI and the log.
 */
final class ProofKeys {
    private static final String TAG = VideoLink.TAG;
    private static final String STORE = "AndroidKeyStore", PREFIX = "mgproof:";

    private ProofKeys() {}

    private static SharedPreferences fingerprints(Context ctx) {
        return ctx.getSharedPreferences("musegadgetstest_proof", Context.MODE_PRIVATE);
    }

    private static KeyStore store() throws Exception {
        KeyStore ks = KeyStore.getInstance(STORE);
        ks.load(null);
        return ks;
    }

    /** The key for this gadget, null if there's none (or the Keystore failed). */
    static SecretKey get(String gadget) {
        if (gadget == null || gadget.isEmpty()) {
            return null;
        }
        try {
            java.security.Key k = store().getKey(PREFIX + gadget, null);
            return k instanceof SecretKey ? (SecretKey) k : null;
        } catch (Exception e) {
            Log.w(TAG, "proof key: Keystore read failed for " + gadget + ": " + e.getClass().getSimpleName());
            return null;
        }
    }

    /** The key's fingerprint for this gadget, null if there's no key. */
    static String fingerprint(Context ctx, String gadget) {
        if (gadget == null || gadget.isEmpty()) {
            return null;
        }
        try {
            return store().containsAlias(PREFIX + gadget) ? fingerprints(ctx).getString(gadget, "?") : null;
        } catch (Exception e) {
            return null;
        }
    }

    /**
     * Sets this gadget's key from 64 hex digits ("" removes it). False: not a
     * key, or the Keystore refused it (the log says which).
     */
    static boolean set(Context ctx, String gadget, String hex) {
        if (gadget == null || gadget.isEmpty()) {
            return false;
        }
        if (hex == null || hex.trim().isEmpty()) {
            try {
                store().deleteEntry(PREFIX + gadget);
            } catch (Exception e) {
                Log.w(TAG, "proof key: Keystore delete failed for " + gadget + ": " + e.getClass().getSimpleName());
                return false;
            }
            fingerprints(ctx).edit().remove(gadget).apply();
            Log.i(TAG, "proof key: removed for " + gadget);
            return true;
        }
        byte[] k = TokenProof.parseKey(hex);
        if (k == null) {
            Log.i(TAG, "proof key: not a key (64 hex digits) for " + gadget);
            return false;
        }
        String fp = TokenProof.fingerprint(k);
        try {
            store().setEntry(PREFIX + gadget, new KeyStore.SecretKeyEntry(new SecretKeySpec(k, KeyProperties.KEY_ALGORITHM_HMAC_SHA256)),
                    new KeyProtection.Builder(KeyProperties.PURPOSE_SIGN).build());
        } catch (Exception e) {
            Log.w(TAG, "proof key: Keystore import failed for " + gadget + ": " + e.getClass().getSimpleName());
            return false;
        } finally {
            Arrays.fill(k, (byte) 0);
        }
        fingerprints(ctx).edit().putString(gadget, fp).apply();
        Log.i(TAG, "proof key: " + fp + " for " + gadget);
        return true;
    }
}
