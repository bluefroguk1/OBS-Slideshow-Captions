#include <obs-module.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

// Shown in OBS's Plugin Manager; without these OBS falls back to the file name.
const char *obs_module_name(void)
{
	return obs_module_text("PluginName");
}

const char *obs_module_description(void)
{
	return obs_module_text("PluginDescription");
}

void register_slideshow_caption(void);

bool obs_module_load(void)
{
	register_slideshow_caption();
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
