//
// OpenVR Capture input plugin for OBS
// by Keijo "Kegetys" Ruotsalainen, http://www.kegetys.fi
//

#define _CRT_SECURE_NO_WARNINGS

#include <obs-module.h>
#include <graphics/image-file.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <sys/stat.h>
#include <d3d11.h>
#include <vector>

#pragma comment(lib, "d3d11.lib")

#include "headers/openvr.h"
#ifdef _WIN64
#pragma comment(lib, "lib/win64/openvr_api.lib")
#else
#pragma comment(lib, "lib/win32/openvr_api.lib")
#endif

#define blog(log_level, message, ...) \
		blog(log_level, "[win_openvr] " message, ##__VA_ARGS__)

#define debug(message, ...) \
		blog(LOG_DEBUG, "[%s] " message, obs_source_get_name(context->source), ##__VA_ARGS__)
#define info(message, ...) \
		blog(LOG_INFO, "[%s] " message, obs_source_get_name(context->source), ##__VA_ARGS__)
#define warn(message, ...) \
		blog(LOG_WARNING, "[%s] " message, obs_source_get_name(context->source), ##__VA_ARGS__)

struct croppreset {
	char name[128];
	float top;
	float left;
	float bottom;
	float right;
};
std::vector<croppreset> croppresets;

struct win_openvr {
	obs_source_t *source;

	bool         righteye;
	int          croppreset;
	float        croptop;
	float        cropleft;
	float        cropbottom;
	float        cropright;

	gs_texture_t *texture;
	ID3D11Device *dev11;
	ID3D11DeviceContext *ctx11;
	ID3D11Resource* tex;
	ID3D11ShaderResourceView *mirrorSrv;

	ID3D11Texture2D *texCrop;

	DWORD lastCheckTick;

	int x;
	int y;
	int width;
	int height;

	bool initialized;
	bool active;
};

bool IsVRSystemInitialized = false;

static void win_openvr_init(void *data, bool forced = false)
{
	struct win_openvr *context = (win_openvr*)data;

	if (context->initialized)
		return;

	// Dont attempt to init OVR too often due to memory leak in VR_Init
	// TODO: OpenVR v1.10.30 should no longer have the memory leak
	if (GetTickCount() - 1000 < context->lastCheckTick && !forced)
	{
		return;
	}

	// Init OpenVR, create D3D11 device and get shared mirror texture
	vr::EVRInitError err = vr::VRInitError_None;
	vr::VR_Init(&err, vr::VRApplication_Background);
	if (err != vr::VRInitError_None)
	{
		debug("OpenVR not available");
		// OpenVR not available
		context->lastCheckTick = GetTickCount();
		return;
	}
	IsVRSystemInitialized = true;

	HRESULT hr;
	D3D_FEATURE_LEVEL featureLevel;
	hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, 0, 0, 0, 0, D3D11_SDK_VERSION, &context->dev11, &featureLevel, &context->ctx11);
	if (FAILED(hr)) {
		warn("win_openvr_show: D3D11CreateDevice failed");
		return;
	}

	vr::VRCompositor()->GetMirrorTextureD3D11(context->righteye ? vr::Eye_Right : vr::Eye_Left, context->dev11, (void**)&context->mirrorSrv);
	if (!context->mirrorSrv)
	{
		warn("win_openvr_show: GetMirrorTextureD3D11 failed");
		return;
	}

	// Get ID3D11Resource from shader resource view
	context->mirrorSrv->GetResource(&context->tex);
	if (!context->tex)
	{
		warn("win_openvr_show: GetResource failed");
		return;
	}

	// Get the size from Texture2D
	ID3D11Texture2D *tex2D;
	context->tex->QueryInterface<ID3D11Texture2D>(&tex2D);
	if (!tex2D)
	{
		warn("win_openvr_show: QueryInterface failed");
		return;
	}

	D3D11_TEXTURE2D_DESC desc;
	tex2D->GetDesc(&desc);

	// Apply wanted cropping to size
	context->x = (int)(desc.Width * context->cropleft);
	context->y = (int)(desc.Height * context->croptop);
	desc.Width = (int)(desc.Width * context->cropright - context->x);
	desc.Height = (int)(desc.Height * context->cropbottom - context->y);

	context->width = desc.Width;
	context->height = desc.Height;

	tex2D->Release();

	// Create cropped, linear texture
	// Using linear here will cause correct sRGB gamma to be applied
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	hr = context->dev11->CreateTexture2D(&desc, NULL, &context->texCrop);
	if (FAILED(hr)) {
		warn("win_openvr_show: CreateTexture2D failed");
		return;
	}

	// Get IDXGIResource, then share handle, and open it in OBS device
	IDXGIResource *res;
	hr = context->texCrop->QueryInterface(__uuidof(IDXGIResource), (void**)&res);
	if (FAILED(hr)) {
		warn("win_openvr_show: QueryInterface failed");
		return;
	}

	HANDLE handle = NULL;
	hr = res->GetSharedHandle(&handle);
	if (FAILED(hr)) {
		warn("win_openvr_show: GetSharedHandle failed");
		return;
	}
	res->Release();

	obs_enter_graphics();
	gs_texture_destroy(context->texture);
	context->texture = gs_texture_open_shared((uint32_t)handle);
	obs_leave_graphics();

	context->initialized = true;
}

static void win_openvr_deinit(void *data)
{
	struct win_openvr *context = (win_openvr*)data;

	context->initialized = false;

	if (context->texture) {
		obs_enter_graphics();
		gs_texture_destroy(context->texture);
		obs_leave_graphics();
		context->texture = NULL;
	}

	if (context->tex)
		context->tex->Release();
	if (context->texCrop)
		context->texCrop->Release();
	//  if (context->mirrorSrv)
	//vr::VRCompositor()->ReleaseMirrorTextureD3D11(context->mirrorSrv);
	//context->mirrorSrv->Release();

	if (IsVRSystemInitialized)
	{
		IsVRSystemInitialized = false;
		vr::VR_Shutdown(); // Releases mirrorSrv
	}

	if (context->ctx11)
		context->ctx11->Release();
	if (context->dev11)
	{
		if (context->dev11->Release() != 0)
		{
			warn("win_openvr_deinit: device refcount not zero!");
		}
	}

	context->ctx11 = NULL;
	context->dev11 = NULL;
	context->tex = NULL;
	context->mirrorSrv = NULL;
	context->texCrop = NULL;
}

static const char *win_openvr_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "OpenVR Capture";
}

static void win_openvr_update(void *data, obs_data_t *settings)
{
	struct win_openvr *context = (win_openvr*)data;
	context->righteye = obs_data_get_bool(settings, "righteye");

	context->cropleft = (float)obs_data_get_double(settings, "cropleft");
	context->cropright = (float)obs_data_get_double(settings, "cropright");
	context->croptop = (float)obs_data_get_double(settings, "croptop");
	context->cropbottom = (float)obs_data_get_double(settings, "cropbottom");

	if (context->initialized)
	{
		win_openvr_deinit(data);
		win_openvr_init(data);
	}
}

static void win_openvr_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "righteye", true);
	obs_data_set_default_double(settings, "cropleft", 0.0f);
	obs_data_set_default_double(settings, "cropright", 1.0f);
	obs_data_set_default_double(settings, "croptop", 0.0f);
	obs_data_set_default_double(settings, "cropbottom", 1.0f);
}

