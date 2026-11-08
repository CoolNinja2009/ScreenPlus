#include "screenplus.h"

#include "sp_pipe.h"

#include "core/io/json.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/main/scene_tree.h"
#include "servers/audio/audio_server.h"
#include "servers/rendering/rendering_server.h"

// Frames are a 4-byte little-endian length and UTF-8 JSON. Anything bigger is a broken peer, not data:
// bulk data travels in shared memory.
static constexpr uint32_t SP_MAX_FRAME = 16u << 20;

// JSON-RPC error codes the engine and this side share (docs/dev-engine.md §4).
enum {
	SP_ERR_NOT_CONNECTED = -32000,
	SP_ERR_NOT_GRANTED = -32001,
};

ScreenPlus *ScreenPlus::singleton = nullptr;

// SPRequest

void SPRequest::_finish(const Variant &p_result, const Dictionary &p_error) {
	if (finished) {
		return;
	}
	finished = true;
	result = p_result;
	error = p_error;
	emit_signal(SNAME("done"), result, error);
}

void SPRequest::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_finished"), &SPRequest::is_finished);
	ClassDB::bind_method(D_METHOD("is_ok"), &SPRequest::is_ok);
	ClassDB::bind_method(D_METHOD("get_result"), &SPRequest::get_result);
	ClassDB::bind_method(D_METHOD("get_error"), &SPRequest::get_error);
	ADD_SIGNAL(MethodInfo("done", PropertyInfo(Variant::NIL, "result", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT), PropertyInfo(Variant::DICTIONARY, "error")));
}

// ScreenPlus

ScreenPlus::ScreenPlus() {
	singleton = this;
}

ScreenPlus::~ScreenPlus() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

void ScreenPlus::start() {
#ifndef WEB_ENABLED
	// Screen+ passes its arguments after "--": --sp-pipe <name> [--sp-display <n>].
	String name;
	const List<String> args = OS::get_singleton()->get_cmdline_user_args();
	for (List<String>::ConstIterator it = args.begin(); it != args.end(); ++it) {
		if (*it == "--sp-pipe") {
			++it;
			if (it == args.end()) {
				break;
			}
			name = *it;
		}
	}
	if (name.is_empty()) {
		return; // Not started by Screen+ (editor, plain run): every call fails with SP_ERR_NOT_CONNECTED.
	}
	pipe = sp_pipe_open(name.utf8().get_data());
	if (!pipe) {
		ERR_PRINT("ScreenPlus: can't open the Screen+ pipe " + name);
		return;
	}
	connected.set();
	reader.start(&ScreenPlus::_reader_main, this);
#endif
	// Web builds talk through JavaScriptBridge to window.screenplus (phase 5).
}

void ScreenPlus::stop() {
	if (pipe) {
		connected.clear();
		sp_pipe_close(pipe);
		if (reader.is_started()) {
			reader.wait_to_finish();
		}
		sp_pipe_free(pipe);
		pipe = nullptr;
	}
}

void ScreenPlus::_reader_main(void *p_self) {
	ScreenPlus *self = static_cast<ScreenPlus *>(p_self);
	Vector<uint8_t> buf;
	for (;;) {
		uint8_t hdr[4];
		if (!sp_pipe_read(self->pipe, hdr, 4)) {
			break;
		}
		const uint32_t len = uint32_t(hdr[0]) | uint32_t(hdr[1]) << 8 | uint32_t(hdr[2]) << 16 | uint32_t(hdr[3]) << 24;
		if (len == 0 || len > SP_MAX_FRAME) {
			break;
		}
		buf.resize(len);
		if (!sp_pipe_read(self->pipe, buf.ptrw(), len)) {
			break;
		}
		String text;
		text.append_utf8(reinterpret_cast<const char *>(buf.ptr()), len);
		JSON json;
		if (json.parse(text) != OK || json.get_data().get_type() != Variant::DICTIONARY) {
			continue; // A bad message is dropped, not fatal.
		}
		callable_mp(self, &ScreenPlus::_dispatch).call_deferred(json.get_data());
	}
	callable_mp(self, &ScreenPlus::_disconnected).call_deferred();
}

bool ScreenPlus::_send(const Dictionary &p_msg) {
	if (!connected.is_set()) {
		return false;
	}
	const CharString utf8 = JSON::stringify(p_msg, "", false).utf8();
	const uint32_t len = uint32_t(utf8.length());
	const uint8_t hdr[4] = { uint8_t(len), uint8_t(len >> 8), uint8_t(len >> 16), uint8_t(len >> 24) };
	MutexLock lock(write_lock);
	return sp_pipe_write(pipe, hdr, 4) && sp_pipe_write(pipe, utf8.get_data(), len);
}

