// SPDX-License-Identifier: AGPL-3.0-only
//
// Nixani Engine: a Remote Play engine for PS4 / PS5 consoles, built on chiaki-ng's libchiaki.
//
// It finds consoles, links with them, wakes them and runs the Remote Play session (on the local
// network or through PSN), and hands everything to the program that started it as plain messages
// on its standard output: compressed video frames, decoded audio, haptics, console events. It takes
// commands and controller states on its standard input. It decodes no video and draws nothing.
// The messages are described in PROTOCOL.md and protocol/nxe_protocol.h.
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU Affero General Public License version 3 as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See LICENSE.

#include "../protocol/nxe_protocol.h"

#include <chiaki/base64.h>
#include <chiaki/common.h>
#include <chiaki/controller.h>
#include <chiaki/discoveryservice.h>
#include <chiaki/log.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/opusencoder.h>
#include <chiaki/orientation.h>
#include <chiaki/regist.h>
#include <chiaki/remote/holepunch.h>
#include <chiaki/session.h>
#include <chiaki/videoreceiver.h>

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <avrt.h>
#include <iphlpapi.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef NXE_VERSION
#define NXE_VERSION "0.0.0"
#endif

namespace {

using nxe::Msg;
using nxe::Reader;
using nxe::Writer;

// ------------------------------------------------------------------------------------------------
// Output: every thread sends whole frames through here.

HANDLE gOut = INVALID_HANDLE_VALUE;
std::mutex gOutMutex;
ChiakiLog gLog{};

void Send(Writer &w)
{
	const std::vector<uint8_t> &frame = w.frame();
	std::lock_guard lock(gOutMutex);
	size_t done = 0;
	while(done < frame.size())
	{
		DWORD n = 0;
		if(!WriteFile(gOut, frame.data() + done, static_cast<DWORD>(frame.size() - done), &n, nullptr) || n == 0)
			ExitProcess(0); // the client is gone
		done += n;
	}
}

void Send(Msg type)
{
	Writer w(type);
	Send(w);
}

enum LogLevel : uint8_t { kError = 0, kWarning = 1, kInfo = 2, kDebug = 3 };

void Log(LogLevel level, const char *fmt, ...)
{
	char text[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	Writer w(Msg::Log);
	w.u8(level).str(text);
	Send(w);
}

void OnChiakiLog(ChiakiLogLevel level, const char *msg, void *)
{
	Writer w(Msg::Log);
	w.u8(level == CHIAKI_LOG_ERROR ? kError : level == CHIAKI_LOG_WARNING ? kWarning : level == CHIAKI_LOG_INFO ? kInfo : kDebug);
	w.str(std::string("[chiaki] ") + msg);
	Send(w);
}

int64_t Qpc()
{
	LARGE_INTEGER v;
	QueryPerformanceCounter(&v);
	return v.QuadPart;
}

// The receive thread carries the video and audio to the client: give it MMCSS "Games" scheduling
// (once per thread).
void BoostCurrentThread()
{
	thread_local bool boosted = false;
	if(boosted)
		return;
	boosted = true;
	DWORD task = 0;
	AvSetMmThreadCharacteristicsW(L"Games", &task);
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}

// ------------------------------------------------------------------------------------------------
// Discovery and wake-up

ChiakiDiscoveryService gDiscovery{};
bool gDiscoveryRunning = false;

// Directed broadcast address (addr | ~mask) of every up Ethernet / Wi-Fi adapter.
std::vector<sockaddr_storage> SubnetBroadcasts()
{
	std::vector<sockaddr_storage> out;
	ULONG size = 0;
	if(GetAdaptersInfo(nullptr, &size) != ERROR_BUFFER_OVERFLOW)
		return out;
	std::unique_ptr<uint8_t[]> buf(new uint8_t[size]);
	auto *info = reinterpret_cast<IP_ADAPTER_INFO *>(buf.get());
	if(GetAdaptersInfo(info, &size) != NO_ERROR)
		return out;
	for(auto *a = info; a; a = a->Next)
	{
		if(a->Type != MIB_IF_TYPE_ETHERNET && a->Type != IF_TYPE_IEEE80211)
			continue;
		for(auto *ip = &a->IpAddressList; ip; ip = ip->Next)
		{
			in_addr addr{}, mask{};
			if(inet_pton(AF_INET, ip->IpAddress.String, &addr) != 1 || inet_pton(AF_INET, ip->IpMask.String, &mask) != 1 || addr.s_addr == 0)
				continue;
			sockaddr_storage ss{};
			auto *sin = reinterpret_cast<sockaddr_in *>(&ss);
			sin->sin_family = AF_INET;
			sin->sin_addr.s_addr = addr.s_addr | ~mask.s_addr;
			out.push_back(ss);
		}
	}
	return out;
}

void OnHosts(ChiakiDiscoveryHost *hosts, size_t count, void *)
{
	auto str = [](const char *s) { return s ? std::string(s) : std::string(); };
	Writer w(Msg::Hosts);
	w.u32(static_cast<uint32_t>(count));
	for(size_t i = 0; i < count; ++i)
	{
		ChiakiDiscoveryHost &h = hosts[i];
		w.u8(h.state == CHIAKI_DISCOVERY_HOST_STATE_READY ? 1 : h.state == CHIAKI_DISCOVERY_HOST_STATE_STANDBY ? 2 : 0);
		w.str(str(h.host_name)).str(str(h.host_addr)).str(str(h.host_id)).str(str(h.system_version)).str(str(h.running_app_name));
		w.u8(chiaki_discovery_host_is_ps5(&h) ? 1 : 0);
		w.u32(static_cast<uint32_t>(chiaki_discovery_host_system_version_target(&h)));
	}
	Send(w);
}

void DiscoveryStart()
{
	if(gDiscoveryRunning)
		return;
	sockaddr_storage sendAddr{};
	auto *sin = reinterpret_cast<sockaddr_in *>(&sendAddr);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = INADDR_BROADCAST;
	std::vector<sockaddr_storage> broadcasts = SubnetBroadcasts();

	ChiakiDiscoveryServiceOptions opts{};
	opts.ping_ms = 500;
	opts.hosts_max = 16;
	opts.host_drop_pings = 3;
	opts.send_addr = &sendAddr;
	opts.send_addr_size = sizeof(sockaddr_in);
	opts.broadcast_addrs = broadcasts.empty() ? nullptr : broadcasts.data();
	opts.broadcast_num = broadcasts.size();
	opts.cb = &OnHosts;
	opts.cb_user = nullptr;
	const ChiakiErrorCode err = chiaki_discovery_service_init(&gDiscovery, &opts, &gLog);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		Log(kError, "Discovery failed to start: %s", chiaki_error_string(err));
		return;
	}
	gDiscoveryRunning = true;
	Log(kInfo, "Discovery running on %zu subnet(s)", broadcasts.size());
}

void DiscoveryStop()
{
	if(!gDiscoveryRunning)
		return;
	chiaki_discovery_service_fini(&gDiscovery);
	gDiscoveryRunning = false;
}

void Wake(Reader &r)
{
	const std::string address = r.str();
	char hex[17] = {};
	r.raw(hex, 16);
	const bool ps5 = r.u8() != 0;
	if(!r.ok())
		return;
	// The registration key is ASCII hex (\0 padded); the wake credential is its numeric value.
	if(std::strlen(hex) > 8)
	{
		Log(kError, "Wake: unexpected registration key format");
		return;
	}
	const uint64_t credential = std::strtoull(hex, nullptr, 16);
	const ChiakiErrorCode err = chiaki_discovery_wakeup(&gLog, nullptr, address.c_str(), credential, ps5);
	if(err != CHIAKI_ERR_SUCCESS)
		Log(kError, "Wake failed: %s", chiaki_error_string(err));
	else
		Log(kInfo, "Wake packet sent to %s", address.c_str());
}

// ------------------------------------------------------------------------------------------------
// Linking with a console (the 8-digit code)

ChiakiRegist gRegist{};
bool gRegistStarted = false;

void RegistReset()
{
	if(!gRegistStarted)
		return;
	chiaki_regist_stop(&gRegist); // joins its thread: never from inside OnRegistEvent
	chiaki_regist_fini(&gRegist);
	gRegistStarted = false;
}

void SendRegistFailure(const char *error)
{
	Writer w(Msg::RegistResult);
	w.u8(0).str(error);
	Send(w);
}

void OnRegistEvent(ChiakiRegistEvent *event, void *)
{
	switch(event->type)
	{
		case CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS: {
			const ChiakiRegisteredHost &h = *event->registered_host;
			Writer w(Msg::RegistResult);
			w.u8(1).str("").str(h.server_nickname).raw(h.server_mac, 6).u32(static_cast<uint32_t>(h.target));
			w.raw(h.rp_regist_key, 16).raw(h.rp_key, 16).u32(h.rp_key_type).u32(h.console_pin);
			Send(w);
			break;
		}
		case CHIAKI_REGIST_EVENT_TYPE_FINISHED_FAILED:
			SendRegistFailure("the console rejected the code (check the 8-digit code and that both devices are on the same network)");
			break;
		case CHIAKI_REGIST_EVENT_TYPE_FINISHED_CANCELED: SendRegistFailure("registration cancelled"); break;
	}
}

void RegistStart(Reader &r)
{
	RegistReset();
	const std::string address = r.str();
	ChiakiRegistInfo info{};
	info.target = static_cast<ChiakiTarget>(r.u32());
	r.raw(info.psn_account_id, sizeof(info.psn_account_id));
	info.pin = r.u32();
	info.console_pin = r.u32();
	if(!r.ok())
		return SendRegistFailure("bad request");
	info.host = address.c_str(); // copied by chiaki_regist_start
	info.broadcast = address == "255.255.255.255";
	info.psn_online_id = nullptr;
	const ChiakiErrorCode err = chiaki_regist_start(&gRegist, &gLog, &info, &OnRegistEvent, nullptr);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		SendRegistFailure((std::string("could not start registration: ") + chiaki_error_string(err)).c_str());
		return;
	}
	gRegistStarted = true;
}