static uint32_t win_openvr_getwidth(void *data)
{
	struct win_openvr *context = (win_openvr*)data;
	return context->width;
}

static uint32_t win_openvr_getheight(void *data)
{
	struct win_openvr *context = (win_openvr*)data;
	return context->height;
}

static void win_openvr_show(void *data)
{
	win_openvr_init(data, true); // When showing do forced init without delay
}

static void win_openvr_hide(void *data)
{
	win_openvr_deinit(data);
}

static void *win_openvr_create(obs_data_t *settings, obs_source_t *source)
{
	struct win_openvr *context = (win_openvr*)bzalloc(sizeof(win_openvr));
	context->source = source;

	context->initialized = false;

	context->ctx11 = NULL;
	context->dev11 = NULL;
	context->tex = NULL;
	context->texture = NULL;
	context->texCrop = NULL;

	context->width = context->height = 100;

	win_openvr_update(context, settings);
	return context;
}

static void win_openvr_destroy(void *data)
{
	struct win_openvr *context = (win_openvr*)data;

	win_openvr_deinit(data);
	bfree(context);
}

static void win_openvr_render(void *data, gs_effect_t *effect)
{
	struct win_openvr *context = (win_openvr*)data;

	if (context->active && !context->initialized)
	{
		// Active & want to render but not initialized - attempt to init
		win_openvr_init(data);
	}

	if (!context->texture || !context->active)
	{
		return;
	}

	// Crop from full size mirror texture
	// This step is required even without cropping as the full res mirror texture is in sRGB space
	D3D11_BOX poksi = {
		(UINT)context->x,
		(UINT)context->y,
		0,
		(UINT)context->x + context->width,
		(UINT)context->y + context->height,
		1,
	};
	context->ctx11->CopySubresourceRegion(context->texCrop, 0, 0, 0, 0, context->tex, 0, &poksi);
	context->ctx11->Flush();

	// Draw from OpenVR shared mirror texture
	effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);

	while (gs_effect_loop(effect, "Draw"))
	{
		obs_source_draw(context->texture, 0, 0, 0, 0, false);
	}
}

