// SPDX-License-Identifier: MIT
//
// Nixani Engine protocol, version 1.
//
// Copyright (c) 2026 Nixani
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this file, to
// deal in it without restriction, including without limitation the rights to use, copy, modify,
// merge, publish, distribute, sublicense, and/or sell copies of it, subject to the following
// conditions: the above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the file. THE FILE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
// ANY KIND.
//
// This header only describes the messages a client and the engine exchange (see PROTOCOL.md).
// It contains no engine code. It is MIT-licensed so that any program, under any licence, can
// speak the protocol; the engine itself is AGPL-3.0.
//
// Transport: the client starts the engine and talks to it over the engine's standard input
// (client -> engine) and standard output (engine -> client). Both carry a stream of frames:
//
//     u32 size      number of bytes that follow (type + payload)
//     u16 type      one of nxe::Msg
//     ...  payload  fields in the order listed at each message, little-endian, no padding
//
// Field types: u8 / u16 / u32 / u64 / i32 / f32; `str` = u32 length + UTF-8 bytes; `bytes N` =
// exactly N bytes; `blob` = everything up to the end of the frame.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace nxe {

constexpr uint32_t kProtocolVersion = 1;
constexpr uint32_t kMaxFrame = 16u * 1024 * 1024; // a frame larger than this is a broken stream

enum class Msg : uint16_t {
	// ---- client -> engine ----
	Hello = 1,           // u32 protocolVersion
	DiscoveryStart = 10, // (none)  consoles on the local network are reported with Hosts until stopped
	DiscoveryStop = 11,  // (none)
	Wake = 12,           // str address, bytes 16 registKey, u8 ps5
	RegistStart = 20,    // str address, u32 target, bytes 8 accountId, u32 linkCode, u32 consolePin
	RegistCancel = 21,   // (none)
	ListDevices = 30,    // u32 requestId, str psnToken        -> DevicesResult
	NewDeviceUid = 31,   // u32 requestId                      -> DeviceUid
	SessionStart = 40,   // see SessionParams
	SessionStop = 41,    // (none)                             -> SessionStopped
	Pad = 42,            // see PadState
	LoginPin = 43,       // str pin
	RequestKeyframe = 44, // (none)
	Ping = 50,           // u64 clientTime, u32 echoBytes      -> Pong

	// ---- engine -> client ----
	HelloReply = 101,     // u32 protocolVersion, str engineVersion
	Log = 102,            // u8 level (0 error, 1 warning, 2 info, 3 debug), str text
	Hosts = 110,          // u32 count, then per console: u8 state (0 unknown, 1 on, 2 rest mode), str name, str address,
	                      //   str hostId, str systemVersion, str runningApp, u8 ps5, u32 target
	RegistResult = 120,   // u8 ok, str error, then if ok: str nickname, bytes 6 mac, u32 target, bytes 16 registKey,
	                      //   bytes 16 rpKey, u32 rpKeyType, u32 consolePin
	DevicesResult = 130,  // u32 requestId, str error, u32 count, then per device: str name, bytes 32 uid, u8 ps5, u8 remotePlayEnabled
	DeviceUid = 131,      // u32 requestId, str uid
	SessionState = 140,   // u8 state (SessionPhase), u8 isError, str text
	Video = 141,          // u64 engineTime, i32 framesLost, u8 recovered, blob: one complete compressed frame
	AudioFormat = 142,    // u32 channels, u32 sampleRate
	Audio = 143,          // blob: interleaved signed 16-bit samples
	Haptics = 144,        // blob: 3 kHz stereo signed 16-bit samples for the controller's actuators
	Rumble = 145,         // u8 left, u8 right
	TriggerEffects = 146, // u8 typeLeft, u8 typeRight, bytes 10 left, bytes 10 right
	Intensity = 147,      // u8 haptics, u8 triggers   (0 off, 1 weak, 2 medium, 3 strong)
	LoginPinRequest = 148, // u8 previousPinWasWrong
	Nickname = 149,       // str name
	CantDisplay = 150,    // u8 on: the console shows content it does not stream
	Stats = 151,          // u64 packetsReceived, u64 packetsLost (both since the last Stats), u32 rttMicroseconds,
	                      //   u64 fecFailures, u64 keyframeRequests (both since the session started)
	SessionStopped = 152, // (none)
	Pong = 160,           // u64 clientTime, u64 engineTime, blob: echoBytes zero bytes
};