// ------------------------------------------------------------------------------------------------
// PSN: the consoles on an account (for internet play)

void ListDevices(uint32_t requestId, std::string token)
{
	// A web request: off the command thread.
	std::thread([requestId, token = std::move(token)] {
		std::string error;
		uint32_t total = 0;
		std::vector<uint8_t> body;
		for(const auto type : {CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS5, CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS4})
		{
			ChiakiHolepunchDeviceInfo *devices = nullptr;
			size_t count = 0;
			const ChiakiErrorCode err = chiaki_holepunch_list_devices(token.c_str(), type, &devices, &count, &gLog);
			if(err != CHIAKI_ERR_SUCCESS)
			{
				if(error.empty())
					error = std::string("PSN device list failed: ") + chiaki_error_string(err);
				continue;
			}
			for(size_t i = 0; i < count; ++i)
			{
				const std::string name(devices[i].device_name, strnlen(devices[i].device_name, sizeof(devices[i].device_name)));
				const uint32_t len = static_cast<uint32_t>(name.size());
				const auto *lp = reinterpret_cast<const uint8_t *>(&len);
				body.insert(body.end(), lp, lp + 4);
				body.insert(body.end(), name.begin(), name.end());
				body.insert(body.end(), devices[i].device_uid, devices[i].device_uid + 32);
				body.push_back(devices[i].type == CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS5 ? 1 : 0);
				body.push_back(devices[i].remoteplay_enabled ? 1 : 0);
				++total;
			}
			chiaki_holepunch_free_device_list(&devices);
		}
		Writer w(Msg::DevicesResult);
		w.u32(requestId).str(total ? std::string() : error).u32(total).raw(body.data(), body.size());
		Send(w);
	}).detach();
}

