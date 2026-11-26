# Local white-background matting

The optional normal-avatar path can derive a foreground mask from a uniformly white backdrop without changing the avatar on the phone or VM. It retains the original RGB565 image and cache bytes, creates an immutable display image plus A8 mask once, and composites over the actual canvas background. Frame rendering allocates nothing and does not rerun segmentation.

Enable `CONFIG_MUSE_AVATAR_WHITE_MATTE=y` alongside avatar sync to include this path; it defaults off. It is independent of pixel-art selection. Normal avatar rendering remains selectable, and `pixel.on` / `pixel.off` only change the saved style preference. No phone Settings UI is added.

The detector requires all four corners and at least 98% of unique border cells to be near-neutral white. A four-connected flood removes only background-connected white pixels, not enclosed white highlights. A narrow soft-edge pass uses immutable strong-foreground neighbors and removes white edge contamination before A8 compositing. Blank images and nonuniform/colored borders are left unchanged. This is conservative uniform-white matting, not a learned or general-purpose segmentation model; white foreground merged into white background can remain ambiguous.

## Compiler and API

The same algorithm is available with `compile_image(..., remove_white_background=True)`, the CLI's `--remove-white-background`, and the strict JSON boolean `remove_white_background` on `POST /v1/compile`. The default is false and retains prior output behavior. Existing nonopaque RGBA is preserved. New opaque analysis is bounded to longest edge 466 before the final pixel profile is made; skipped analysis preserves the original image.

```sh
python -m avatar_pixel compile --input avatar.png --output avatar.mspix \
  --profile 64 --colors 24 --dither --remove-white-background --preview preview.png
```

## Use the device's mask without selecting pixel art

The paired canonical-still export can carry its existing A8 mask. The SDK helper detects stable `alpha_export` support rather than using a transient image-alpha observation, and checks the availability, size, CRC and generation of the actual pinned export.

```sh
python tools/muse/pixel_avatar.py compile-current --port PORT \
  --output avatar.mspix --preview preview.png --dither
python tools/muse/pixel_avatar.py install --port PORT --no-enable avatar.mspix
```

`--no-enable` installs data without changing the selected style. Legacy `pixel.image` still exports RGB565 only. `pixel.image_alpha` exports RGB565 followed by optional A8; `alpha_bytes`, `alpha_available`, the total size and CRC describe the pinned snapshot. A stale opaque decision that omits a present mask is rejected after draining the response, rather than silently producing an opaque black square.

## Storage, memory and limits

The existing normal-avatar cache format and original pixels are unchanged. Masks are reconstructed on cache restore and held with the same image reference count through the final strip. Temporary allocation pressure retains the original image and retries with bounded backoff; failed cache loads are no longer marked complete. With matting enabled the tracked allocation cap is 4 MiB instead of the legacy 1.5 MiB, with the existing PSRAM reserve still required. Allow this headroom on smaller boards.

H.264 clip views remain opaque and unchanged. This path handles canonical stills and optional pixel-art packs, not per-frame video alpha or facial/body rigging.

## Tests

`test_avatar_matte.py` exercises real native masking, alpha strip rendering, original-cache writes, pinned-image lifetime, allocation recovery, revocation, and bounded load retries under sanitizers. `test_avatar_matte_conformance.py` compares synthetic Python/C mask, rounding and RGB565 results. `test_pixel_alpha_export.py` checks legacy/RGBA export, both availability-transition directions, CRC/bounds/generation rejection, and style-preserving installation. Compiler/API tests cover opt-in defaults, enclosed-white preservation, true alpha, skip behavior and genuine loopback HTTP requests. Device appearance and memory validation remain separate from these synthetic tests.
