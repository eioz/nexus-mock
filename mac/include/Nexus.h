#pragma once

#include <cstdint>

#define NEXUS_API_VERSION 6
#define DL_MUMBLE_LINK "DL_MUMBLE_LINK"

enum ERenderType
{
	RT_PreRender,
	RT_Render,
	RT_PostRender,
	RT_OptionsRender
};

enum ELogLevel
{
	LOGL_CRITICAL = 1,
	LOGL_WARNING = 2,
	LOGL_INFO = 3,
	LOGL_DEBUG = 4,
	LOGL_TRACE = 5
};

enum EAddonFlags
{
	AF_None = 0
};

enum EUpdateProvider
{
	UP_None = 0,
	UP_GitHub = 2
};

struct Texture_t
{
	uint32_t Width = 0;
	uint32_t Height = 0;
	void* Resource = nullptr;
};

struct AddonVersion_t
{
	uint16_t Major = 0;
	uint16_t Minor = 0;
	uint16_t Build = 0;
	uint16_t Revision = 0;
};

struct AddonAPI_t;

using ADDON_LOAD = void (*)(AddonAPI_t*);
using ADDON_UNLOAD = void (*)();
using GUI_RENDER = void (*)();
using GUI_ADDRENDER = void (*)(ERenderType, GUI_RENDER);
using GUI_REMRENDER = void (*)(GUI_RENDER);
using LOGGER_LOG = void (*)(ELogLevel, const char*, const char*);
using PATHS_GETADDONDIR = const char* (*)(const char*);
using DATALINK_GET = void* (*)(const char*);
using INPUTBINDS_PROCESS = void (*)(const char*, bool);
using INPUTBINDS_REGISTERWITHSTRING = void (*)(const char*, INPUTBINDS_PROCESS, const char*);
using INPUTBINDS_DEREGISTER = void (*)(const char*);
using TEXTURES_RECEIVECALLBACK = void (*)(const char*, Texture_t*);
using TEXTURES_LOADFROMMEMORY = void (*)(const char*, void*, uint64_t, TEXTURES_RECEIVECALLBACK);
using QUICKACCESS_ADDSHORTCUT = void (*)(const char*, const char*, const char*, const char*, const char*);
using QUICKACCESS_GENERIC = void (*)(const char*);

struct AddonDefinition_t
{
	uint32_t Signature = 0;
	uint32_t APIVersion = 0;
	const char* Name = nullptr;
	AddonVersion_t Version;
	const char* Author = nullptr;
	const char* Description = nullptr;
	ADDON_LOAD Load = nullptr;
	ADDON_UNLOAD Unload = nullptr;
	EAddonFlags Flags = AF_None;
	EUpdateProvider Provider = UP_None;
	const char* UpdateLink = nullptr;
};

struct AddonAPI_t
{
	void* SwapChain = nullptr;
	void* ImguiContext = nullptr;
	void* ImguiMalloc = nullptr;
	void* ImguiFree = nullptr;

	GUI_ADDRENDER GUI_Register = nullptr;
	GUI_REMRENDER GUI_Deregister = nullptr;
	LOGGER_LOG Log = nullptr;

	PATHS_GETADDONDIR Paths_GetAddonDirectory = nullptr;
	DATALINK_GET DataLink_Get = nullptr;

	INPUTBINDS_REGISTERWITHSTRING InputBinds_RegisterWithString = nullptr;
	INPUTBINDS_DEREGISTER InputBinds_Deregister = nullptr;

	TEXTURES_LOADFROMMEMORY Textures_LoadFromMemory = nullptr;

	QUICKACCESS_ADDSHORTCUT QuickAccess_Add = nullptr;
	QUICKACCESS_GENERIC QuickAccess_Remove = nullptr;
};
