---
name: gadget-hp-color-laserjet-pro-m254dw
description: >-
  Print documents and inspect printer or job status on HP Color LaserJet Pro M254dw through
  Muse Home Link. Use its advertised IPP/IPPS service and supported document formats.
---

# HP Color LaserJet Pro M254dw

Use this skill when fresh discovery identifies an HP Color LaserJet Pro M254dw with an IPP or IPPS print service.

## Identify the Device

- Match the service's model information and confirm `printer-make-and-model` using Get-Printer-Attributes.
- Use the discovered print-service endpoint and resource path. A web administration page is not necessarily an IPP endpoint.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- An authorized printer endpoint, any required print credentials, and a user-approved document and print options.
- A client that encodes binary IPP, such as an IPP library or CUPS tooling, with transport routed through HomeLink. An ordinary JSON HTTP request is not IPP.

## Workflow

1. Send Get-Printer-Attributes for `printer-state`, `printer-state-reasons`, `printer-is-accepting-jobs`, `operations-supported`, `document-format-supported`, media, sides and resolution capabilities.
2. Choose a format the printer actually advertises and prepare the document with a suitable renderer if necessary. Do not assume this model has the same format preferences as a Brother printer.
3. For the requested print, send Print-Job with the supported document MIME type and only requested, supported job attributes. Record the returned job ID or URI and the IPP status code.
4. Poll Get-Job-Attributes using that job identity until completed, canceled, aborted or a meaningful intervention state. Inspect job/printer reasons when paper, supplies or authorization prevent completion.

## Verify the Result

- An HTTP success is not necessarily IPP success. Check both protocol status and job state.
- Completion indicates the printer's job outcome; ask the user to check the physical page when layout or paper output matters.
- If a submission times out, inspect the job queue before retrying to avoid duplicate pages.

## Limits

- No SNMP/UDP, speculative PJL commands, firmware updates or administrative LEDM operations are part of this path.

## Sources

- [IPP/1.1 operations and semantics](https://www.rfc-editor.org/rfc/rfc8011.html)
- [CUPS ipptool](https://openprinting.github.io/cups/doc/man-ipptool.html)
