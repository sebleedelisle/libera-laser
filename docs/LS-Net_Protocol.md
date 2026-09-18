# LS-Net Protocol Summary

**Protocol version:** `0x0001`  
**Network port:** `25555` for both UDP and TCP  
**Byte order:** Big-endian throughout

> This document separates the official LS-Net protocol from behaviour inferred from the supplied Libera implementation. Notes labelled **Libera implementation observation** are not formal requirements of the LS-Net specification.

## Overview

LS-Net uses UDP broadcast to discover Lightspace controllers. Once a controller has been discovered, the supplied Libera implementation opens a TCP connection to the controller for control commands, heartbeats, acknowledgments, and complete point-pattern uploads.

```text
UDP broadcast discovery on port 25555
                |
                v
Controller IP and identity discovered
                |
                v
TCP connection to controller-IP:25555
                |
                +-- Laser on/off commands, acknowledged
                +-- Scan-frequency commands, acknowledged
                +-- Heartbeat once per second
                +-- Complete point-pattern packets, not acknowledged
```

The official protocol document states that both TCP and UDP use port `25555`. It explicitly identifies the discovery query as a UDP broadcast. The supplied controller implementation uses TCP for connected communication.

---

## 1. Discovery

### Broadcast query

The host sends a **Basic Class / Broadcast Query** packet as a UDP broadcast to port `25555`.

The payload contains one fixed byte:

```text
FF
```

The complete example packet from the protocol document is:

```text
4C 49 47 48 54 53 50 41 43 45   "LIGHTSPACE"
00 15                              Total packet length: 21 bytes
00 01                              Protocol version: 1
01                                 Packet type: Basic Class
01                                 Command: Broadcast Query
00 01                              Payload length: 1 byte
FF                                 Query payload
87 B2                              CRC-16
```

### Broadcast response

A controller responds with a **Basic Class / Broadcast Response** packet containing:

| Field | Size | Description |
|---|---:|---|
| Device ID | 4 bytes | Controller identifier |
| Firmware version | 2 bytes | Firmware version |
| Hardware version | 2 bytes | Hardware version |
| IP address | 4 bytes | Controller IPv4 address |
| MAC address | 6 bytes | Controller MAC address |
| Device name | Variable | Device name string |

All multi-byte fields are big-endian.

The fixed part of the response payload is 18 bytes. There is no separate device-name length field, so the name length must be inferred from the remaining payload length.

### Discovery details not defined by the protocol

The official document does not clearly specify:

- Whether a discovery response is unicast to the requesting host or broadcast.
- Which local UDP port the host should bind.
- Whether discovery should use `255.255.255.255`, a directed subnet broadcast, or both.
- The encoding or termination rules for the device name.
- Controller behaviour when DHCP is unavailable.

The ML-S hardware manual shows the controller acquiring an address through DHCP, starting UDP, and starting a TCP server on port `25555`.

### Libera implementation observation

The supplied Libera headers use the following discovery policy:

| Setting | Value |
|---|---:|
| UDP receive timeout | 100 ms |
| Probe interval during a discovery burst | 300 ms |
| Discovery listening window | 1.2 seconds |
| Idle interval between background scans | 5 seconds |
| Remove a controller not seen for | 15 seconds |

These timings are implementation choices rather than LS-Net protocol requirements.

---

## 2. Connected communication

After discovery, the supplied Libera implementation opens a TCP connection to:

```text
controller-ip-address:25555
```

The protocol is host-driven:

1. The host sends commands.
2. The controller executes them.
3. Normal control commands return an acknowledgment.
4. Point-pattern packets do not require acknowledgment.
5. Heartbeats are used to detect whether the controller is still connected.

### TCP packet framing

TCP is a byte stream, so one TCP read is not guaranteed to contain exactly one LS-Net packet. A receiver should:

1. Accumulate incoming bytes in a persistent buffer.
2. Check for the `LIGHTSPACE` header.
3. Read the two-byte total packet length.
4. Wait until the complete packet is present.
5. Validate the payload length and CRC.
6. Extract the packet and retain any following bytes for the next packet.

The supplied Libera controller includes a persistent TCP receive buffer for this purpose.

### Reliable command behaviour