static void win_openvr_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);

	struct win_openvr *context = (win_openvr*)data;

	context->active = obs_source_active(context->source);

	if (context->initialized)
	{
		vr::VREvent_t e;

		if ((vr::VRSystem() != NULL) && (IsVRSystemInitialized))
		{
			if (vr::VRSystem()->PollNextEvent(&e, sizeof(vr::VREvent_t)))
			{
				if (e.eventType == vr::VREvent_Quit)
				{
					//vr::VRSystem()->AcknowledgeQuit_Exiting();
					//vr::VRSystem()->AcknowledgeQuit_UserPrompt();

					// Without this SteamVR will kill OBS process when it exits
					win_openvr_deinit(data);
				}
			}
		}
		else if (context->active)
		{
			context->initialized = false;
			win_openvr_init(data);
		}

	}
}

static bool crop_preset_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);

	int sel = (int)obs_data_get_int(s, "croppreset") - 1;

	if (sel >= croppresets.size() || sel < 0)
		return false;

	bool flip = obs_data_get_bool(s, "righteye");

	// Mirror preset horizontally if right eye is captured
	obs_data_set_double(s, "cropleft", flip ? 1 - croppresets[sel].right : croppresets[sel].left);
	obs_data_set_double(s, "cropright", flip ? 1 - croppresets[sel].left : croppresets[sel].right);
	obs_data_set_double(s, "croptop", croppresets[sel].top);
	obs_data_set_double(s, "cropbottom", croppresets[sel].bottom);

	return true;
}

static bool crop_preset_manual(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);

	if (obs_data_get_int(s, "croppreset") != 0)
	{
		// Slider moved manually, disable preset
		obs_data_set_int(s, "croppreset", 0);
		return true;
	}
	return false;
}

static bool crop_preset_flip(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	int pi = (int)obs_data_get_int(s, "croppreset");
	if (pi != 0)
	{
		// Changed to capture other eye, flip crop preset
		obs_data_set_int(s, "croppreset", pi);
		crop_preset_changed(props, p, s);
		return true;
	}
	return true;
}

static obs_properties_t *win_openvr_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_bool(props, "righteye", obs_module_text("RightEye"));
	obs_property_set_modified_callback(p, crop_preset_flip);

	p = obs_properties_add_list(props, "croppreset", obs_module_text("Preset"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "none", 0);
	int i = 1;
	for (const auto c : croppresets)
	{
		obs_property_list_add_int(p, c.name, i++);
	}
	obs_property_set_modified_callback(p, crop_preset_changed);

	p = obs_properties_add_float_slider(props, "croptop", obs_module_text("CropTop"), 0.0, 0.5, 0.01);
	obs_property_set_modified_callback(p, crop_preset_manual);
	p = obs_properties_add_float_slider(props, "cropbottom", obs_module_text("CropBottom"), 0.5, 1.0, 0.01);
	obs_property_set_modified_callback(p, crop_preset_manual);
	p = obs_properties_add_float_slider(props, "cropleft", obs_module_text("CropLeft"), 0.0, 0.5, 0.01);
	obs_property_set_modified_callback(p, crop_preset_manual);
	p = obs_properties_add_float_slider(props, "cropright", obs_module_text("CropRight"), 0.5, 1.0, 0.01);
	obs_property_set_modified_callback(p, crop_preset_manual);

	return props;
}

static void load_presets(void)
{
	char *presets_file = NULL;
	presets_file = obs_module_file("win-openvr-presets.ini");
	if (presets_file)
	{
		FILE *f = fopen(presets_file, "rb");
		if (f)
		{
			croppreset p = { 0 };
			while (fscanf(f, "%f,%f,%f,%f,%[^\n]\n", &p.top, &p.bottom, &p.left, &p.right, p.name) > 0)
			{
				croppresets.push_back(p);
			}
			fclose(f);
		}
		else
		{
			blog(LOG_WARNING, "Failed to load presets file 'win-openvr-presets.ini' not found!");
		}
		bfree(presets_file);
	}
	else
	{
		blog(LOG_WARNING, "Failed to load presets file 'win-openvr-presets.ini' not found!");
	}
}


OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("win-openvr", "en-US")

bool obs_module_load(void)
{
	obs_source_info info	= {};
	info.id			= "openvr_capture";
	info.type		= OBS_SOURCE_TYPE_INPUT;
	info.output_flags	= OBS_SOURCE_VIDEO |
				  OBS_SOURCE_CUSTOM_DRAW;
	info.get_name		= win_openvr_get_name;
	info.create		= win_openvr_create;
	info.destroy		= win_openvr_destroy;
	info.update		= win_openvr_update;
	info.get_defaults	= win_openvr_defaults;
	info.show		= win_openvr_show;
	info.hide		= win_openvr_hide;
	info.get_width		= win_openvr_getwidth;
	info.get_height		= win_openvr_getheight;
	info.video_render	= win_openvr_render;
	info.video_tick		= win_openvr_tick;
	info.get_properties	= win_openvr_properties;
	obs_register_source(&info);
	load_presets();
	return true;
}