void NewDeviceUid(uint32_t requestId)
{
	char duid[CHIAKI_DUID_STR_SIZE] = {};
	size_t size = sizeof(duid);
	chiaki_holepunch_generate_client_device_uid(duid, &size);
	Writer w(Msg::DeviceUid);
	w.u32(requestId).str(duid);
	Send(w);
}

// ------------------------------------------------------------------------------------------------
// The Remote Play session

void SendState(nxe::SessionPhase phase, bool isError, const std::string &text)
{
	Writer w(Msg::SessionState);
	w.u8(static_cast<uint8_t>(phase)).u8(isError ? 1 : 0).str(text);
	Send(w);
}

constexpr unsigned kMicRate = 48000, kMicFrame = 480; // what the console takes: 48 kHz, 10 ms at a time

struct Session {
	ChiakiSession session{};
	ChiakiOpusDecoder opus{};
	bool init = false, started = false, opusInit = false;
	// The player's microphone, when the client sends one: encoded here and sent to the console.
	ChiakiOpusEncoder mic{};
	bool micInit = false, micConnected = false, micLive = false;
	std::atomic<bool> streaming{false};

	// Internet play: the PSN handshake runs on its own thread, then starts the session.
	ChiakiHolepunchSession holepunch = nullptr;
	std::thread psnThread;
	std::atomic<bool> psnCancel{false};
	uint8_t deviceUid[32] = {};

	ChiakiOrientationTracker orient{};
	ChiakiAccelNewZero accelZero{};
	int hapticLevel = 3, triggerLevel = 3;
	uint32_t audioChannels = 2;
	std::atomic<uint64_t> fecFailures{0}, keyframeRequests{0};

