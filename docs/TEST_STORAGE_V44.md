# V44 Wi-Fi storage experiment

Wi-Fi-only AP slots changed from 512 to 256; scan-metadata slots changed from
1024 to 512. The 80 KB allocation-time heap reserve and dual-radio settings
are unchanged. This reassigns 15,360 bytes from fixed tables to observations.

## Device comparison

Measured on the COM4 classic ESP32 over its infrastructure Wi-Fi connection.
V43 held 652 real observations; V44 was filled to 95% using 16 synthetic APs.
These are short sequential HTTP tests, not browser paint times or a controlled
RF endurance trial. The different data and uptime limit causal comparisons.

| Metric | V43 | V44, 95% full |
| --- | ---: | ---: |
| Observation capacity | 666 | 3,227 |
| Retained observations | 652 | 3,065 |
| Allocated history bytes | 34,716 | 34,722 |
| Free heap at final export, bytes | 51,256 | 52,484 |
| Minimum heap since boot, bytes | 18,944 | 17,900 |
| Main page median, ms | 719 | 1,157 |
| Observed-network fragment median, ms | 297 | 219 |
| Channel fragment median, ms | 63 | 62 |
| Terminal median, ms | 375 | 328 |

Each median above uses three requests. Main-page samples varied substantially
(V43: 1672/719/656 ms; V44: 1562/1157/421 ms), so the higher V44 median warrants
longer testing; this does not establish a speed improvement. No request failed
in these runs. Both full-history exports reported zero history-integrity
anomalies and zero AP-table drops.

A separate populated V44 RSSI plot took 281 ms median across five requests,
returning 22,976 bytes. CSV export contained 3,065 observations. Empty-selection
plot timings from the initial benchmark do not measure actual plotting work.

## Tradeoffs and recovery

Capacity increased about 4.8 times at essentially the same history RAM cost.
The distinct-AP ceiling is now 256; metadata can retain at most 512 scan groups.
Old observations can expire due to either table limit before the observation
ring fills, especially with few APs per scan. Dense/mobile surveys need further
testing for AP-table drops and recycling.

V43 checkpoints contain the old metadata-table size and are rejected by V44.
The original real survey was exported to `logs/storage-original.csv` before
flashing. Synthetic test data was removed afterward and a real scan requested.
Raw before/after exports and timings are in ignored `logs/storage-*.json` on
the test machine. Preserve these separately if sharing or cleaning build logs.

The experiment remains V44; long-run performance and battery life have not
been established. Run the existing survey/recovery tests and the lag recorder
during extended field trials. Reducing the runtime reserve is not part of
this change.
