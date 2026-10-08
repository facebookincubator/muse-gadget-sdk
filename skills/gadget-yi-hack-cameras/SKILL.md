---
name: gadget-yi-hack-cameras
description: >-
  Retrieve snapshots or bounded streams from cameras already running compatible yi-hack
  firmware through Muse Home Link. Use the installed fork's enabled HTTP or TCP-interleaved
  RTSP service; not stock-camera access, flashing, or camera control.
---

# Yi Cameras with Existing yi-hack Firmware

Use this skill when the camera already runs a confirmed compatible yi-hack fork with its local snapshot or RTSP service enabled.

## Identify the Device

- Confirm the exact camera hardware, installed yi-hack fork/version and enabled interfaces from existing configuration.
- Use that fork's documented endpoint/path; do not assume all Yi cameras or yi-hack forks share a snapshot URL or stream name.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- Already-installed compatible firmware and its existing HTTP/RTSP credentials.
- A credential-aware HTTP or RTSP client using HomeLink. RTSP media must support interleaved TCP.
- User permission for the particular camera and requested image/view/recording.

## Workflow

1. Select the installed fork's documented HTTP snapshot endpoint or enabled RTSP stream. Use only the current authorized host and service.
2. For a still snapshot, make the documented authenticated read and verify the returned image type/content rather than saving an HTML error page.
3. For RTSP, explicitly use interleaved TCP for control and media; do not fall back to UDP. Decode only the requested frame or bounded stream.
4. Store the result in the user's approved private location and close the session after completing the request.

## Verify the Result

- Check that the image is fresh, decodable and from the requested camera, not cached content or a login response.
- An available web UI or open RTSP port is not proof of successful image capture.

## Limits

- Read-only snapshot/view/capture scope; no camera settings, firmware changes or arbitrary CGI operations.
- No stock-camera support, automatic flashing or claim that firmware changes are harmless or reversible.
- No UDP streams, unsupported hardware/fork combinations, implicit recording or media sharing.

## Sources

- [yi-hack Allwinner v2 documentation and supported models](https://github.com/roleoroleo/yi-hack-Allwinner-v2)