	std::thread statsThread;
	std::atomic<bool> statsStop{false};
};
std::unique_ptr<Session> gSession;

bool OnVideoSample(uint8_t *buf, size_t size, int32_t framesLost, bool recovered, void *)
{
	const int64_t received = Qpc();
	BoostCurrentThread();
	Writer w(Msg::Video);
	w.u64(static_cast<uint64_t>(received)).i32(framesLost).u8(recovered ? 1 : 0).raw(buf, size);
	Send(w);
	return true; // a client that cannot decode a frame asks for a keyframe
}

void OnAudioSettings(uint32_t channels, uint32_t rate, void *user)
{
	static_cast<Session *>(user)->audioChannels = channels;
	Writer w(Msg::AudioFormat);
	w.u32(channels).u32(rate);
	Send(w);
}

void OnAudioFrame(int16_t *buf, size_t samples, void *user)
{
	BoostCurrentThread();
	Writer w(Msg::Audio);
	w.raw(buf, samples * static_cast<Session *>(user)->audioChannels * sizeof(int16_t)); // `samples` counts per channel
	Send(w);
}

void OnHapticsFrame(uint8_t *buf, size_t size, void *)
{
	if(!buf || !size)
		return;
	Writer w(Msg::Haptics);
	w.raw(buf, size);
	Send(w);
}

void OnCantDisplay(void *, bool cantDisplay)
{
	Writer w(Msg::CantDisplay);
	w.u8(cantDisplay ? 1 : 0);
	Send(w);
}

void OnEvent(ChiakiEvent *event, void *user)
{
	auto *self = static_cast<Session *>(user);
	switch(event->type)
	{
		case CHIAKI_EVENT_CONNECTED:
			self->streaming = true;
			SendState(nxe::SessionPhase::Streaming, false, {});
			break;
		case CHIAKI_EVENT_LOGIN_PIN_REQUEST: {
			Writer w(Msg::LoginPinRequest);
			w.u8(event->login_pin_request.pin_incorrect ? 1 : 0);
			Send(w);
			break;
		}
		case CHIAKI_EVENT_NICKNAME_RECEIVED: {
			Writer w(Msg::Nickname);
			w.str(event->server_nickname);
			Send(w);
			break;
		}
		case CHIAKI_EVENT_RUMBLE: {
			Writer w(Msg::Rumble);
			w.u8(event->rumble.left).u8(event->rumble.right);
			Send(w);
			break;
		}
		case CHIAKI_EVENT_TRIGGER_EFFECTS: {
			Writer w(Msg::TriggerEffects);
			w.u8(event->trigger_effects.type_left).u8(event->trigger_effects.type_right);
			w.raw(event->trigger_effects.left, 10).raw(event->trigger_effects.right, 10);
			Send(w);
			break;
		}
		case CHIAKI_EVENT_HAPTIC_INTENSITY:
		case CHIAKI_EVENT_TRIGGER_INTENSITY: {
			// libchiaki: Off 0, Strong 1, Medium 2, Weak 3 -> protocol: 0 off .. 3 strong.
			const int level = event->intensity == Off ? 0 : event->intensity == Weak ? 1 : event->intensity == Medium ? 2 : 3;
			(event->type == CHIAKI_EVENT_HAPTIC_INTENSITY ? self->hapticLevel : self->triggerLevel) = level;
			Writer w(Msg::Intensity);
			w.u8(static_cast<uint8_t>(self->hapticLevel)).u8(static_cast<uint8_t>(self->triggerLevel));
			Send(w);
			break;
		}
		case CHIAKI_EVENT_VIDEO_FEC_FAILURE:
			self->fecFailures.fetch_add(1);
			if(!event->video_fec_failure.idr_request_sent)
			{
				chiaki_session_request_idr(&self->session);
				chiaki_video_receiver_set_waiting_for_idr(self->session.stream_connection.video_receiver, true);
			}
			self->keyframeRequests.fetch_add(1);
			break;
		case CHIAKI_EVENT_QUIT: {
			self->streaming = false;
			std::string reason = chiaki_quit_reason_string(event->quit.reason);
			if(event->quit.reason_str && *event->quit.reason_str)
				reason += std::string(": ") + event->quit.reason_str;
			SendState(nxe::SessionPhase::Ended, chiaki_quit_reason_is_error(event->quit.reason), reason);
			break;
		}
		default: break;
	}
}

