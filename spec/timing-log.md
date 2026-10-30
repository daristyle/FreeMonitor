# FreeMonitor timing log

Version 1 · Status: draft

This document is the contract between the programs that write timing logs (the C++ host and the Kotlin tablet app) and the tools that read them (the Python analysis in T29). Each side writes one row per frame, with the time of each processing stage.

## 1. Files

Logging is off unless asked for. Each side writes its own CSV file.

- **Host:** `FreeMonitorHost --timing-log FILE`. An existing file with that name is overwritten.
- **Tablet:** start the app with `adb shell am start -n io.github.daristyle.freemonitor/.MainActivity --ez timingLog true`. The file is `/sdcard/Android/data/io.github.daristyle.freemonitor/files/timing/tablet-timing-YYYYMMDD-HHMMSS.csv`. Fetch it with `adb pull`.

The tablet starts a new file each time its video surface is created. That is normally once per run, but a run has more than one file if the app returns from the background or the host changes codec. The file name has a resolution of one second, so a file started in the same second as the one before it replaces it.

## 2. Format

The first line is `# freemonitor-timing <version> <side>`, where `<side>` is `host` or `tablet`. The second line holds the column names. Every other line is one frame. Values are separated by commas and are never quoted.

The format version is `1`. Any change to the columns, their order or their meaning raises the version. A reader must refuse a version it does not know.

Times are integers in microseconds. An empty field means that the stage did not happen for that frame.

`pandas.read_csv(path, comment="#")` reads a file directly.

Example host file:

```
# freemonitor-timing 1 host
frameId,outcome,codec,keyframe,frameBytes,packetsSent,captureUs,acquireUs,encodeStartUs,encodeEndUs,sendStartUs,sendEndUs
3141592653,SENT,H264,1,84211,63,812345678901,812345679850,812345679910,812345685420,812345685431,812345685902
,ENCODE_FAILED,JPEG,0,0,0,812345695512,812345696480,812345696530,812345696610,,
```

## 3. Host columns

| Column | Meaning |
| --- | --- |
| `frameId` | The frame's `frameId` in the protocol ([4.1](protocol.md#41-fragment-header)). Empty if the frame never reached the sender or the sender refused it. |
| `outcome` | What happened to the frame, see below. |
| `codec` | `JPEG` or `H264`. |
| `keyframe` | `1` or `0`. |
| `frameBytes` | Encoded size in bytes. |
| `packetsSent` | Fragments sent. |
| `captureUs` | The compositor's presentation time of the captured frame, which is the frame's `captureTimestampUs` in the protocol. If the compositor gives no usable time, this is the moment the host processed the buffer instead, so `acquireUs - captureUs` is then close to zero. |
| `acquireUs` | The host got the frame from the capture source. |
| `encodeStartUs`, `encodeEndUs` | Around the encoder. |
| `sendStartUs`, `sendEndUs` | Before the first and after the last fragment was sent. |

| Outcome | Meaning |
| --- | --- |
| `SENT` | All fragments were sent. |
| `ENCODED` | The frame was encoded, but the host had no destination. `frameId` and the send times are empty. |
| `ENCODE_FAILED` | The encoder produced no frame. `frameId` and the send times are empty. `codec`, `keyframe`, `frameBytes` and `packetsSent` hold the placeholders `JPEG`, `0`, `0` and `0`, even on a host that encodes H.264, so a reader must not count these rows by codec or size. |
| `SEND_FAILED` | Not all fragments were sent: the socket buffer was full, sending failed, or the sender refused the frame. `packetsSent` is the number that did go out. `frameId` is empty only if the sender refused the frame. |

## 4. Tablet columns

