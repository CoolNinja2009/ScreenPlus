// Registers the ScreenPlus singleton and connects to the Screen+ engine as soon as the scene level is up.

#include "register_types.h"

#include "screenplus.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"

static ScreenPlus *sp_singleton = nullptr;

void initialize_screenplus_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(SPRequest);
	GDREGISTER_ABSTRACT_CLASS(ScreenPlus);
	sp_singleton = memnew(ScreenPlus);
	Engine::get_singleton()->add_singleton(Engine::Singleton("ScreenPlus", sp_singleton));
	sp_singleton->start();
}

void uninitialize_screenplus_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE || !sp_singleton) {
		return;
	}
	sp_singleton->stop();
	memdelete(sp_singleton);
	sp_singleton = nullptr;
}