void RunPsnConnect(Session *s)
{
	SetThreadDescription(GetCurrentThread(), L"PSN connect");
	const ChiakiHolepunchConsoleType type = s->session.target >= CHIAKI_TARGET_PS5_UNKNOWN ? CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS5 : CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS4;
	auto fail = [&](const char *what, ChiakiErrorCode err) {
		if(s->psnCancel.load())
			return;
		Log(kWarning, "Internet play: %s failed: %s", what, chiaki_error_string(err));
		SendState(nxe::SessionPhase::Ended, true, std::string("internet connection failed (") + what + ": " + chiaki_error_string(err) + ")");
	};
	auto step = [&](const char *stage, const char *what, auto fn) {
		if(s->psnCancel.load())
			return false;
		SendState(nxe::SessionPhase::Connecting, false, stage);
		const ChiakiErrorCode err = fn();
		if(err != CHIAKI_ERR_SUCCESS)
		{
			fail(what, err);
			return false;
		}
		Log(kInfo, "Internet play: %s ok", what);
		return true;
	};
	// Same order as chiaki-ng: UPnP (optional port mapping), PSN session, offer, start, punch control hole.
	if(!step("Checking your router (UPnP)...", "UPnP discovery", [&] { return chiaki_holepunch_upnp_discover(s->holepunch); }) ||
	   !step("Creating the PSN session...", "PSN session", [&] { return chiaki_holepunch_session_create(s->holepunch); }) ||
	   !step("Sending connection offer...", "offer", [&] { return holepunch_session_create_offer(s->holepunch); }) ||
	   !step("Waiting for the console to answer...", "console start", [&] { return chiaki_holepunch_session_start(s->holepunch, s->deviceUid, type); }) ||
	   !step("Punching through the routers...", "control hole punch", [&] { return chiaki_holepunch_session_punch_hole(s->holepunch, CHIAKI_HOLEPUNCH_PORT_TYPE_CTRL); }))
		return;
	if(s->psnCancel.load())
		return;
	SendState(nxe::SessionPhase::Connecting, false, "Starting the stream...");
	const ChiakiErrorCode err = chiaki_session_start(&s->session);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		fail("session start", err);
		return;
	}
	s->started = true;
}

void SessionStop(bool announce)
{
	if(!gSession)
	{
		if(announce)
			Send(Msg::SessionStopped);
		return;
	}
	Session *s = gSession.get();
	s->streaming = false;
	s->statsStop = true;
	if(s->statsThread.joinable())
		s->statsThread.join();
	if(s->psnThread.joinable())
	{
		s->psnCancel = true;
		if(s->holepunch)
			chiaki_holepunch_main_thread_cancel(s->holepunch, true);
		s->psnThread.join();
	}
	if(s->started)
	{
		chiaki_session_stop(&s->session);
		chiaki_session_join(&s->session);
	}
	if(s->init)
		chiaki_session_fini(&s->session); // also ends the PSN session (internet play)
	if(s->opusInit)
		chiaki_opus_decoder_fini(&s->opus);
	if(s->micInit)
		chiaki_opus_encoder_fini(&s->mic);
	gSession.reset();
	if(announce)
		Send(Msg::SessionStopped);
}