void ScreenPlus::_fail_request(const Ref<SPRequest> &p_req, int p_code, const String &p_message) {
	Dictionary err;
	err["code"] = p_code;
	err["message"] = p_message;
	// Deferred, so `await req.done` is connected before the signal fires.
	callable_mp(p_req.ptr(), &SPRequest::_finish).call_deferred(Variant(), err);
}

Ref<SPRequest> ScreenPlus::call_api(const String &p_method, const Variant &p_params) {
	Ref<SPRequest> req;
	req.instantiate();
	req->id = next_id++;
	if (!connected.is_set()) {
		_fail_request(req, SP_ERR_NOT_CONNECTED, "Not running in Screen+");
		return req;
	}
	// A capability the user didn't grant is refused here too, to save a round trip; the engine still checks.
	const String ns = p_method.get_slicec('.', 0);
	if (ns != "sp" && !has_capability(ns) && !has_capability(p_method)) {
		_fail_request(req, SP_ERR_NOT_GRANTED, "Capability not granted: " + ns);
		return req;
	}
	Dictionary msg;
	msg["jsonrpc"] = "2.0";
	msg["id"] = req->id;
	msg["method"] = p_method;
	if (p_params.get_type() != Variant::NIL) {
		msg["params"] = p_params;
	}
	pending[req->id] = req;
	if (!_send(msg)) {
		pending.erase(req->id);
		_fail_request(req, SP_ERR_NOT_CONNECTED, "Screen+ went away");
	}
	return req;
}

void ScreenPlus::notify(const String &p_method, const Variant &p_params) {
	Dictionary msg;
	msg["jsonrpc"] = "2.0";
	msg["method"] = p_method;
	if (p_params.get_type() != Variant::NIL) {
		msg["params"] = p_params;
	}
	_send(msg);
}

void ScreenPlus::log(const String &p_message, int p_level) {
	Dictionary params;
	params["level"] = p_level;
	params["msg"] = p_message;
	notify("sp.log", params);
}

void ScreenPlus::_dispatch(const Variant &p_msg) {
	const Dictionary msg = p_msg;
	if (msg.has("id") && !msg.has("method")) {
		// A reply to call_api.
		const int id = int(msg["id"]);
		HashMap<int, Ref<SPRequest>>::Iterator e = pending.find(id);
		if (!e) {
			return;
		}
		Ref<SPRequest> req = e->value;
		pending.remove(e);
		req->_finish(msg.get("result", Variant()), msg.get("error", Dictionary()));
		return;
	}
	if (msg.has("method")) {
		// The engine sends notifications only; it never calls into the pack.
		_notification_in(msg["method"], msg.get("params", Variant()));
	}
}

void ScreenPlus::_notification_in(const String &p_method, const Variant &p_params) {
	const Dictionary p = p_params.get_type() == Variant::DICTIONARY ? Dictionary(p_params) : Dictionary();
	if (p_method == "sp.hello") {
		engine_version = p.get("version", "");
		display = p.get("display", Dictionary());
		props = p.get("props", Dictionary());
		granted = p.get("granted", Dictionary());
		if (!ready_sent && RenderingServer::get_singleton()) {
			RenderingServer::get_singleton()->connect(SNAME("frame_post_draw"), callable_mp(this, &ScreenPlus::_first_frame), CONNECT_ONE_SHOT);
		}
		emit_signal(SNAME("hello"), p);
	} else if (p_method == "sp.pause") {
		_set_paused(true);
	} else if (p_method == "sp.resume") {
		_set_paused(false);
	} else if (p_method == "sp.props") {
		for (const KeyValue<Variant, Variant> &kv : p) {
			props[kv.key] = kv.value;
			emit_signal(SNAME("property_changed"), kv.key, kv.value);
		}
	} else if (p_method == "sp.mute") {
		muted = p.get("on", false);
		_apply_mute();
	} else if (p_method == "sp.mouse") {
		cursor = Vector2(float(p.get("x", 0.0)), float(p.get("y", 0.0)));
		emit_signal(SNAME("cursor_moved"), cursor, int(p.get("buttons", 0)));
	} else if (p_method == "sp.display") {
		display = p;
		emit_signal(SNAME("display_changed"), display);
	} else if (p_method == "sp.grants") {
		granted = p;
		emit_signal(SNAME("grants_changed"), granted);
	} else if (p_method == "sp.stop") {
		if (SceneTree *tree = SceneTree::get_singleton()) {
			tree->quit();
		}
	} else if (p_method == "audio.frame") {
		// v0: the spectrum arrives as a notification; shared memory replaces it in phase 2.
		spectrum = p.get("spectrum", PackedFloat32Array());
		level = float(p.get("level", 0.0));
		emit_signal(SNAME("audio_frame"), spectrum, level);
	} else {
		emit_signal(SNAME("event"), p_method, p_params);
	}
}

