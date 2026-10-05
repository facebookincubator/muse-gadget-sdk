---
name: gadget-epson-printers
description: >-
  Print on Epson IPP printers or scan from compatible Epson eSCL devices through Muse Home
  Link. Use only when the exact model advertises the corresponding print or scan service; IPP
  alone does not establish scanning support.
---

# Epson IPP Printers and eSCL Scanners

Use this skill when a confirmed Epson printer advertises IPP/IPPS, or an exact Epson multifunction model exposes a documented eSCL scan service.

## Identify the Device

- Match manufacturer/model from current discovery and print or scan capability responses.
- Treat printing and scanning as distinct advertised services. IPP does not imply a scanner; sane-airscan compatibility does not mean every listed Epson device uses eSCL rather than another scan protocol.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- The actual service endpoints, any device authentication, and user permission for the document or physical scan.
- A binary IPP client for printing. For scanning, an eSCL-capable client and the exact device's advertised eSCL base path.

## Workflow

1. For printing, use Get-Printer-Attributes to read supported formats, media, sides, resolution, operations and current state. Submit a requested document with Print-Job in an advertised format and retain its job ID.
2. Monitor Get-Job-Attributes and printer reasons. Resolve an uncertain submission before resending it.
3. For an advertised eSCL service, read `ScannerCapabilities` and `ScannerStatus` beneath its service base. Select only supported input source, size, resolution, color mode and output format.
4. With a document placed in the selected feeder or on the platen, create the requested `ScanJobs` job using eSCL XML. Follow the returned job Location and retrieve `NextDocument` as the device's protocol requires; authorize any changed network destination first.
5. Retrieve all requested pages, handle feeder-empty and job-complete responses, and close or cancel the specific scan job if necessary. Keep scanned material in the user's approved storage.

## Verify the Result

- Check IPP status and terminal print-job state, not only HTTP status.
- For scans, validate the downloaded media type, readable image/document, page count and requested settings; an accepted scan job is not a retrieved scan.

## Limits

- Do not promise eSCL, duplex scanning or unauthenticated access for the whole Epson family.
- WSD-only scanning, SNMP/UDP discovery/control, browser automation and firmware changes are outside this skill.

## Sources

- [IPP operations and semantics](https://www.rfc-editor.org/rfc/rfc8011.html)
- [sane-airscan device and protocol documentation](https://github.com/alexpevzner/sane-airscan)