void SessionStart(Reader &r)
{
	SessionStop(false);
	const nxe::SessionParams p = nxe::ReadSessionParams(r);
	if(!r.ok())
		return SendState(nxe::SessionPhase::Ended, true, "bad session request");

	gSession = std::make_unique<Session>();
	Session *s = gSession.get();
	std::memcpy(s->deviceUid, p.deviceUid, sizeof(s->deviceUid));
	chiaki_orientation_tracker_init(&s->orient);

	ChiakiConnectInfo ci{};
	ci.ps5 = p.ps5;
	ci.host = p.host.c_str();
	std::memcpy(ci.regist_key, p.registKey, sizeof(ci.regist_key));
	std::memcpy(ci.morning, p.rpKey, sizeof(ci.morning));
	std::memcpy(ci.psn_account_id, p.accountId, sizeof(ci.psn_account_id));
	const ChiakiVideoResolutionPreset res = p.resolution <= 360   ? CHIAKI_VIDEO_RESOLUTION_PRESET_360p
	                                        : p.resolution <= 540 ? CHIAKI_VIDEO_RESOLUTION_PRESET_540p
	                                        : p.resolution <= 720 ? CHIAKI_VIDEO_RESOLUTION_PRESET_720p
	                                                              : CHIAKI_VIDEO_RESOLUTION_PRESET_1080p;
	chiaki_connect_video_profile_preset(&ci.video_profile, res, p.fps <= 30 ? CHIAKI_VIDEO_FPS_PRESET_30 : CHIAKI_VIDEO_FPS_PRESET_60);
	if(p.bitrateKbps > 0)
		ci.video_profile.bitrate = p.bitrateKbps;
	ci.video_profile.codec = p.codec == 2 ? CHIAKI_CODEC_H265_HDR : p.codec == 1 ? CHIAKI_CODEC_H265 : CHIAKI_CODEC_H264;
	ci.video_profile_auto_downgrade = true; // e.g. a base PS4 cannot do 1080p
	ci.enable_keyboard = false;
	ci.enable_dualsense = p.dualsense;
	ci.audio_video_disabled = CHIAKI_NONE_DISABLED;
	ci.auto_regist = false;
	ci.packet_loss_max = 0.05;
	ci.enable_idr_on_fec_failure = true; // ask for a recovery frame immediately instead of waiting

	auto fail = [&](const std::string &why) {
		gSession.reset();
		SendState(nxe::SessionPhase::Ended, true, why);
	};
	if(p.internet)
	{
		s->holepunch = chiaki_holepunch_session_init(p.psnToken.c_str(), &gLog);
		if(!s->holepunch)
			return fail("could not start a PSN session");
		ci.holepunch_session = s->holepunch;
	}
	ChiakiErrorCode err = chiaki_session_init(&s->session, &ci, &gLog);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		if(s->holepunch)
			chiaki_holepunch_session_fini(s->holepunch);
		return fail(std::string("session init failed: ") + chiaki_error_string(err));
	}
	s->init = true;

	chiaki_opus_decoder_init(&s->opus, &gLog);
	s->opusInit = true;
	chiaki_opus_decoder_set_cb(&s->opus, &OnAudioSettings, &OnAudioFrame, s);
	ChiakiAudioSink sink{};
	chiaki_opus_decoder_get_sink(&s->opus, &sink);
	chiaki_session_set_audio_sink(&s->session, &sink);
	chiaki_session_set_video_sample_cb(&s->session, &OnVideoSample, s);
	// Ready for a microphone (48 kHz stereo, 10 ms frames). Nothing is sent unless the client opens it.
	chiaki_opus_encoder_init(&s->mic, &gLog);
	s->micInit = true;
	ChiakiAudioHeader micHeader{};
	chiaki_audio_header_set(&micHeader, 2, 16, kMicRate, kMicFrame);
	chiaki_opus_encoder_header(&micHeader, &s->mic, &s->session);
	ChiakiAudioSink haptics{};
	haptics.user = s;
	haptics.frame_cb = &OnHapticsFrame;
	chiaki_session_set_haptics_sink(&s->session, &haptics);
	chiaki_session_set_event_cb(&s->session, &OnEvent, s);
	// libchiaki calls this without a null check when the console shows content that can't be
	// streamed (YouTube, Netflix, ...), so it must always be set.
	ChiakiCtrlDisplaySink displaySink{};
	displaySink.user = s;
	displaySink.cantdisplay_cb = &OnCantDisplay;
	chiaki_session_ctrl_set_display_sink(&s->session, &displaySink);

	Log(kInfo, "Connecting to %s (%s, %up, %u kbps, %s)", p.internet ? "the console over the internet" : p.host.c_str(), p.ps5 ? "PS5" : "PS4",
	    p.resolution, ci.video_profile.bitrate, p.codec == 2 ? "HEVC HDR10" : p.codec == 1 ? "HEVC" : "H.264");
	SendState(nxe::SessionPhase::Connecting, false, {});

	if(p.internet)
	{
		// PSN handshake + NAT hole punching takes a few seconds; the session starts when it's done.
		s->psnThread = std::thread(&RunPsnConnect, s);
	}
	else
	{
		err = chiaki_session_start(&s->session);
		if(err != CHIAKI_ERR_SUCCESS)
		{
			chiaki_session_fini(&s->session);
			chiaki_opus_decoder_fini(&s->opus);
			chiaki_opus_encoder_fini(&s->mic);
			return fail(std::string("could not start session: ") + chiaki_error_string(err));
		}
		s->started = true;
	}

	// Network numbers once a second while the stream runs.
	s->statsThread = std::thread([s] {
		int ticks = 0;
		while(!s->statsStop.load())
		{
			Sleep(50);
			if(++ticks < 20)
				continue;
			ticks = 0;
			if(!s->streaming.load())
				continue;
			uint64_t received = 0, lost = 0;
			chiaki_packet_stats_get(&s->session.stream_connection.packet_stats, true, &received, &lost);
			Writer w(Msg::Stats);
			w.u64(received).u64(lost).u32(static_cast<uint32_t>(s->session.rtt_us)).u64(s->fecFailures.load()).u64(s->keyframeRequests.load());
			Send(w);
		}
	});
}

