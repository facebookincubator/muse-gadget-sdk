---
name: gadget-brother-printers
description: >-
  Print documents and inspect print jobs on Brother IPP printers through Muse Home Link. Use
  when the printer advertises IPP/IPPS; check its formats and capabilities, including the
  documented MFC-J880DW PWG Raster path.
---

# Brother Printers: IPP Printing

Use this skill when fresh discovery identifies a Brother printer offering IPP and the user asks to print or inspect a print job.

## Identify the Device

- Match Brother manufacturer/model and an advertised `_ipp._tcp` or `_ipps._tcp` service; do not identify a printer from its vendor prefix alone.
- Use the discovered IPv4 address, port and advertised resource path. Query `Get-Printer-Attributes` before submitting a document.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- The printer must be ready, supplied with the requested media, and authorized for the requested document.
- Use the printer's configured authentication and TLS trust where required. Prepare documents in the agent environment, not on HomeLink.

## Workflow

1. Read `printer-state`, `printer-state-reasons`, `printer-is-accepting-jobs`, `document-format-supported`, `media-supported`, resolution and duplex capabilities using an IPP client such as CUPS/ipptool.
2. Prefer `image/pwg-raster` when advertised. Generate PWG Raster with standard CUPS filters such as `imagetoraster` and `rastertopwg`; select installed filter options for the actual input and target resolution.
3. Match media, printable area, resolution, color and sides to advertised capabilities; scale the document layout for that resolution. Do not send PDF or JPEG merely because another printer supports it.
4. Send IPP `Print-Job` with the selected `document-format` and document bytes through the authorized printer endpoint. Preserve the returned job ID/URI.
5. Monitor that job with `Get-Job-Attributes`. Resolve paper, media or authentication errors before considering another submission.

## Verify the Result

- Confirm a successful IPP response, not just HTTP 200, and inspect `job-state` and `job-state-reasons`.
- A queued/processing job is not yet a printed page. Check completion and ask for physical-output confirmation when necessary.
- If the submission times out, inspect existing jobs before resending to avoid duplicate prints.

## Limits

- The PWG Raster path applies to MFC-J880DW; check advertised formats and capabilities on other Brother IPP models.
- Do not change printer configuration, print unsolicited pages or cancel unrelated jobs.

## Sources

- [IPP operations and attributes — RFC 8011](https://www.rfc-editor.org/rfc/rfc8011.html)
- [CUPS ipptool](https://openprinting.github.io/cups/doc/man-ipptool.html)
- [CUPS raster programming](https://openprinting.github.io/cups/doc/spec-raster.html)
