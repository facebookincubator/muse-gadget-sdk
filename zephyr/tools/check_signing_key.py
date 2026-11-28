#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Refuses a build signed with a public MCUboot development key, unless it
says it's a development build.

  check_signing_key.py --key PEM --mcuboot MCUBOOT_DIR [--dev-ok] [--blocklist FILE]

A key is public when it lies in MCUboot's tree or has the same public key as
one of MCUboot's root-*.pem keys (a copy elsewhere, re-encoded or not, is
still public), or when its public key's sha256 (DER SubjectPublicKeyInfo) is
in the --blocklist file (one hex fingerprint per line, # comments). Keys are compared by their public key in DER, derived with the
cryptography package (imgtool's) or else the openssl command. Exit 1 with a
message for a public key without --dev-ok, a missing key or no key, and when
the comparison can't be made (no parser, no readable root keys, a key that
doesn't parse); exit 0 otherwise (a warning is printed for a public key with
--dev-ok). Never prints key material.
"""
import argparse
import hashlib
import os
import subprocess
import sys


def public_der(path: str):
    """The key's public key as DER (SubjectPublicKeyInfo), or None if it
    can't be read or parsed. Raises RuntimeError when no parser is found."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None
    try:
        from cryptography.hazmat.primitives import serialization
    except ImportError:
        serialization = None
    if serialization is not None:
        for load in (lambda d: serialization.load_pem_private_key(d, password=None).public_key(),
                     serialization.load_pem_public_key):
            try:
                pub = load(data)
            except (ValueError, TypeError):
                continue
            return pub.public_bytes(serialization.Encoding.DER,
                                    serialization.PublicFormat.SubjectPublicKeyInfo)
        return None
    for extra in ([], ["-pubin"]):
        try:
            r = subprocess.run(["openssl", "pkey", "-in", path, *extra, "-pubout",
                                "-outform", "DER", "-passin", "pass:"],
                               capture_output=True)
        except OSError:
            raise RuntimeError("neither the cryptography package nor openssl is available")
        if r.returncode == 0 and r.stdout:
            return r.stdout
    return None


class Unknown(Exception):
    """The comparison couldn't be made."""


def public_key(key: str, mcuboot: str) -> bool:
    k = os.path.realpath(key)
    if not mcuboot:
        raise Unknown("no MCUboot directory to compare with")
    m = os.path.realpath(mcuboot)
    if os.path.commonpath([k, m]) == m:
        return True
    try:
        names = sorted(n for n in os.listdir(m) if n.startswith("root-") and n.endswith(".pem"))
    except OSError as e:
        raise Unknown(f"MCUboot's directory can't be read ({e.strerror})")
    try:
        mine = public_der(k)
        if mine is None:
            raise Unknown(f"the signing key {key} doesn't parse as a PEM key")
        roots = [public_der(os.path.join(m, n)) for n in names]
    except RuntimeError as e:
        raise Unknown(str(e))
    roots = [r for r in roots if r]
    if not roots:
        raise Unknown("no MCUboot root-*.pem key could be read to compare with")
    return mine in roots


def blocklisted(key: str, blocklist: str) -> bool:
    """The key's public half (sha256 of its DER SubjectPublicKeyInfo) is on the
    --blocklist file's list of public, sample or committed keys. A list that can't be
    read, or a key whose public half can't be derived, can't be checked (Unknown)."""
    if not blocklist:
        return False
    try:
        der = public_der(key)
    except RuntimeError as e:
        raise Unknown(str(e))
    if der is None:
        raise Unknown(f"the signing key {key} doesn't parse as a PEM key")
    try:
        with open(blocklist) as f:
            listed = {line.split()[0].lower() for line in f
                      if line.strip() and not line.lstrip().startswith("#")}
    except OSError as e:
        raise Unknown(f"the key blocklist {blocklist} can't be read ({e.strerror})")
    return hashlib.sha256(der).hexdigest() in listed


def check(key: str, mcuboot: str, dev_ok: bool, blocklist: str = "") -> tuple[int, str]:
    how = ("production: set SB_CONFIG_BOOT_SIGNATURE_KEY_FILE to your own private key "
           "(outside the repository); a bench or development unit: "
           "-DSB_EXTRA_CONF_FILE=sysbuild-dev.conf")
    if not key:
        return 1, "No MCUboot signing key (SB_CONFIG_BOOT_SIGNATURE_KEY_FILE): " + how
    if not os.path.isfile(key):
        return 1, f"MCUboot signing key {key} not found"
    try:
        public = public_key(key, mcuboot) or blocklisted(key, blocklist)
    except Unknown as e:
        if dev_ok:
            return 0, f"Development build: the signing key wasn't checked ({e})."
        return 1, f"The MCUboot signing key couldn't be checked: {e}. " + how
    if public:
        if dev_ok:
            return 0, (f"Development build: signed with a public development key "
                       f"{os.path.basename(key)} (MCUboot's, or on the key blocklist); units "
                       f"built with it accept images anyone can sign. Never ship it.")
        return 1, (f"The MCUboot signing key {key} is a public development key (MCUboot's, "
                   f"or on the key blocklist): units built with it accept images anyone can "
                   f"sign. " + how)
    return 0, ""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--key", default="")
    ap.add_argument("--mcuboot", default="")
    ap.add_argument("--dev-ok", action="store_true")
    ap.add_argument("--blocklist", default="",
                    help="a file of public-key sha256 fingerprints (DER SubjectPublicKeyInfo, "
                         "one per line, # comments): any key listed there counts as public "
                         "(a committed dev key is a sample key)")
    a = ap.parse_args()
    rc, msg = check(a.key, a.mcuboot, a.dev_ok, a.blocklist)
    if msg:
        print(msg)
    return rc


if __name__ == "__main__":
    sys.exit(main())
