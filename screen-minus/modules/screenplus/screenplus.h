// ScreenPlus: the pack's side of the Screen+ bus (docs/dev-engine.md §4–5).
// Every API is a JSON-RPC call to the engine, which checks the grant and does the work; nothing here touches devices.
// A reader thread receives frames and hands them to the main thread; writes are locked and may come from any thread.

#pragma once

#include "core/object/object.h"
#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/hash_map.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/typed_array.h"

struct SPPipe;

// One pending call. `await ScreenPlus.call_api(...).done` gives [result, error]; error is {} on success.
class SPRequest : public RefCounted {
	GDCLASS(SPRequest, RefCounted);

	friend class ScreenPlus;
	int id = 0;
	bool finished = false;
	Variant result;
	Dictionary error;

	void _finish(const Variant &p_result, const Dictionary &p_error);

protected:
	static void _bind_methods();

public:
	bool is_finished() const { return finished; }
	Variant get_result() const { return result; }
	Dictionary get_error() const { return error; }
	bool is_ok() const { return finished && error.is_empty(); }
};

class ScreenPlus : public Object {
	GDCLASS(ScreenPlus, Object);

	static ScreenPlus *singleton;

	SPPipe *pipe = nullptr;
	Thread reader;
	Mutex write_lock;
	SafeFlag connected;

	int next_id = 1;
	HashMap<int, Ref<SPRequest>> pending;

	String engine_version;
	Dictionary props;
	Dictionary display;
	Dictionary granted;
	Vector2 cursor;
	bool paused = false;
	bool muted = false;
	bool ready_sent = false;
	int saved_sleep_usec = -1;
	bool saved_low_processor = false;
	PackedFloat32Array spectrum;
	float level = 0.0f;

	static void _reader_main(void *p_self);
	bool _send(const Dictionary &p_msg);
	void _dispatch(const Variant &p_msg);
	void _notification_in(const String &p_method, const Variant &p_params);
	void _set_paused(bool p_paused);
	void _apply_mute();
	void _first_frame();
	void _disconnected();
	void _fail_request(const Ref<SPRequest> &p_req, int p_code, const String &p_message);

protected:
	static void _bind_methods();

public:
	static ScreenPlus *get_singleton() { return singleton; }

	void start();
	void stop();

	Ref<SPRequest> call_api(const String &p_method, const Variant &p_params = Variant());
	void notify(const String &p_method, const Variant &p_params = Variant());
	void log(const String &p_message, int p_level = 1);

	bool is_connected_to_engine() const { return connected.is_set(); }
	bool is_paused() const { return paused; }
	String get_engine_version() const { return engine_version; }
	Variant property(const String &p_name, const Variant &p_default = Variant()) const;
	Dictionary get_properties() const { return props; }
	Dictionary get_display() const { return display; }
	Dictionary get_granted() const { return granted; }
	bool has_capability(const String &p_name) const;
	Vector2 get_cursor() const { return cursor; }
	PackedFloat32Array audio_spectrum() const { return spectrum; }
	float audio_level() const { return level; }

	ScreenPlus();
	~ScreenPlus();
};