void ScreenPlus::_first_frame() {
	ready_sent = true;
	notify("sp.ready");
}

void ScreenPlus::_set_paused(bool p_paused) {
	if (paused == p_paused) {
		return;
	}
	paused = p_paused;
	OS *os = OS::get_singleton();
	if (paused) {
		saved_low_processor = os->is_in_low_processor_usage_mode();
		saved_sleep_usec = os->get_low_processor_usage_mode_sleep_usec();
		os->set_low_processor_usage_mode(true);
		os->set_low_processor_usage_mode_sleep_usec(100000); // Wakes ten times a second, only to read the pipe.
	} else if (saved_sleep_usec >= 0) {
		os->set_low_processor_usage_mode(saved_low_processor);
		os->set_low_processor_usage_mode_sleep_usec(saved_sleep_usec);
	}
	if (SceneTree *tree = SceneTree::get_singleton()) {
		tree->set_pause(paused);
	}
	RenderingServer::get_singleton()->set_render_loop_enabled(!paused);
	_apply_mute();
	emit_signal(SNAME("paused_changed"), paused);
}

void ScreenPlus::_apply_mute() {
	if (AudioServer *audio = AudioServer::get_singleton()) {
		audio->set_bus_mute(0, muted || paused);
	}
}

void ScreenPlus::_disconnected() {
	connected.clear();
	for (KeyValue<int, Ref<SPRequest>> &kv : pending) {
		_fail_request(kv.value, SP_ERR_NOT_CONNECTED, "Screen+ went away");
	}
	pending.clear();
	// Without Screen+ there is nobody to show the wallpaper to.
	if (SceneTree *tree = SceneTree::get_singleton()) {
		tree->quit();
	}
}

Variant ScreenPlus::property(const String &p_name, const Variant &p_default) const {
	return props.get(p_name, p_default);
}

bool ScreenPlus::has_capability(const String &p_name) const {
	if (!granted.has(p_name)) {
		return false;
	}
	const Variant v = granted[p_name];
	return v.get_type() != Variant::BOOL || bool(v);
}

void ScreenPlus::_bind_methods() {
	ClassDB::bind_method(D_METHOD("call_api", "method", "params"), &ScreenPlus::call_api, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("notify", "method", "params"), &ScreenPlus::notify, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("log", "message", "level"), &ScreenPlus::log, DEFVAL(1));
	ClassDB::bind_method(D_METHOD("is_connected_to_engine"), &ScreenPlus::is_connected_to_engine);
	ClassDB::bind_method(D_METHOD("is_paused"), &ScreenPlus::is_paused);
	ClassDB::bind_method(D_METHOD("get_engine_version"), &ScreenPlus::get_engine_version);
	ClassDB::bind_method(D_METHOD("property", "name", "default"), &ScreenPlus::property, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("get_properties"), &ScreenPlus::get_properties);
	ClassDB::bind_method(D_METHOD("get_display"), &ScreenPlus::get_display);
	ClassDB::bind_method(D_METHOD("get_granted"), &ScreenPlus::get_granted);
	ClassDB::bind_method(D_METHOD("has_capability", "name"), &ScreenPlus::has_capability);
	ClassDB::bind_method(D_METHOD("get_cursor"), &ScreenPlus::get_cursor);
	ClassDB::bind_method(D_METHOD("audio_spectrum"), &ScreenPlus::audio_spectrum);
	ClassDB::bind_method(D_METHOD("audio_level"), &ScreenPlus::audio_level);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "paused", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "", "is_paused");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "display", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "", "get_display");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "cursor", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "", "get_cursor");

	ADD_SIGNAL(MethodInfo("hello", PropertyInfo(Variant::DICTIONARY, "info")));
	ADD_SIGNAL(MethodInfo("paused_changed", PropertyInfo(Variant::BOOL, "paused")));
	ADD_SIGNAL(MethodInfo("property_changed", PropertyInfo(Variant::STRING, "name"), PropertyInfo(Variant::NIL, "value", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT)));
	ADD_SIGNAL(MethodInfo("cursor_moved", PropertyInfo(Variant::VECTOR2, "position"), PropertyInfo(Variant::INT, "buttons")));
	ADD_SIGNAL(MethodInfo("display_changed", PropertyInfo(Variant::DICTIONARY, "display")));
	ADD_SIGNAL(MethodInfo("grants_changed", PropertyInfo(Variant::DICTIONARY, "granted")));
	ADD_SIGNAL(MethodInfo("audio_frame", PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "spectrum"), PropertyInfo(Variant::FLOAT, "level")));
	ADD_SIGNAL(MethodInfo("event", PropertyInfo(Variant::STRING, "name"), PropertyInfo(Variant::NIL, "data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT)));
}
