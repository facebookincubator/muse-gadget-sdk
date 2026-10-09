# PAA certificates

The commissioner accepts a Matter device only if its attestation chains to a
Product Attestation Authority (PAA) it trusts. `esp-matter`'s SPIFFS trust
store (`SPIFFS_ATTESTATION_TRUST_STORE`) reads them from the `paa_cert`
partition.

`paa_cert.bin` is that partition's image, built with
`tools/matter_paa_image.py` from:

- the PAAs on the CSA's Distributed Compliance Ledger (DCL) main net, as
  connectedhomeip keeps them in `credentials/production/paa-root-certs`
  (v1.6.1.0, commit 3bcdd56b, 2026-09-24): 75 certificates;
- CHIP's test PAA for vendor 0xFFF1
  (`credentials/test/attestation/Chip-Test-PAA-FFF1-Cert.der`), for test
  devices (below).

Total: 76 certificates.

## Flashing

`idf.py flash` (and `tools/board.sh BOARD flash`) writes the image with the
app. To write only this partition, for example after refreshing it:

```bash
python $IDF_PATH/components/partition_table/parttool.py -p PORT \
  --partition-table-offset 0x10000 \
  write_partition --partition-name paa_cert \
  --input components/esp32_matter_controller/paa/paa_cert.bin
```

`0x10000` is the firmware's `CONFIG_PARTITION_TABLE_OFFSET` (parttool's
default is `0x8000`). Always give `-p`: without it, parttool writes to the
first board it finds.

An OTA update doesn't change the partition: a device gets new certificates
when it is flashed over USB.

## Refreshing

1. Get the production certificates as `.der` files in a directory, with
   connectedhomeip's
   [`credentials/fetch_paa_certs_from_dcl.py`](https://github.com/project-chip/connectedhomeip/blob/master/credentials/fetch_paa_certs_from_dcl.py)
   (`--use-main-net-http`) or from its
   [`credentials/production/paa-root-certs`](https://github.com/project-chip/connectedhomeip/tree/master/credentials/production/paa-root-certs).
2. Build the image (after ESP-IDF's `export.sh`), with the FFF1 test PAA from
   [`credentials/test/attestation`](https://github.com/project-chip/connectedhomeip/tree/master/credentials/test/attestation):

   ```bash
   tools/matter_paa_image.py path/to/paa-root-certs path/to/Chip-Test-PAA-FFF1-Cert.der
   ```

   It takes `.der` files and directories of them and writes
   `components/esp32_matter_controller/paa/paa_cert.bin`; the same
   certificates always give the same image. Update the list above.

## Test devices

- **What they are:** example firmware (CHIP's example apps, `esp-matter`'s
  examples) and uncertified devices using CHIP's example credentials, which
  chain to `Chip-Test-PAA-FFF1` and claim vendor 0xFFF1.
- **Attestation:** the image trusts that PAA, which only vouches for vendor
  0xFFF1.
- **Certification declarations:** signed with CHIP's test key, which the
  firmware accepts by default
  (`CONFIG_ESP_MATTER_COMMISSIONER_SUPPORT_TEST_CD=y` in
  `devices/sdkconfig.matter-controller`). In product firmware, set it to `n`
  to accept only certified devices; test devices then fail with
  `attestation_failed`.
- **Other test vendors:** devices using CHIP's example credentials for vendor
  0xFFF2 or 0xFFF3 can't be added; build them for vendor 0xFFF1.

## Limits

A device whose manufacturer's PAA is newer than this image fails to commission
with `attestation_failed`. Revocation lists are not checked.