For acknowledged control commands:

1. The host sends the command.
2. It waits up to 100 ms for an acknowledgment.
3. If no acknowledgment arrives, it retransmits.
4. It stops after three total attempts, or 300 ms in total.

Available control commands are:

| Packet type | Command | Purpose | Payload |
|---|---:|---|---|
| Command Class `0x02` | `0x02` | Laser on/off | `0x01` on, `0x02` off |
| Command Class `0x02` | `0x03` | Set scan frequency | One byte, 1 to 100 kHz |

### Command acknowledgment

The acknowledgment packet is:

```text
Packet type:  Command Class 0x02
Command:      Command Acknowledgment 0x01
Payload:      Two-byte acknowledged command identifier
```

The official example gives `0x0202` as the acknowledgment value for the Laser On/Off command.

This strongly suggests that the acknowledgment payload is:

```text
[original packet type][original command word]
```

For example:

```text
02 02   Acknowledges Command Class / Laser On-Off
02 03   Presumably acknowledges Command Class / Set Scan Frequency
```

The document calls this field the "Acknowledged Command Word", but it is two bytes and does not explicitly define the byte-level interpretation apart from the `0x0202` example.

---

## 3. Heartbeat and disconnection

The host sends a heartbeat once per second.

### Heartbeat query

```text
Packet type:  Basic Class 0x01
Command:      Heartbeat Query 0x03
Payload:      8-byte host runtime timestamp in milliseconds
```

### Heartbeat response

```text
Packet type:  Basic Class 0x01
Command:      Heartbeat Response 0x04
Payload:      8-byte controller runtime timestamp in milliseconds
```

If the host receives no response within one second, it retransmits. After three attempts, or three seconds without a response, the controller is considered disconnected and discovery should restart.

The heartbeat is primarily a liveness check. It is not a complete clock-synchronisation mechanism because:

- The response contains the controller's runtime timestamp rather than echoing the host timestamp.
- No clock relationship is defined.
- There is no sequence number.
- No offset or round-trip calculation is specified.

---

## 4. Common packet structure

Every LS-Net packet uses the same envelope:

| Offset | Size | Field | Description |
|---:|---:|---|---|
| 0 | 10 bytes | Protocol header | ASCII `LIGHTSPACE` |
| 10 | 2 bytes | Packet length | Total length of the complete packet |
| 12 | 2 bytes | Version | Current documented version is `0x0001` |
| 14 | 1 byte | Packet type | Basic or Command Class |
| 15 | 1 byte | Command word | Command within the selected class |
| 16 | 2 bytes | Data length | Number of payload bytes |
| 18 | N bytes | Data content | Command-specific payload |
| 18 + N | 2 bytes | CRC-16 | CRC of every preceding byte |

The total packet size is:

```text
total packet length = 20 + payload length
```

The minimum packet size, with an empty payload, would therefore be 20 bytes.

### Packet types

| Value | Meaning |
|---:|---|
| `0x01` | Basic Class |
| `0x02` | Command Class |

A command number only has meaning in combination with its packet type. For example, command `0x01` means Broadcast Query under Basic Class, but Command Acknowledgment under Command Class.

---

## 5. Point-pattern transmission

Live output uses:

```text
Packet type:  Basic Class 0x01
Command:      Point-stream Transmission 0x10
```

The payload is:

```text
uint16 pointCount

repeated pointCount times:
    uint16 X
    uint16 Y
    uint8  R
    uint8  G
    uint8  B
```

Each point occupies exactly seven bytes.

Therefore:

```text
payload length      = 2 + 7 * pointCount
total packet length = 22 + 7 * pointCount
```

The official description calls this the total number of points in the **current pattern**. This suggests that each point-stream packet represents a complete current pattern rather than a numbered subsection of a continuous sample stream.

No acknowledgment is required for a point-stream packet.

### Undefined point-stream details

The official protocol does not define:

- Whether X and Y are signed or unsigned.
- The coordinate range.
- Axis orientation or inversion.
- Explicit blanking semantics other than RGB values.
- A frame number or sequence number.
- A presentation timestamp.
- A point-buffer fill level.
- Flow control or backpressure.
- A maximum point count or packet size.
- What the controller does if patterns arrive faster or slower than the configured scan frequency.