| Column | Meaning |
| --- | --- |
| `frameId` | The frame's `frameId` in the protocol. Never empty. |
| `outcome` | What happened to the frame, see below. |
| `keyframe` | `1` or `0`. |
| `frameBytes` | Size of the received frame in bytes. |
| `hostCaptureUs` | The frame's `captureTimestampUs`, in the **host's** clock. It is there to match rows with the host file. |
| `firstFragmentUs`, `completeUs` | The frame's first and last fragment arrived. |
| `acquireUs` | The app took the frame from the receiver. The gap after `completeUs` is time spent waiting in the receiver's mailbox. |
| `decodeStartUs`, `decodeEndUs` | Around decoding. For H.264, `decodeStartUs` is when the frame was handed to the decoder and `decodeEndUs` is when the decoded picture was ready. |
| `postUs` | The frame was handed to the display. It appears on screen about one refresh later, which only filming can measure (T28). |

| Outcome | Meaning | Times that are filled in |
| --- | --- | --- |
| `SHOWN` | The frame was handed to the display. | All |
| `NOT_DRAWN` | The frame was not drawn: the surface was not available, or it was being replaced after a codec change. | `acquireUs`, plus the decode times if the frame was decoded |
| `DECODE_FAILED` | Decoding failed. | `acquireUs`, plus the decode times for JPEG |
| `UNSUPPORTED_CODEC` | The frame uses a codec the app does not know. | `acquireUs` |
| `WAITING_FOR_KEYFRAME` | An H.264 frame was discarded because an earlier frame was missing. The app waits for the next keyframe. | `acquireUs` |

The arrival times `firstFragmentUs` and `completeUs` are filled in for every outcome.

## 5. Reading the rows

**Order.** Rows are written when a frame's handling ends, so the row position is not the frame order. On the tablet, an H.264 frame's row is written when the decoder outputs it, while a discarded frame's row is written at once. A discarded frame can therefore appear before an earlier frame that is still inside the decoder.

**Comparing frames.** `frameId` starts at a random value and wraps at 2^32, so sort and compare it with serial number arithmetic ([2.1](protocol.md#21-seq)), not as plain integers.

**Matching the two files.** Match rows by `frameId`, and check that `hostCaptureUs` equals the host's `captureUs`. A host frame with no tablet row has one of these causes:

- It was lost, or dropped because it stayed incomplete.
- A newer frame replaced it before the app took it.
- The decoder never output it.
- Its row was dropped from the log ([7](#7-losses-in-the-log)).

## 6. Stages

Each device's times come from its monotonic clock, the same clock as `timestampUs` ([2.2](protocol.md#22-timestampus)). All stages within one device can be subtracted directly:

| Stage | Calculation | Side |
| --- | --- | --- |
| Capture delay | `acquireUs - captureUs` | Host |
| Waiting for the encoder | `encodeStartUs - acquireUs` | Host |
| Encoding | `encodeEndUs - encodeStartUs` | Host |
| Waiting to send | `sendStartUs - encodeEndUs` | Host |
| Sending | `sendEndUs - sendStartUs` | Host |
| Network | `completeUs - sendEndUs` | Both, see below |
| Fragment arrival | `completeUs - firstFragmentUs` | Tablet |
| Waiting in the mailbox | `acquireUs - completeUs` | Tablet |
| Waiting for the decoder | `decodeStartUs - acquireUs` | Tablet |
| Decoding | `decodeEndUs - decodeStartUs` | Tablet |
| Handing to the display | `postUs - decodeEndUs` | Tablet |

The two devices' clocks are not synchronized, so the network stage needs the offset between them, which the `HEARTBEAT` round trips of T18 will provide. Until then, `completeUs - sendEndUs` still shows network jitter: it equals the offset plus the delay, so subtracting its minimum gives each frame's extra delay. The offset changes slowly because clocks drift, so this works for short runs. For long runs, subtract a fitted straight line instead of the minimum.

## 7. Losses in the log

Each side keeps up to 1024 rows waiting and writes them out every 500 ms. A row that does not fit is dropped, never delayed. The host prints how many at exit, and the tablet logs it under the logcat tag `FreeMonitor`. Check this number before reading a missing tablet row as a lost frame.

A process that is killed loses the rows it has not written out yet.

## 8. Not in the log

The log holds per-frame timing only. A benchmark has to record these itself:

- CPU use
- The encoder that was used, and the bitrate and resolution settings
- The emulated loss and delay
- The devices tested