uint32_t ToChiakiButtons(uint32_t b)
{
	static const struct {
		uint32_t ours, theirs;
	} map[] = {
		{nxe::kCross, CHIAKI_CONTROLLER_BUTTON_CROSS},         {nxe::kCircle, CHIAKI_CONTROLLER_BUTTON_MOON},
		{nxe::kSquare, CHIAKI_CONTROLLER_BUTTON_BOX},          {nxe::kTriangle, CHIAKI_CONTROLLER_BUTTON_PYRAMID},
		{nxe::kDpadLeft, CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT},  {nxe::kDpadRight, CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT},
		{nxe::kDpadUp, CHIAKI_CONTROLLER_BUTTON_DPAD_UP},      {nxe::kDpadDown, CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN},
		{nxe::kL1, CHIAKI_CONTROLLER_BUTTON_L1},               {nxe::kR1, CHIAKI_CONTROLLER_BUTTON_R1},
		{nxe::kL3, CHIAKI_CONTROLLER_BUTTON_L3},               {nxe::kR3, CHIAKI_CONTROLLER_BUTTON_R3},
		{nxe::kOptions, CHIAKI_CONTROLLER_BUTTON_OPTIONS},     {nxe::kShare, CHIAKI_CONTROLLER_BUTTON_SHARE},
		{nxe::kTouchpad, CHIAKI_CONTROLLER_BUTTON_TOUCHPAD},   {nxe::kPs, CHIAKI_CONTROLLER_BUTTON_PS},
	};
	uint32_t out = 0;
	for(const auto &m : map)
		if(b & m.ours)
			out |= m.theirs;
	return out;
}

void Pad(Reader &r)
{
	Session *s = gSession.get();
	const nxe::PadState p = nxe::ReadPad(r);
	if(!s || !r.ok() || !s->streaming.load())
		return;
	ChiakiControllerState cs;
	chiaki_controller_state_set_idle(&cs);
	cs.buttons = ToChiakiButtons(p.buttons);
	cs.l2_state = p.l2;
	cs.r2_state = p.r2;
	cs.left_x = p.leftX;
	cs.left_y = p.leftY;
	cs.right_x = p.rightX;
	cs.right_y = p.rightY;
	for(size_t i = 0; i < CHIAKI_CONTROLLER_TOUCHES_MAX; ++i)
	{
		cs.touches[i].id = p.touches[i].id;
		cs.touches[i].x = p.touches[i].x;
		cs.touches[i].y = p.touches[i].y;
	}
	if(p.hasMotion)
		chiaki_orientation_tracker_update(&s->orient, p.gyro[0], p.gyro[1], p.gyro[2], p.accel[0], p.accel[1], p.accel[2], &s->accelZero, false,
		                                  static_cast<uint32_t>(p.motionTimeUs));
	chiaki_orientation_tracker_apply_to_controller_state(&s->orient, &cs);
	chiaki_session_set_controller_state(&s->session, &cs);
}

// The microphone: the first "open" tells the console a microphone is there, after that it is
// opened and muted. (libchiaki's call takes "is muted now": true opens it.)
void Mic(Reader &r)
{
	Session *s = gSession.get();
	const bool live = r.u8() != 0;
	if(!s || !r.ok() || !s->streaming.load())
		return;
	if(live && !s->micConnected)
	{
		if(chiaki_session_connect_microphone(&s->session) != CHIAKI_ERR_SUCCESS)
			return Log(kWarning, "The console did not take the microphone");
		s->micConnected = true;
		Log(kInfo, "Microphone connected");
	}
	if(!s->micConnected)
		return;
	chiaki_session_toggle_microphone(&s->session, live);
	s->micLive = live;
}

void MicAudio(Reader &r)
{
	Session *s = gSession.get();
	if(!s || !s->streaming.load() || !s->micLive)
		return;
	int16_t frame[kMicFrame * 2]; // the message bytes are not aligned for int16
	while(r.left() >= sizeof(frame))
	{
		r.raw(frame, sizeof(frame));
		chiaki_opus_encoder_frame(frame, &s->mic);
	}
}

// ------------------------------------------------------------------------------------------------

