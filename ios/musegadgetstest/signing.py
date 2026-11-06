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

"""Finds how to sign the app on this Mac, for build.sh.

signing.py BUNDLE_ID OUT_DIR [PROFILE] [DEVICE_UDID]

Picks a development provisioning profile that Xcode has downloaded (or the
one given) whose app ID covers BUNDLE_ID (exactly, preferred, or a wildcard),
that hasn't expired, that lists the iPhone if one is given, and one of whose
certificates is a signing identity in your keychain: a free Personal Team's
profile works. Writes OUT_DIR/embedded.mobileprovision and
OUT_DIR/app.entitlements and prints the identity's SHA-1. Exits 1 with what
to do if there's none.
"""
import datetime
import glob
import hashlib
import os
import plistlib
import shutil
import subprocess
import sys

DIRS = [
    "~/Library/Developer/Xcode/UserData/Provisioning Profiles",
    "~/Library/MobileDevice/Provisioning Profiles",
]


def identities():
    out = subprocess.run(["security", "find-identity", "-v", "-p", "codesigning"],
                         capture_output=True, text=True).stdout
    return {line.split()[1]: line.split('"')[1] for line in out.splitlines() if '"' in line and ")" in line}


def load(path):
    data = subprocess.run(["security", "cms", "-D", "-i", path], capture_output=True).stdout
    return plistlib.loads(data) if data else None


def score(p, bundle, udid, ids):
    """(rank, identity sha1) if the profile will do, else None. Lower rank is better."""
    ent = p.get("Entitlements", {})
    if not ent.get("get-task-allow"):
        return None   # not a development profile
    exp = p.get("ExpirationDate")
    if exp and exp.replace(tzinfo=datetime.timezone.utc) < datetime.datetime.now(datetime.timezone.utc):
        return None
    if udid and not p.get("ProvisionsAllDevices") and udid not in p.get("ProvisionedDevices", []):
        return None
    prefix = p["ApplicationIdentifierPrefix"][0]
    appid = ent.get("application-identifier", "")
    if appid == f"{prefix}.{bundle}":
        rank = 0
    elif appid.endswith("*") and f"{prefix}.{bundle}".startswith(appid[:-1]):
        rank = 1
    else:
        return None
    for der in p.get("DeveloperCertificates", []):
        sha = hashlib.sha1(der).hexdigest().upper()
        if sha in ids:
            return rank, sha
    return None


def main():
    bundle, out = sys.argv[1], sys.argv[2]
    given = sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] else None
    udid = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] else None
    ids = identities()
    paths = [given] if given else [f for d in DIRS for f in glob.glob(os.path.join(os.path.expanduser(d), "*.mobileprovision"))]
    best = None
    for path in paths:
        p = load(path)
        s = p and score(p, bundle, udid, ids)
        if s and (not best or s[0] < best[0]):
            best = (s[0], s[1], path, p)
    if not best:
        sys.stderr.write(f"""No development provisioning profile on this Mac covers {bundle}{' and your iPhone' if udid else ''}
with a certificate in your keychain. To get one with a free Apple ID:
  1. In Xcode: Settings > Accounts, add your Apple ID (a Personal Team is enough).
  2. File > New > Project > iOS App; set the bundle identifier to {bundle}
     (or pick your own, e.g. com.yourname.musegadgetstest, and build with
     BUNDLE_ID set to it: bundle IDs are unique across all teams); choose your
     team under Signing & Capabilities.
  3. Connect your iPhone (Developer Mode on) and run that project on it once.
     Xcode then makes the certificate and the profile; run build.sh again.
Or pass a profile with PROFILE=path/to/profile.mobileprovision.
""")
        return 1
    _, sha, path, p = best
    shutil.copy(path, os.path.join(out, "embedded.mobileprovision"))
    prefix, team = p["ApplicationIdentifierPrefix"][0], p["TeamIdentifier"][0]
    ent = {
        "application-identifier": f"{prefix}.{bundle}",
        "com.apple.developer.team-identifier": team,
        "get-task-allow": True,
    }
    with open(os.path.join(out, "app.entitlements"), "wb") as f:
        plistlib.dump(ent, f)
    sys.stderr.write(f"signing as {ids[sha]} with profile \"{p.get('Name')}\" (team {p.get('TeamName', team)})\n")
    print(sha)
    return 0


if __name__ == "__main__":
    sys.exit(main())