// SessionState.state
enum class SessionPhase : uint8_t {
	Connecting = 1, // text = what is happening (internet play has several steps)
	Streaming = 2,
	Ended = 3,      // text = why; isError tells a failure from a normal end
};

// Times are QueryPerformanceCounter ticks: the same clock in both processes.

// Controller buttons (PadState.buttons).
enum PadButton : uint32_t {
	kDpadUp = 1u << 0,
	kDpadDown = 1u << 1,
	kDpadLeft = 1u << 2,
	kDpadRight = 1u << 3,
	kCross = 1u << 4,
	kCircle = 1u << 5,
	kSquare = 1u << 6,
	kTriangle = 1u << 7,
	kL1 = 1u << 8,
	kR1 = 1u << 9,
	kL3 = 1u << 10,
	kR3 = 1u << 11,
	kShare = 1u << 12,
	kOptions = 1u << 13,
	kTouchpad = 1u << 14,
	kPs = 1u << 15,
};

// Touchpad coordinates run from 0 to these.
constexpr uint16_t kTouchpadMaxX = 1919, kTouchpadMaxY = 1079;

struct PadTouch {
	int8_t id = -1; // -1 = no finger in this slot; otherwise 0..127, a new number for every new touch
	uint16_t x = 0, y = 0;
};

// One controller state. Every Pad message becomes one update to the console, in order.
// Wire layout: u32 buttons, u8 l2, u8 r2, i16 leftX, i16 leftY, i16 rightX, i16 rightY,
//   2 x (i8 id, u16 x, u16 y), u8 hasMotion, f32 gyro[3], f32 accel[3], u64 motionTimeUs
struct PadState {
	uint32_t buttons = 0;
	uint8_t l2 = 0, r2 = 0;
	int16_t leftX = 0, leftY = 0, rightX = 0, rightY = 0;
	PadTouch touches[2];
	// A motion sensor sample (gyro rad/s, accelerometer in g, its time in microseconds). The engine
	// turns the samples into the orientation the console wants. hasMotion = 0: no new sample.
	bool hasMotion = false;
	float gyro[3] = {0, 0, 0};
	float accel[3] = {0, 1, 0};
	uint64_t motionTimeUs = 0;
};

// SessionStart payload, in this order.
struct SessionParams {
	std::string host;          // address on the local network (ignored for internet play)
	bool ps5 = true;
	uint8_t registKey[16] = {}; // from RegistResult
	uint8_t rpKey[16] = {};
	uint8_t accountId[8] = {};
	uint32_t resolution = 1080; // 360, 540, 720 or 1080
	uint32_t fps = 60;          // 30 or 60
	uint32_t bitrateKbps = 0;   // 0 = the default for the resolution
	uint8_t codec = 0;          // 0 H.264, 1 HEVC, 2 HEVC HDR10
	bool dualsense = true;      // ask for haptics and adaptive trigger data
	bool internet = false;      // connect through PSN instead of the local network
	std::string psnToken;       // internet play: Remote Play access token
	uint8_t deviceUid[32] = {}; // internet play: the console's uid from DevicesResult
};

// ---------------------------------------------------------------------------------------------
// Reading and writing payloads.

class Writer {
public:
	explicit Writer(Msg type)
	{
		data_.resize(6);
		const uint16_t t = static_cast<uint16_t>(type);
		std::memcpy(data_.data() + 4, &t, 2);
	}
	Writer &u8(uint8_t v) { return raw(&v, 1); }
	Writer &u16(uint16_t v) { return raw(&v, 2); }
	Writer &u32(uint32_t v) { return raw(&v, 4); }
	Writer &u64(uint64_t v) { return raw(&v, 8); }
	Writer &i32(int32_t v) { return raw(&v, 4); }
	Writer &f32(float v) { return raw(&v, 4); }
	Writer &str(const std::string &s) { return u32(static_cast<uint32_t>(s.size())).raw(s.data(), s.size()); }
	Writer &raw(const void *p, size_t n)
	{
		const auto *b = static_cast<const uint8_t *>(p);
		data_.insert(data_.end(), b, b + n);
		return *this;
	}
	// The complete frame, ready to be written in one piece.
	const std::vector<uint8_t> &frame()
	{
		const uint32_t size = static_cast<uint32_t>(data_.size() - 4);
		std::memcpy(data_.data(), &size, 4);
		return data_;
	}

private:
	std::vector<uint8_t> data_;
};