void Dispatch(Msg type, Reader &r)
{
	switch(type)
	{
		case Msg::Hello: {
			const uint32_t version = r.u32();
			if(version != nxe::kProtocolVersion)
				Log(kWarning, "Client speaks protocol %u, this engine speaks %u", version, nxe::kProtocolVersion);
			Writer w(Msg::HelloReply);
			w.u32(nxe::kProtocolVersion).str("nixani-engine " NXE_VERSION);
			Send(w);
			break;
		}
		case Msg::DiscoveryStart: DiscoveryStart(); break;
		case Msg::DiscoveryStop: DiscoveryStop(); break;
		case Msg::Wake: Wake(r); break;
		case Msg::RegistStart: RegistStart(r); break;
		case Msg::RegistCancel: RegistReset(); break;
		case Msg::ListDevices: {
			const uint32_t id = r.u32();
			ListDevices(id, r.str());
			break;
		}
		case Msg::NewDeviceUid: NewDeviceUid(r.u32()); break;
		case Msg::SessionStart: SessionStart(r); break;
		case Msg::SessionStop: SessionStop(true); break;
		case Msg::Pad: Pad(r); break;
		case Msg::Mic: Mic(r); break;
		case Msg::MicAudio: MicAudio(r); break;
		case Msg::LoginPin: {
			const std::string pin = r.str();
			if(gSession && gSession->init)
				chiaki_session_set_login_pin(&gSession->session, reinterpret_cast<const uint8_t *>(pin.data()), pin.size());
			break;
		}
		case Msg::RequestKeyframe:
			if(gSession && gSession->streaming.load())
			{
				chiaki_session_request_idr(&gSession->session);
				gSession->keyframeRequests.fetch_add(1);
			}
			break;
		case Msg::Ping: {
			const uint64_t clientTime = r.u64();
			const uint32_t echo = std::min<uint32_t>(r.u32(), 4u * 1024 * 1024);
			const std::vector<uint8_t> zeros(echo);
			Writer w(Msg::Pong);
			w.u64(clientTime).u64(static_cast<uint64_t>(Qpc())).raw(zeros.data(), zeros.size());
			Send(w);
			break;
		}
		default: break;
	}
}

} // namespace

int main(int argc, char **argv)
{
	if(argc >= 2 && std::strcmp(argv[1], "--version") == 0)
	{
		std::printf("nixani-engine %s (protocol %u)\n", NXE_VERSION, nxe::kProtocolVersion);
		return 0;
	}

	// The protocol owns the real standard output. Anything a library prints with printf goes nowhere
	// instead of into the message stream.
	const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
	if(!DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_OUTPUT_HANDLE), GetCurrentProcess(), &gOut, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
	   in == nullptr || in == INVALID_HANDLE_VALUE)
	{
		std::fprintf(stderr, "nixani-engine is started by a client program and talks to it over stdin/stdout (see PROTOCOL.md).\n");
		return 2;
	}
	FILE *nul = nullptr;
	freopen_s(&nul, "NUL", "w", stdout);

	chiaki_log_init(&gLog, CHIAKI_LOG_ALL & ~(CHIAKI_LOG_VERBOSE | CHIAKI_LOG_DEBUG), &OnChiakiLog, nullptr);
	const ChiakiErrorCode err = chiaki_lib_init();
	if(err != CHIAKI_ERR_SUCCESS)
	{
		Log(kError, "libchiaki init failed: %s", chiaki_error_string(err));
		return 1;
	}

	// Commands, one frame after another, until the client closes the pipe.
	std::vector<uint8_t> buf;
	std::vector<uint8_t> chunk(64 * 1024);
	for(;;)
	{
		DWORD n = 0;
		if(!ReadFile(in, chunk.data(), static_cast<DWORD>(chunk.size()), &n, nullptr) || n == 0)
			break;
		buf.insert(buf.end(), chunk.begin(), chunk.begin() + n);
		size_t pos = 0;
		while(buf.size() - pos >= 6)
		{
			uint32_t size = 0;
			std::memcpy(&size, buf.data() + pos, 4);
			if(size < 2 || size > nxe::kMaxFrame)
				return 3; // not our protocol
			if(buf.size() - pos < 4 + static_cast<size_t>(size))
				break;
			uint16_t type = 0;
			std::memcpy(&type, buf.data() + pos + 4, 2);
			Reader r(buf.data() + pos + 6, size - 2);
			Dispatch(static_cast<Msg>(type), r);
			pos += 4 + static_cast<size_t>(size);
		}
		buf.erase(buf.begin(), buf.begin() + static_cast<ptrdiff_t>(pos));
	}
	SessionStop(false);
	RegistReset();
	DiscoveryStop();
	return 0;
}
