# FreeMonitor protocol

Version 1 · Status: ready for review

This document is the contract between the host (C++), the tablet app (Kotlin and C++ through the NDK) and the Python tools. Every implementation must follow it byte for byte. The packet schema is generated from this spec, and the conformance tests check implementations against it.

## 1. Conventions

### 1.1 Transport

Each packet is sent as exactly one UDP datagram, and each datagram contains exactly one packet: packets are never combined or split across datagrams. So a packet's length is the length of its datagram.

FreeMonitor uses IPv4 only. The host listens on UDP port `HOST_PORT`; the port is configurable. The tablet sends from any local port. Packets belong to a session through the `sessionId` in their header ([2](#2-common-header)), not through their address and port ([6.3](#63-session)).

### 1.2 Byte order and types

- All multi-byte integers are **big-endian** (network byte order).
- Field types are fixed-size integers: unsigned `u8`, `u16`, `u32`, `u64` and signed `i32` (two's complement).
- There is no padding between fields. Offsets in this document are in bytes from the start of the datagram.
- Every field is aligned: its offset is a multiple of its size. This is only a convenience. Implementations must still read and write each field explicitly, byte by byte, and never by casting a pointer into the packet buffer.
- Field names are lowerCamelCase, the same in every language.

### 1.3 Packet size

- The UDP payload of a packet must be at most `MAX_PACKET_SIZE` = **1400 bytes**.
- On a normal 1500-byte network, the largest UDP payload that is not split into IP fragments is 1472 bytes (1500 MTU − 20 IP − 8 UDP). If one IP fragment of a datagram is lost, the whole datagram is lost, so packets must stay below that limit.
- The 72-byte margin leaves room for encryption in version 2 (24-byte nonce + 16-byte tag, giving 1440 bytes, [7.4](#74-encryption)), with 32 bytes to spare.
- All fragment sizes are derived from `MAX_PACKET_SIZE` ([1.5](#15-constants)), so changing it means changing one value.

### 1.4 Invalid packets

A receiver silently drops a packet, without replying, when any of these is true:

- It is shorter than the common header (24 bytes).
- `magic` is not `0x464D`.
- `version` is not supported.
- `type` is unknown.
- Its length does not match what its type requires.
- A reserved field or bit is not `0`.
- `sessionId` is `0`, except in development mode ([6.6](#66-development-mode)).

Dropping an invalid packet must never change the receiver's state.

The one exception to dropping silently: the host answers a `HELLO` whose `version` it does not support with `REJECT` ([6.5](#65-reject-and-version-stability)).

### 1.5 Constants

| Name | Value | Meaning |
|------|------:|---------|
| `MAX_PACKET_SIZE` | 1400 | Largest UDP payload of a packet. |
| `MAX_FRAGMENT_PAYLOAD` | `MAX_PACKET_SIZE` − 48 = 1352 | Frame data per `VIDEO_FRAGMENT` ([4.2](#42-fragment-size)). |
| `MAX_SHAPE_PAYLOAD` | `MAX_PACKET_SIZE` − 40 = 1360 | Pixel data per `CURSOR_SHAPE` ([5.2](#52-cursor_shape)). |
| `MAX_FRAME_SIZE` | 4,194,304 (4 MiB) | Largest video frame ([4.2](#42-fragment-size)). |
| `HOST_PORT` | 50500 | Default UDP port of the host. |
| `DISCOVERY_PORT` | 50501 | Fixed UDP port for discovery ([7.5](#75-discovery)). |

## 2. Common header

Every packet starts with this 24-byte header. The type-specific payload follows directly after it.

| Offset | Field            | Type | Description |
|-------:|------------------|------|-------------|
| 0      | `magic`          | u16  | Always `0x464D` (ASCII `"FM"`). |
| 2      | `version`        | u8   | Protocol version. This document defines version `1`. |
| 3      | `type`           | u8   | Packet type; see [Packet types](#3-packet-types). |
| 4      | `sessionId`      | u32  | Session the packet belongs to; see [6.3](#63-session). `0` only in development mode. |
| 8      | `seq`            | u32  | Sender's packet sequence number. |
| 12     | `headerReserved` | u32  | Must be `0`. |
| 16     | `timestampUs`    | u64  | Sender's monotonic clock in microseconds, read when the packet is sent. |

Informative Python `struct` format: `"!HBBIIIQ"`. The packet schema is the authoritative source for generated code.

### 2.1 `seq`

- Each side keeps one counter per session for the packets it sends in that session, of every type. The two directions are independent. The counter increases by 1 per packet.
- The counter wraps from `2^32 − 1` to `0`.
- The starting value is not specified, and receivers must not assume it is 0.
- Receivers compare sequence numbers with serial number arithmetic (RFC 1982). Never use a plain `<` or `>`:

  ```
  diff(b, a) = ((b − a) mod 2^32), minus 2^32 if that value is ≥ 2^31
  ```

  `diff > 0` means `b` is newer, `diff < 0` means it is older, and `diff == 0` means it is a duplicate. Two values more than 2^31 apart cannot be ordered.

### 2.2 `timestampUs`

- It comes from a monotonic clock that never jumps backwards (for example `CLOCK_MONOTONIC`, `QueryPerformanceCounter` or `time.monotonic_ns()`).
- The clocks of different devices are **not** synchronized. A receiver may compare timestamps from the same sender, or with its own clock, only to measure intervals. It must not compare them as absolute times.

## 3. Packet types

| Value  | Name               | Section |
|--------|--------------------|---------|
| `0x01` | `VIDEO_FRAGMENT`   | [4](#4-video-fragments) |
| `0x10` | `CURSOR_POS`       | [5.1](#51-cursor_pos) |
| `0x11` | `CURSOR_SHAPE`     | [5.2](#52-cursor_shape) |
| `0x20` | `HELLO`            | [6.4](#64-messages) |
| `0x21` | `WELCOME`          | [6.4](#64-messages) |
| `0x22` | `ACK`              | [6.4](#64-messages) |
| `0x23` | `HEARTBEAT`        | [6.4](#64-messages) |
| `0x24` | `KEYFRAME_REQUEST` | [6.4](#64-messages) |
| `0x25` | `SHAPE_REQUEST`    | [6.4](#64-messages) |
| `0x26` | `STATS`            | [6.4](#64-messages) |
| `0x27` | `BYE`              | [6.4](#64-messages) |
| `0x28` | `REJECT`           | [6.5](#65-reject-and-version-stability) |

Ranges: `0x01–0x0F` video, `0x10–0x1F` cursor, `0x20–0x2F` control, `0x30–0x3F` discovery ([7.5](#75-discovery)). All other values are invalid.

## 4. Video fragments

A video frame is one encoded image: a complete JPEG file, or one H.264 access unit. It is split into fragments, and each fragment is sent as its own `VIDEO_FRAGMENT` packet.

### 4.1 Fragment header

The fragment header follows the common header. The fragment's slice of the frame data follows the fragment header and runs to the end of the datagram.

| Offset | Field                | Type | Description |
|-------:|----------------------|------|-------------|
| 24     | `frameId`            | u32  | Frame number. Increases by 1 per frame and wraps at 2^32. The starting value is not specified, and receivers must not assume it is 0. |
| 28     | `fragIndex`          | u16  | Index of this fragment, from `0` to `fragCount − 1`. |
| 30     | `fragCount`          | u16  | Number of fragments in the frame, at least `1`. |
| 32     | `frameSize`          | u32  | Total size of the frame data in bytes, from 1 to `MAX_FRAME_SIZE`. |
| 36     | `codec`              | u8   | `0` = JPEG, `1` = H.264. All other values are invalid. |
| 37     | `flags`              | u8   | Bit 0 = `KEYFRAME`. Bits 1–7 are reserved and must be `0`. |
| 38     | `reserved`           | u16  | Must be `0`. |
| 40     | `captureTimestampUs` | u64  | Sender's monotonic clock in microseconds, read when the frame was captured. |
| 48     | payload              | —    | Frame data from byte `fragIndex × MAX_FRAGMENT_PAYLOAD` onwards. |

Informative Python `struct` format, header included: `"!HBBIIIQIHHIBBHQ"` (48 bytes).

### 4.2 Fragment size

- `fragCount` = ceil(`frameSize` / `MAX_FRAGMENT_PAYLOAD`).
- Every fragment except the last carries exactly `MAX_FRAGMENT_PAYLOAD` bytes. The last fragment carries `frameSize − (fragCount − 1) × MAX_FRAGMENT_PAYLOAD` bytes, which is between 1 and `MAX_FRAGMENT_PAYLOAD`.
- So a fragment's payload always starts at byte `fragIndex × MAX_FRAGMENT_PAYLOAD` of the frame.
- Senders must never send a frame larger than `MAX_FRAME_SIZE`. An encoder that would exceed it lowers the quality of that frame instead.

Example, with `MAX_FRAGMENT_PAYLOAD` = 1352: a 50,000-byte JPEG is sent as 37 fragments. Fragments 0–35 carry 1352 bytes each and fragment 36 carries 1328 bytes.

### 4.3 Codec payloads

- **JPEG:** a complete baseline JFIF file. Every JPEG frame has `KEYFRAME` set.
- **H.264:** one access unit in Annex B format (start codes, no length prefixes). A keyframe is an IDR frame and must include its SPS and PPS, so a receiver can start decoding from any keyframe. H.264 has two kinds of frames: **keyframes** (I-frames), complete pictures that decode on their own, and **delta frames** (P-frames), which hold only the changes since the previous frame, such as a moved window or a changed line of text, and are often 10–50× smaller. A lost delta frame corrupts the picture until the next keyframe.

### 4.4 Validation

On top of the checks in [1.4](#14-invalid-packets), a receiver drops a `VIDEO_FRAGMENT` packet when any of these is true:

- `fragCount` is `0`, or `fragIndex ≥ fragCount`.
- `frameSize` is `0` or larger than `MAX_FRAME_SIZE`.
- `fragCount` ≠ ceil(`frameSize` / `MAX_FRAGMENT_PAYLOAD`).
- The payload length is not the length required by [4.2](#42-fragment-size).
- `codec` is unknown.
- It disagrees with an earlier fragment of the same `frameId` on `fragCount`, `frameSize`, `codec`, `flags` or `captureTimestampUs`.

### 4.5 Reassembly

- The receiver buffers fragments by `frameId` until all `fragCount` fragments of a frame have arrived, then delivers the frame.
- Duplicate fragments are ignored.
- `frameId` values are compared with serial number arithmetic ([2.1](#21-seq)).
- The receiver never delivers a frame older than, or the same as, the last frame it delivered. Older fragments are dropped.
- When a frame is delivered, all incomplete frames older than it are discarded.
- Receivers should keep at most 4 incomplete frames, and discard each one 100 ms after its first fragment arrived. Because frames never exceed `MAX_FRAME_SIZE`, a receiver can preallocate its reassembly buffers once (4 × `MAX_FRAME_SIZE`).
- A lost H.264 frame breaks the frames that reference it. The receiver recovers by requesting a keyframe ([6.4](#64-messages)).

## 5. Cursor packets

The host captures video without the cursor and sends the cursor separately: its position as small `CURSOR_POS` packets, and its image as `CURSOR_SHAPE` packets that are sent only when the image changes. The tablet draws the cursor on top of the video. Position updates skip capture and encoding, so the cursor moves with less delay than the video.

Coordinates are in pixels of the video frame, with `(0, 0)` at its top-left corner.

### 5.1 `CURSOR_POS`

| Offset | Field       | Type | Description |
|-------:|-------------|------|-------------|
| 24     | `shapeId`   | u32  | Shape to draw; see [5.2](#52-cursor_shape). |
| 28     | `x`         | i32  | Hotspot x. Negative or beyond the frame when the cursor is on another monitor. |
| 32     | `y`         | i32  | Hotspot y. Same as `x`. |
| 36     | `flags`     | u8   | Bit 0 = `VISIBLE`. Bits 1–7 are reserved and must be `0`. |
| 37     | `reserved0` | u8   | Must be `0`. |
| 38     | `reserved1` | u16  | Must be `0`. |

Total size: 40 bytes. Informative Python `struct` format: `"!HBBIIIQIiiBBH"`.

Sender:
- Sends a `CURSOR_POS` packet whenever the position, shape or visibility changes, at most once per 4 ms.
- Repeats the latest state at least every 250 ms even if nothing changed, so a lost packet is corrected.
- Clears `VISIBLE` when the cursor is hidden or is on another monitor.

Receiver:
- Ignores a `CURSOR_POS` packet whose `seq` is not newer than the last `CURSOR_POS` it accepted ([2.1](#21-seq)).
- Draws the shape's image with its top-left corner at `(x − hotspotX, y − hotspotY)`.
- If it does not have `shapeId`, keeps drawing the last shape it has and requests the missing one ([6.4](#64-messages)).

### 5.2 `CURSOR_SHAPE`

A cursor image can be larger than one packet, so it is fragmented with the same rules as video ([4.2](#42-fragment-size)), but with a different header and fragment size.

| Offset | Field       | Type | Description |
|-------:|-------------|------|-------------|
| 24     | `shapeId`   | u32  | Shape identifier. |
| 28     | `fragIndex` | u16  | Index of this fragment, from `0` to `fragCount − 1`. |
| 30     | `fragCount` | u16  | Number of fragments, at least `1`. |
| 32     | `width`     | u16  | Image width in pixels, from 1 to 256. |
| 34     | `height`    | u16  | Image height in pixels, from 1 to 256. |
| 36     | `hotspotX`  | u16  | Hotspot column, less than `width`. |
| 38     | `hotspotY`  | u16  | Hotspot row, less than `height`. |
| 40     | payload     | —    | Pixel data from byte `fragIndex × MAX_SHAPE_PAYLOAD` onwards. |

Informative Python `struct` format, header included: `"!HBBIIIQIHHHHHH"` (40 bytes).

Pixel data:
- `width × height × 4` bytes, in **BGRA** byte order (blue, green, red, alpha) with 8 bits per channel.
- Alpha is **straight**, not premultiplied. Hosts whose cursor source uses another format, such as premultiplied or monochrome cursors, convert it.
- Rows run from top to bottom, and pixels within a row from left to right.
- The hotspot is the pixel that points, for example the tip of the arrow.

Fragmentation:
- `fragCount` = ceil(`width × height × 4` / `MAX_SHAPE_PAYLOAD`). Every fragment except the last carries exactly `MAX_SHAPE_PAYLOAD` bytes.
- Example, with `MAX_SHAPE_PAYLOAD` = 1360: a 32×32 cursor is 4096 bytes, sent as 4 fragments of 1360, 1360, 1360 and 16 bytes.

`shapeId` rules:
- Within one session, a `shapeId` always means the same image. The sender may reuse the id when an earlier image returns, for example when the arrow comes back after the text cursor.
- The sender sends a shape before the first `CURSOR_POS` packet that uses it, and again whenever the receiver requests it.

Receiver:
- Drops a `CURSOR_SHAPE` packet that breaks the size, range or fragmentation rules above, or that disagrees with earlier fragments of the same `shapeId`.
- Reassembles the image like a video frame. It should discard an incomplete shape 500 ms after its first fragment arrived.
- Should cache at least 16 shapes by `shapeId`.

## 6. Control messages

Control messages set up and maintain a session, and carry requests and statistics. `HELLO` and `WELCOME` must not be lost, so they use a small reliability layer of acknowledgements and resends.

### 6.1 Control header

Every control message starts with this header, directly after the common header. The message body follows it.

| Offset | Field             | Type | Description |
|-------:|-------------------|------|-------------|
| 24     | `msgId`           | u32  | Reliable message number, or `0` for unreliable messages; see [6.2](#62-reliable-delivery). |
| 28     | `controlReserved` | u32  | Must be `0`. |

### 6.2 Reliable delivery

`HELLO` and `WELCOME` are **reliable**. All other control messages are unreliable and have `msgId` = `0`.

Each side numbers its own reliable messages: `1` for the first message of a session, then +1 per message. The two directions are independent.

Sender:
- Has at most one unacknowledged reliable message at a time (stop-and-wait). Later reliable messages wait in a queue.
- Resends the message every 100 ms until it receives an `ACK` with that `msgId`. A resent copy has the same `msgId` but a new `seq` and `timestampUs`.
- Keeps resending until the session times out ([6.3](#63-session), step 6). It does not give up earlier, so a short Wi-Fi stall does not end the session.
- Exception: before any session exists, the tablet resends `HELLO` every 500 ms until it receives an `ACK` or a `REJECT`.

Receiver:
- Sends an `ACK` for **every** copy of a reliable message it receives, including duplicates, because an earlier `ACK` may have been lost.
- Processes the message only if its `msgId` is newer than the last `msgId` it processed from that sender, compared as in [2.1](#21-seq). Otherwise it is a duplicate, and is only acknowledged.

### 6.3 Session

A session is one connection between the host and one tablet. In version 1, the host has at most one session at a time.

1. **Start.** The tablet picks a random non-zero `sessionId` and sends `HELLO` with it.
2. **Accept.** When the host receives `HELLO`:
   - If it cannot accept it, the host answers with `REJECT` ([6.5](#65-reject-and-version-stability)) and changes nothing.
   - If its `sessionId` differs from the current session's, the host ends the current session, if any, by sending `BYE` to its tablet (step 7). Then it starts a new session with the new tablet and sends it `ACK` and `WELCOME`.
   - If its `sessionId` is the current one, it is a duplicate and the host only sends `ACK`.
3. **Connected.** When the tablet receives `WELCOME` with its `sessionId`, the session is established. It acknowledges `WELCOME` and does not display H.264 video until the first keyframe.
4. **Streaming.** The host starts streaming only after it receives the `ACK` for `WELCOME`, so no video arrives before the tablet is ready. It sends a keyframe first, then the current cursor shape and `CURSOR_POS`.
5. **Keep-alive.** Both sides send `HEARTBEAT` every 500 ms.
6. **Timeout.** A side that has received no valid packet of the session for 3 s treats the session as lost. The host stops streaming. The tablet starts again from step 1 with a new `sessionId`.
7. **End.** A side that closes the session sends `BYE` 3 times, 20 ms apart. The receiver ends the session at once. A tablet whose session was ended by `BYE` does **not** reconnect automatically. It reconnects only when the user asks, because the host ended the session on purpose (it shut down, or another tablet took over). Automatic reconnection is only for timeouts (step 6).

**Identifying the peer.** A peer is identified by its `sessionId`, not by its address and port:
- Each side drops packets whose `sessionId` is not the current session's. The one exception is that the host accepts `HELLO` with any `sessionId`.
- When a valid packet with the current `sessionId` arrives from a new address or port, the receiver sends to that address from then on. This keeps the session alive when the tablet's IP address changes, for example after a DHCP renewal or when it roams between Wi-Fi access points.

**New session, fresh state.** When a session starts, each side resets all the state it keeps about the peer: `seq` counters and tracking, loss counters, the last processed `msgId`, the reliable send queue, video reassembly ([4.5](#45-reassembly)), the last accepted `CURSOR_POS`, and the shape cache ([5.2](#52-cursor_shape)). Nothing carries over from an earlier session.

**Screen changes.** When the tablet's screen size changes, for example because it was rotated, the tablet ends the session with `BYE` and starts a new one with a `HELLO` that has the new size.

### 6.4 Messages

Offsets continue after the control header (offset 32). Every message has exactly the total size given.

**`HELLO` (0x20)**: tablet → host, reliable. Total 40 bytes.

| Offset | Field       | Type | Description |
|-------:|-------------|------|-------------|
| 32     | `width`     | u16  | Tablet screen width in pixels. |
| 34     | `height`    | u16  | Tablet screen height in pixels. |
| 36     | `refreshHz` | u16  | Tablet screen refresh rate. |
| 38     | `codecs`    | u8   | Supported codecs: bit 0 = JPEG, bit 1 = H.264. Bit 0 must always be set, so the host's CPU fallback (JPEG) always works. Other bits must be `0`. |
| 39     | `reserved`  | u8   | Must be `0`. |

**`WELCOME` (0x21)**: host → tablet, reliable. Total 40 bytes.

| Offset | Field      | Type | Description |
|-------:|------------|------|-------------|
| 32     | `width`    | u16  | Stream width in pixels. Even. |
| 34     | `height`   | u16  | Stream height in pixels. Even. |
| 36     | `fps`      | u16  | Target frame rate. |
| 38     | `codec`    | u8   | Codec the host will send, as in [4.1](#41-fragment-header). It must be one the tablet listed. |
| 39     | `reserved` | u8   | Must be `0`. |

The stream size should equal the size in `HELLO`, but the host may choose a different one. For example, it must round odd sizes to even ones, because H.264 requires even dimensions, and it may pick a smaller size to save bandwidth. If the stream size differs from its screen, the tablet scales the video to fit, and scales cursor coordinates and images by the same factor.

**`ACK` (0x22)**: both directions, unreliable. Total 36 bytes.

| Offset | Field        | Type | Description |
|-------:|--------------|------|-------------|
| 32     | `ackedMsgId` | u32  | `msgId` of the reliable message being acknowledged. |

**`HEARTBEAT` (0x23)**: both directions, unreliable. Total 48 bytes.

| Offset | Field             | Type | Description |
|-------:|-------------------|------|-------------|
| 32     | `echoTimestampUs` | u64  | `timestampUs` of the last `HEARTBEAT` received from the peer, or `0` if none yet. |
| 40     | `echoDelayUs`     | u32  | Microseconds between receiving that `HEARTBEAT` and sending this one. |
| 44     | `reserved`        | u32  | Must be `0`. |

On receiving a `HEARTBEAT` with a non-zero `echoTimestampUs`, a side computes the round-trip time using only its own clock:
`rttUs = nowUs − echoTimestampUs − echoDelayUs`.

**`KEYFRAME_REQUEST` (0x24)**: tablet → host, unreliable. Total 32 bytes, with no body.

The tablet sends it when it is decoding H.264 and loses a frame: it discarded an incomplete frame, saw a gap in delivered `frameId`s, or its decoder reported an error. The host makes its next frame a keyframe. It may skip requests that arrive within 100 ms of a keyframe it already sent.

The request is not acknowledged: its answer is the keyframe. The tablet repeats it every 200 ms until a keyframe arrives, and sends no other `KEYFRAME_REQUEST` in between, even if more frames are lost or fail to decode in the meantime.

**`SHAPE_REQUEST` (0x25)**: tablet → host, unreliable. Total 36 bytes.

| Offset | Field     | Type | Description |
|-------:|-----------|------|-------------|
| 32     | `shapeId` | u32  | Shape the tablet is missing. The host sends that `CURSOR_SHAPE` again. |

The request is not acknowledged: its answer is the shape. The tablet repeats it every 500 ms until the shape arrives, and requests a given `shapeId` at most once per 500 ms, however many `CURSOR_POS` packets refer to it in the meantime.

**`STATS` (0x26)**: tablet → host, unreliable, every 1 s. Total 48 bytes. All counters count from the start of the session and wrap at 2^32.

| Offset | Field             | Type | Description |
|-------:|-------------------|------|-------------|
| 32     | `packetsReceived` | u32  | Valid packets received. |
| 36     | `packetsLost`     | u32  | Packets missing, from gaps in `seq`. |
| 40     | `framesCompleted` | u32  | Video frames fully reassembled. |
| 44     | `framesDropped`   | u32  | Video frames discarded incomplete. |

**`BYE` (0x27)**: both directions, unreliable. Total 32 bytes, with no body.

### 6.5 `REJECT` and version stability

**`REJECT` (0x28)**: host → tablet, unreliable. Total 36 bytes. The host sends it in answer to a `HELLO` it cannot accept, with `sessionId` copied from that `HELLO` and `msgId` = `0`.

| Offset | Field      | Type | Description |
|-------:|------------|------|-------------|
| 32     | `reason`   | u8   | Why the `HELLO` was rejected; see below. |
| 33     | `version`  | u8   | Highest protocol version the host supports. |
| 34     | `reserved` | u16  | Must be `0`. |

| `reason` | Meaning |
|---------:|---------|
| `1` | Unsupported protocol version. |
| `2` | No codec in common. |
| `3` | Invalid parameters, for example a width or height of `0`, or `codecs` without JPEG. |

All other values are reserved. A tablet that receives an unknown `reason` treats it as a rejection anyway.

The host sends at most one `REJECT` per `HELLO` it receives. A tablet that receives a `REJECT` matching its `HELLO`'s `sessionId` stops resending `HELLO` and shows the reason to the user.

**Version stability.** So that a host and a tablet with different protocol versions can still tell each other that they are incompatible, these layouts are frozen and will never change in any version:

- the common header ([2](#2-common-header)), which includes the `type` and `sessionId` of every `HELLO`;
- the whole `REJECT` packet.

A host that receives a `HELLO` with an unsupported `version` reads only its common header and answers with `REJECT` reason `1`. A tablet accepts `REJECT` regardless of its `version` field.

### 6.6 Development mode

Until sessions are implemented, the host can run in **development mode**. It is used for early development and by the debugging tools (debug viewer, test sender):

- It streams `VIDEO_FRAGMENT` and cursor packets with `sessionId` = `0` to a fixed address and port given on the command line, with no session.
- It keeps one `seq` counter for that destination.
- It sends no control messages and ignores any it receives.
- The codec and size are set on the command line.
- The receiver cannot send `KEYFRAME_REQUEST` or `SHAPE_REQUEST`, so the host sends a keyframe and the current cursor shape every 1 s on its own.

Development mode is off by default and must never be enabled by a release build. Receivers in development mode, such as the debug viewer, skip [6.3](#63-session) and accept packets with `sessionId` = `0` from any address.

## 7. Extensions

### 7.1 Rules for changing the protocol

- **New packet types** may be added without changing `version`. Receivers that do not know a type drop it ([1.4](#14-invalid-packets)). So a sender uses a new type only after the peer has said it supports it ([7.2](#72-feature-negotiation)).
- **Reserved fields and bits** must be `0` when sent, and receivers drop packets where they are not. A later revision may give them a meaning. A sender uses that meaning only with a peer that supports it.
- **Any other change** to an existing packet's layout or meaning requires a new `version`.

### 7.2 Feature negotiation

The `reserved` byte at offset 39 of `HELLO` and `WELCOME` will become a `features` bitmask. The tablet lists the features it supports, and the host answers with the subset it will use. Planned bits:

| Bit | Feature |
|----:|---------|
| 0   | Forward error correction ([7.3](#73-forward-error-correction)) |

### 7.3 Forward error correction

Planned packet type `0x02` `VIDEO_PARITY`. It carries the XOR of a group of consecutive fragments of one frame, padded to `MAX_FRAGMENT_PAYLOAD` bytes, so the receiver can rebuild one lost fragment per group. Its exact layout will be specified with the feature. The video fragment format in [4](#4-video-fragments) does not change.

### 7.4 Encryption

Planned as protocol `version` 2:

- The 24-byte common header stays unencrypted and is authenticated as associated data.
- Everything after the common header is encrypted and authenticated with libsodium (XChaCha20-Poly1305). This adds a 24-byte nonce and a 16-byte tag, so a packet can be up to 1440 bytes, which is within the margin in [1.3](#13-packet-size).
- The layouts after the common header stay the same, including fragment sizes.
- Pairing (QR code and key exchange) will be specified with the feature.

### 7.5 Discovery

Planned packet types `0x30` `DISCOVER` (tablet broadcast) and `0x31` `ANNOUNCE` (host reply with its name and port), on the fixed port `DISCOVERY_PORT`. They are sent before any session exists, so they have no control header, use `sessionId` = `0`, and are never encrypted. Their layout will be specified with the feature.

### 7.6 Out of scope

The local control port between the host and the tray app is not part of this protocol. It will have its own document.

## 8. Security

Version 1 has **no authentication and no encryption**. Anyone on the same network can:

- watch the stream by capturing its packets;
- send forged packets, including a `REJECT` that stops a tablet from connecting;
- take over the host with their own `HELLO`, which ends the current session ([6.3](#63-session));
- redirect the stream to themselves by sending a packet with the current `sessionId`, which they can read from any captured packet ([6.3](#63-session), identifying the peer).

This is accepted for version 1, which is meant for trusted home networks. Pairing and encryption ([7.4](#74-encryption)) remove these risks in version 2. Until then, the host should listen only on the network interface the user chooses.