The protocol's general big-endian rule implies that X and Y are transmitted most-significant byte first, but it does not establish their signedness or range.

### Libera implementation observation: coordinate encoding

The supplied Libera implementation defaults to:

```text
Signed 16-bit coordinates
Big-endian byte order
No axis inversion
No axis swap
Scale 1.0
Zero offset
```

It also supports alternative signed and unsigned 12-bit, 15-bit, and 16-bit interpretations. This indicates that the exact coordinate convention is not clearly established by the official document.

### Libera implementation observation: practical packet limit

The supplied Libera configuration records hardware probing that found:

- A 5,118-byte packet was accepted.
- A 5,125-byte packet caused the current firmware to stop responding.
- This is consistent with an internal packet limit close to 5,120 bytes.
- Repeated playback close to that boundary was unreliable.
- Libera therefore uses an operational maximum of 700 points.

At 700 points:

```text
payload length      = 2 + 7 * 700 = 4,902 bytes
total packet length = 4,922 bytes
```

The measured theoretical maximum below a 5,120-byte packet boundary is 728 points:

```text
total packet length = 22 + 7 * 728 = 5,118 bytes
```

This limit is based on implementation probing and is not stated in the official LS-Net protocol.

The supplied Libera configuration sends complete patterns at a default interval of 40 ms, equivalent to 25 pattern uploads per second.

---

## 6. CRC-16

The final two bytes contain a CRC calculated over every preceding byte, starting with the first byte of `LIGHTSPACE` and ending with the final payload byte.

CRC parameters:

| Parameter | Value |
|---|---|
| Algorithm | CRC-16/CCITT |
| Polynomial | `0x1021` |
| Initial value | `0xFFFF` |
| Input reflection | No |
| Output reflection | No |
| Final XOR | `0x0000` |

The resulting 16-bit value is appended in big-endian order.

For the documented broadcast query packet, the CRC is:

```text
CRC value:       0x87B2
Transmitted as:  87 B2
```

---

## 7. Compact command reference

| Packet type | Command | Direction | Purpose | Payload |
|---|---:|---|---|---|
| Basic `0x01` | `0x01` | Host to controller | Broadcast query | Fixed byte `FF` |
| Basic `0x01` | `0x02` | Controller to host | Broadcast response | ID, versions, IP, MAC, name |
| Basic `0x01` | `0x03` | Host to controller | Heartbeat query | Host runtime, 8 bytes |
| Basic `0x01` | `0x04` | Controller to host | Heartbeat response | Controller runtime, 8 bytes |
| Basic `0x01` | `0x10` | Host to controller | Complete current pattern | Point count plus `n x 7` point data |
| Command `0x02` | `0x01` | Controller to host | Command acknowledgment | Two-byte command identifier |
| Command `0x02` | `0x02` | Host to controller | Laser on/off | One byte |
| Command `0x02` | `0x03` | Host to controller | Scan frequency | One byte in kHz |

---

## 8. Minimal implementation sequence

```text
1. Open a UDP discovery socket.
2. Broadcast the LS-Net query packet to port 25555.
3. Receive and validate Broadcast Response packets.
4. Parse each controller's ID, versions, IP, MAC, and name.
5. Open a TCP connection to the selected controller on port 25555.
6. Send scan-frequency and laser-state commands.
7. Wait for command acknowledgments and retry after 100 ms if required.
8. Send complete point-pattern packets without waiting for acknowledgments.
9. Send a heartbeat once per second.
10. Parse TCP as a byte stream using the packet-length field.
11. Treat the controller as disconnected after three seconds without a heartbeat response.
12. Close the TCP connection and return to UDP discovery.
```

---

## Sources

Official documents:

- `LS-Net_Protocol_EN.pdf`
- `CRC_Checksum_EN.pdf`
- `ML-S_User_Manual_EN.pdf`

Supplied Libera implementation headers:

- `LightSpaceNetConfig.hpp`
- `LightSpaceNetManager.hpp`
- `LightSpaceNetController.hpp`
- `LightSpaceNetControllerInfo.hpp`
- `LightSpaceNetPacket.hpp`
- `LightSpaceNetStatus.hpp`
