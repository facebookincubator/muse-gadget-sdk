# Upstream PR candidates — muse-gadget-sdk

New device skills authored locally, staged here in the repo's own
format (`gadget-<device>/SKILL.md`, Markdown-only, no personal data,
IPs, or credentials) for submission to facebookincubator/muse-gadget-sdk.

| Candidate | Device class | Authored | Local copy |
|---|---|---|---|
| gadget-homewhiz-laundry | HomeWhiz (Arçelik/Beko/Blomberg) washer/dryer — cloud state lane | 2026-11-07 | ~/workspace/custom-skills/gadget-homewhiz-laundry/ |
| gadget-vesync-humidifier | VeSync (Levoit) humidifier — cloud state + control lane | 2026-11-07 | ~/workspace/custom-skills/gadget-vesync-humidifier/ |
| gadget-roborock-body | Roborock vacuum — piloting/movement, map + obstacle-photo eyes, live camera via account MQTT lane; companion to gadget-roborock-vacuums | 2026-11-07 | ~/workspace/custom-skills/local-roborock-body/ |
| gadget-nvidia-shield-tv | NVIDIA Shield TV — fixed Wi-Fi scan-RSSI sensing point (sideloaded FGS app + ADB sampler); RSSI-class presence corroboration, no-CSI honesty encoded | 2026-11-07 | ~/workspace/shield-wifi-sense/ (+ public repo jonathancruzate/shield-wifi-sense) |
| gadget-windows-pc | Windows PC — Wake-on-LAN via a LAN-resident sender, verify-by-service; Fast Startup/BIOS diagnosis ladder | 2026-11-07 | ~/workspace/custom-skills/local-wake-on-lan/ |

## Submission state (2026-11-07)

SUBMITTED 2026-11-07 via the GitHub web lane. Fork created:
jonathancruzate/muse-gadget-sdk. PRs open upstream:
- gadget-homewhiz-laundry → https://github.com/facebookincubator/muse-gadget-sdk/pull/347
- gadget-vesync-humidifier → https://github.com/facebookincubator/muse-gadget-sdk/pull/348
Both await: (1) Jonathan's Meta CLA signature (code.facebook.com/cla) —
the meta-cla bot commented on both; (2) one approving maintainer review.
Known nit: both PRs set the catalog count 47 → 48; whichever merges
second needs its count renumbered. The original staging note follows:

### gadget-roborock-body (2026-11-07)

BRANCH PUSHED, PR CREATION BLOCKED on the connector — web lane needed.
Branch `gadget-roborock-body` on jonathancruzate/muse-gadget-sdk
(from fork main 463bd2a05380fea4211b0099ad0f70933c108b20), pushed
commit 99bfdacf227382b63a0b0353b79ecb552857f875 with
`skills/gadget-roborock-body/SKILL.md` + updated `skills/CATALOG.md`
(47 → 48 active / 46 → 47 device-family, new Vacuums entry directly
after Roborock), verified by read-back. The connector's
`create_pull_request` to facebookincubator/muse-gadget-sdk failed:
`403 Resource not accessible by integration` on
POST /repos/facebookincubator/muse-gadget-sdk/pulls. Prepared PR
payload (title, head, base, full body) is saved at
`~/workspace/muse-gadget-sdk-roborock-body-staging/PR-PAYLOAD.md`
(+ PR-BODY.md / PR-ARGS.json there) for the parent to open via the
GitHub web lane, as with #347/#348. If #347/#348 merge first, this
PR's catalog count needs a renumber, as noted in its body.

STAGED, not yet submitted. This clone's origin is the upstream repo
itself; there is no fork under the user's account and no push access.
The remaining step needs the GitHub web lane (the connector cannot
create repos/forks):

1. Fork facebookincubator/muse-gadget-sdk to the user's account.
2. Copy each candidate to `skills/<name>/SKILL.md` on a branch.
3. Open one PR per skill (or one combined PR, per repo convention in
   CONTRIBUTING.md), describing the verified live deployment behind
   each (a working direct client + a production home deployment,
   details scrubbed).

Also add both to skills/CATALOG.md in the same PR(s) if the repo's
contribution flow expects catalog entries with new skills.

### gadget-nvidia-shield-tv + roborock-vacuums RSSI (2026-11-07)

SUBMITTED 2026-11-07 via the GitHub web lane (connector branch push + browser PR creation, same as #347/#348):
- gadget-nvidia-shield-tv -> https://github.com/facebookincubator/muse-gadget-sdk/pull/352 (branch gadget-nvidia-shield-tv @ f0d853c3; skill + CATALOG 47 -> 48)
- Roborock vacuums GET_NETWORK_INFO RSSI section -> https://github.com/facebookincubator/muse-gadget-sdk/pull/353 (branch gadget-roborock-vacuums-rssi @ 8082aead; edit to upstream's own skill, no catalog change)
No CLA notice on either page at creation (Jonathan's CLA signed 2026-11-07). Catalog-count renumber note applies as with #347/#348.

### gadget-windows-pc (2026-11-07)

Branch gadget-windows-pc pushed to the fork @ a1a36bb4 (skill + new
CATALOG "Computing" section, count 47 -> 48); PR #354 OPEN AS DRAFT 2026-11-07 (https://github.com/facebookincubator/muse-gadget-sdk/pull/354) — GitHub caps non-write users at 5 open PRs per repo and Jonathan already has #347/#348/#350/#352/#353 open; drafts do not count toward the limit, so #354 waits there and can be marked Ready the moment one of the five merges or closes. CLA check passed on it. Origin: the house local-wake-on-lan skill + wol.py, proven
live the same day (PC woke via a Shield-sent magic packet).
