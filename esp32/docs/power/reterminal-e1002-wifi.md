# reTerminal E1002 Wi-Fi modem sleep

The E1002 profile enables `CONFIG_HOMEHUB_WIFI_MIN_MODEM`, selecting
`WIFI_PS_MIN_MODEM` at Wi-Fi startup. The station stays associated and wakes
at each access-point DTIM. Incoming traffic can incur DTIM-dependent latency.
CPU frequency and display behavior are unchanged. Other board profiles retain
`WIFI_PS_NONE` at startup; their existing runtime power policies still apply.
Disable this option in the generated sdkconfig to compare or troubleshoot.

## Battery experiment

One E1002 was tested in October 2026 with the same instrumented firmware,
changing only the Wi-Fi power-save mode, in A/B/A order. A persistent logger
sampled voltage, boot ID, uptime, Wi-Fi association and power-save mode every
five minutes. Each row below ends at the last sample of continuous operation
before low-voltage restarts, not an exact shutdown measurement.

| Run | Wi-Fi mode | Voltage before unplug | Last voltage | Recorded runtime |
| --- | --- | --- | --- | --- |
| A2 | NONE | 4.130 V | 2.678 V | 21 h 11 min |
| B1 | MIN_MODEM | 4.124 V | 2.594 V | 45 h 55 min |
| A4 | NONE | 4.136 V | 2.776 V | 20 h 53 min |

B1 lasted about 2.18 times the mean baseline runtime. The two baselines differ
by about 1.4%. A separate attempted repeat (A3) was excluded because the host
lost power while connected, leaving the charging intervals ambiguous.

Duration is the last sample's host-anchored device uptime minus the observed
unplug wall-clock timestamp. Host monotonic elapsed time was not used: it did
not include Mac sleep. Only samples from the original boot were used; no wall
time was assigned across later restarts. A4 was recovered manually after the
USB port name changed. All three retained logs passed count and sequence checks.

The intended workload was two display updates per day; exact command counts
and end-to-end latency were not recorded. The user confirmed a Muse display
update on B1. Wi-Fi was associated at every discharge sample, and the selected
power-save mode was retained, but that does not prove Muse was reachable for
the entire run or establish an idle command-delivery guarantee. These are
single-device observations, not a current measurement or a general battery-life
guarantee. Logger overhead was present in both variants. The runtime results
precede the upstream rebase; the rebased firmware requires separate functional
validation and has not undergone another full battery discharge.
