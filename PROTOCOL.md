# Nixani Engine protocol (version 1)

The engine is a program without a window. A client program starts it and talks to it through the
engine's standard input and standard output. Any program in any language can be a client; the
header [`protocol/nxe_protocol.h`](protocol/nxe_protocol.h) with the message numbers and a small
reader / writer is MIT-licensed for that purpose.

## Frames

Both directions carry a stream of frames, little-endian, no padding:

| Field | Type | Meaning |
|---|---|---|
| size | u32 | number of bytes that follow (type + payload) |
| type | u16 | message number |
| payload | | the fields of that message, in order |

Field types: `u8` `u16` `u32` `u64` `i32` `f32`; `str` = u32 length + UTF-8 bytes; `bytes N` =
exactly N bytes; `blob` = everything up to the end of the frame. Times are
`QueryPerformanceCounter` ticks (one clock for both processes).

The engine ends when its input is closed. Standard error is not used.

## Client to engine

| # | Message | Payload | Effect |
|---|---|---|---|
| 1 | Hello | u32 protocolVersion | answered with HelloReply |
| 10 | DiscoveryStart | | consoles on the local network are reported with Hosts |
| 11 | DiscoveryStop | | |
| 12 | Wake | str address, bytes 16 registKey, u8 ps5 | wakes a console from rest mode |
| 20 | RegistStart | str address, u32 target, bytes 8 accountId, u32 linkCode, u32 consolePin | links with a console; answered with RegistResult |
| 21 | RegistCancel | | |
| 30 | ListDevices | u32 requestId, str psnToken | consoles on a PSN account; answered with DevicesResult |
| 31 | NewDeviceUid | u32 requestId | a new client device id for the PSN sign-in; answered with DeviceUid |
| 40 | SessionStart | SessionParams (below) | starts a stream; progress arrives as SessionState |
| 41 | SessionStop | | ends the stream; answered with SessionStopped |
| 42 | Pad | PadState (below) | one controller state; each message is one update to the console |
| 43 | LoginPin | str pin | the console's login PIN, after LoginPinRequest |
| 44 | RequestKeyframe | | asks the console for a fresh picture (after a frame could not be decoded) |
| 50 | Ping | u64 clientTime, u32 echoBytes | answered with Pong |

**SessionParams**: str host, u8 ps5, bytes 16 registKey, bytes 16 rpKey, bytes 8 accountId,
u32 resolution (360 / 540 / 720 / 1080), u32 fps (30 / 60), u32 bitrateKbps (0 = default),
u8 codec (0 H.264, 1 HEVC, 2 HEVC HDR10), u8 dualsense, u8 internet, str psnToken,
bytes 32 deviceUid. With `internet` = 1 the connection goes through PSN (token and deviceUid are
needed, host is not).

**PadState**: u32 buttons, u8 l2, u8 r2, i16 leftX, i16 leftY, i16 rightX, i16 rightY,
2 x (i8 touchId, u16 x, u16 y), u8 hasMotion, f32 gyro[3] (rad/s), f32 accel[3] (g),
u64 motionTimeUs. A touch id of -1 means no finger in that slot; touch coordinates run to
1919 x 1079. Button bits, from bit 0: dpad up, down, left, right, cross, circle, square,
triangle, L1, R1, L3, R3, share, options, touchpad, PS.

## Engine to client

| # | Message | Payload |
|---|---|---|
| 101 | HelloReply | u32 protocolVersion, str engineVersion |
| 102 | Log | u8 level (0 error, 1 warning, 2 info, 3 debug), str text |
| 110 | Hosts | u32 count, then per console: u8 state (0 unknown, 1 on, 2 rest mode), str name, str address, str hostId, str systemVersion, str runningApp, u8 ps5, u32 target |
| 120 | RegistResult | u8 ok, str error; if ok: str nickname, bytes 6 mac, u32 target, bytes 16 registKey, bytes 16 rpKey, u32 rpKeyType, u32 consolePin |
| 130 | DevicesResult | u32 requestId, str error, u32 count, then per device: str name, bytes 32 uid, u8 ps5, u8 remotePlayEnabled |
| 131 | DeviceUid | u32 requestId, str uid |
| 140 | SessionState | u8 state (1 connecting, 2 streaming, 3 ended), u8 isError, str text |
| 141 | Video | u64 engineTime, i32 framesLost, u8 recovered, blob: one complete compressed frame (H.264 / HEVC, Annex B) |
| 142 | AudioFormat | u32 channels, u32 sampleRate |
| 143 | Audio | blob: interleaved signed 16-bit samples |
| 144 | Haptics | blob: 3 kHz stereo signed 16-bit samples for the controller's actuators |
| 145 | Rumble | u8 left, u8 right |
| 146 | TriggerEffects | u8 typeLeft, u8 typeRight, bytes 10 left, bytes 10 right |
| 147 | Intensity | u8 haptics, u8 triggers (0 off, 1 weak, 2 medium, 3 strong) |
| 148 | LoginPinRequest | u8 previousPinWasWrong |
| 149 | Nickname | str name |
| 150 | CantDisplay | u8 on (the console shows content it does not stream) |
| 151 | Stats | u64 packetsReceived, u64 packetsLost (since the last Stats), u32 rttMicroseconds, u64 fecFailures, u64 keyframeRequests (since the start) |
| 152 | SessionStopped | |
| 160 | Pong | u64 clientTime, u64 engineTime, blob: echoBytes zero bytes |

## A stream, step by step

1. `Hello`, wait for `HelloReply`.
2. `DiscoveryStart`; pick a console from `Hosts`. If it is in rest mode, `Wake` it and wait until
   `Hosts` shows it on.
3. Once per console: `RegistStart` with the 8-digit code the console shows; keep the keys from
   `RegistResult`.
4. `SessionStart`. `SessionState` reports connecting, then streaming. From then on `Video`,
   `Audio` and the feedback messages arrive, and every `Pad` message is sent to the console.
5. `SessionStop`, wait for `SessionStopped`. A session that ended by itself (`SessionState` 3)
   is stopped the same way before the next one starts.

The engine decodes no video and opens no audio device or controller: that is the client's part.