class Reader {
public:
	Reader(const uint8_t *p, size_t n) : p_(p), end_(p + n) {}
	bool ok() const { return ok_; }
	size_t left() const { return static_cast<size_t>(end_ - p_); }
	const uint8_t *here() const { return p_; }
	uint8_t u8() { return get<uint8_t>(); }
	uint16_t u16() { return get<uint16_t>(); }
	uint32_t u32() { return get<uint32_t>(); }
	uint64_t u64() { return get<uint64_t>(); }
	int32_t i32() { return get<int32_t>(); }
	float f32() { return get<float>(); }
	std::string str()
	{
		const uint32_t n = u32();
		if(!ok_ || n > left())
		{
			ok_ = false;
			return {};
		}
		std::string s(reinterpret_cast<const char *>(p_), n);
		p_ += n;
		return s;
	}
	void raw(void *out, size_t n)
	{
		if(n > left())
		{
			ok_ = false;
			std::memset(out, 0, n);
			return;
		}
		std::memcpy(out, p_, n);
		p_ += n;
	}

private:
	template <typename T> T get()
	{
		T v{};
		raw(&v, sizeof(T));
		return v;
	}
	const uint8_t *p_, *end_;
	bool ok_ = true;
};

inline void WritePad(Writer &w, const PadState &s)
{
	w.u32(s.buttons).u8(s.l2).u8(s.r2).u16(static_cast<uint16_t>(s.leftX)).u16(static_cast<uint16_t>(s.leftY));
	w.u16(static_cast<uint16_t>(s.rightX)).u16(static_cast<uint16_t>(s.rightY));
	for(const PadTouch &t : s.touches)
		w.u8(static_cast<uint8_t>(t.id)).u16(t.x).u16(t.y);
	w.u8(s.hasMotion ? 1 : 0);
	for(float v : s.gyro)
		w.f32(v);
	for(float v : s.accel)
		w.f32(v);
	w.u64(s.motionTimeUs);
}

inline PadState ReadPad(Reader &r)
{
	PadState s;
	s.buttons = r.u32();
	s.l2 = r.u8();
	s.r2 = r.u8();
	s.leftX = static_cast<int16_t>(r.u16());
	s.leftY = static_cast<int16_t>(r.u16());
	s.rightX = static_cast<int16_t>(r.u16());
	s.rightY = static_cast<int16_t>(r.u16());
	for(PadTouch &t : s.touches)
	{
		t.id = static_cast<int8_t>(r.u8());
		t.x = r.u16();
		t.y = r.u16();
	}
	s.hasMotion = r.u8() != 0;
	for(float &v : s.gyro)
		v = r.f32();
	for(float &v : s.accel)
		v = r.f32();
	s.motionTimeUs = r.u64();
	return s;
}

inline void WriteSessionParams(Writer &w, const SessionParams &p)
{
	w.str(p.host).u8(p.ps5).raw(p.registKey, 16).raw(p.rpKey, 16).raw(p.accountId, 8);
	w.u32(p.resolution).u32(p.fps).u32(p.bitrateKbps).u8(p.codec).u8(p.dualsense).u8(p.internet);
	w.str(p.psnToken).raw(p.deviceUid, 32);
}

inline SessionParams ReadSessionParams(Reader &r)
{
	SessionParams p;
	p.host = r.str();
	p.ps5 = r.u8() != 0;
	r.raw(p.registKey, 16);
	r.raw(p.rpKey, 16);
	r.raw(p.accountId, 8);
	p.resolution = r.u32();
	p.fps = r.u32();
	p.bitrateKbps = r.u32();
	p.codec = r.u8();
	p.dualsense = r.u8() != 0;
	p.internet = r.u8() != 0;
	p.psnToken = r.str();
	r.raw(p.deviceUid, 32);
	return p;
}

} // namespace nxe
