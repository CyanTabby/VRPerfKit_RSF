#include "openvr_hooks.h"
#include "hooks.h"
#include "logging.h"
#include "win_header_sane.h"
#include "openvr.h"
#include "openvr_manager.h"
#include "resolution_scaling.h"

namespace vrperfkit {
	extern HMODULE g_module;

	namespace {
		void *g_clientCoreInstance = nullptr;
		int g_compositorVersion = 0;
		int g_systemVersion = 0;

		void IVRSystemHook_GetRecommendedRenderTargetSize(vr::IVRSystem *self, uint32_t *pnWidth, uint32_t *pnHeight) {
			hooks::CallOriginal(IVRSystemHook_GetRecommendedRenderTargetSize)(self, pnWidth, pnHeight);

			if (pnWidth == nullptr || pnHeight == nullptr) {
				return;
			}

			AdjustRenderResolution(*pnWidth, *pnHeight);
		}

		vr::EVRCompositorError IVRCompositor009Hook_Submit(vr::EVREye eEye, const vr::Texture_t *pTexture, const vr::VRTextureBounds_t *pBounds, vr::EVRSubmitFlags nSubmitFlags) {
			OpenVrSubmitInfo info { eEye, pTexture, pBounds, nSubmitFlags };
			g_openVr.OnSubmit(info);
			auto error = hooks::CallOriginal(IVRCompositor009Hook_Submit)(info.eye, info.texture, info.bounds, info.submitFlags);
			return error;
		}

		vr::EVRCompositorError IVRCompositor008Hook_Submit(vr::EVREye eEye, unsigned int eTextureType, void *pTexture, const vr::VRTextureBounds_t *pBounds, vr::EVRSubmitFlags nSubmitFlags) {
			vr::Texture_t texInfo { pTexture, (vr::ETextureType)eTextureType, vr::ColorSpace_Auto };
			OpenVrSubmitInfo info { eEye, &texInfo, pBounds, nSubmitFlags };
			g_openVr.OnSubmit(info);
			auto error = hooks::CallOriginal(IVRCompositor008Hook_Submit)(info.eye, info.texture->eType, info.texture->handle, info.bounds, info.submitFlags);
			return error;
		}

		vr::EVRCompositorError IVRCompositor007Hook_Submit(vr::EVREye eEye, unsigned int eTextureType, void *pTexture, const vr::VRTextureBounds_t *pBounds) {
			vr::Texture_t texInfo { pTexture, (vr::ETextureType)eTextureType, vr::ColorSpace_Auto };
			OpenVrSubmitInfo info { eEye, &texInfo, pBounds, vr::Submit_Default };
			g_openVr.OnSubmit(info);
			auto error = hooks::CallOriginal(IVRCompositor007Hook_Submit)(info.eye, info.texture->eType, info.texture->handle, info.bounds);
			return error;
		}

		void *Hook_VRClientCoreFactory(const char *pInterfaceName, int *pReturnCode) {
			void *instance = hooks::CallOriginal(Hook_VRClientCoreFactory)(pInterfaceName, pReturnCode);
			HookOpenVrInterface(pInterfaceName, instance);
			return instance;
		}

		void *IVRClientCoreHook_GetGenericInterface(void *self, const char *interfaceName, vr::EVRInitError *error) {
			void *instance = hooks::CallOriginal(IVRClientCoreHook_GetGenericInterface)(self, interfaceName, error);
			HookOpenVrInterface(interfaceName, instance);
			return instance;
		}

		void IVRClientCoreHook_Cleanup(void *self) {
			hooks::CallOriginal(IVRClientCoreHook_Cleanup)(self);
			LOG_INFO << "IVRClientCore::Cleanup was called, deleting hooks...";
			hooks::RemoveHook((void*)&IVRClientCoreHook_GetGenericInterface);
			hooks::RemoveHook((void*)&IVRClientCoreHook_Cleanup);
			hooks::RemoveHook((void*)IVRCompositor009Hook_Submit);
			hooks::RemoveHook((void*)IVRCompositor008Hook_Submit);
			hooks::RemoveHook((void*)IVRCompositor007Hook_Submit);
			hooks::RemoveHook((void*)IVRSystemHook_GetRecommendedRenderTargetSize);
			g_compositorVersion = 0;
			g_systemVersion = 0;
		}
	}

	void InstallOpenVrHooks() {
		static bool hooksLoaded = false;
		if (hooksLoaded) {
			return;
		}

#ifdef WIN64
		std::wstring dllName = L"vrclient_x64.dll";
#else
		std::wstring dllName = L"vrclient.dll";
#endif
		HMODULE handle;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, dllName.c_str(), &handle) || handle == g_module) {
			return;
		}

		LOG_INFO << dllName << " is loaded in the process, installing hooks...";
		hooks::InstallHookInDll("VRClientCoreFactory", handle, (void*)Hook_VRClientCoreFactory);

		hooksLoaded = true;
	}

	void HookOpenVrInterface(const char *interfaceName, void *instance) {
		LOG_INFO << "OpenVR: requested interface " << interfaceName;
		if (instance == nullptr) {
			return;
		}

		if (unsigned int version = 0; std::sscanf(interfaceName, "IVRClientCore_%u", &version)) {
			hooks::RemoveHook((void*)&IVRClientCoreHook_Cleanup);
			hooks::RemoveHook((void*)&IVRClientCoreHook_GetGenericInterface);
			if (version <= 3) {
				hooks::InstallVirtualFunctionHook("IVRClientCore::GetGenericInterface", instance, 3, (void*)&IVRClientCoreHook_GetGenericInterface);
				hooks::InstallVirtualFunctionHook("IVRClientCore::Cleanup", instance, 1, (void*)&IVRClientCoreHook_Cleanup);
				g_clientCoreInstance = instance;
			}
			else {
				LOG_ERROR << "Don't know how to inject into version " << version << " of IVRClientCore";
			}
		}

		if (g_compositorVersion == 0 && std::sscanf(interfaceName, "IVRCompositor_%u", &g_compositorVersion)) {
			if (g_compositorVersion >= 9) {
				uint32_t methodPos = g_compositorVersion >= 12 ? 5 : 4;
				hooks::InstallVirtualFunctionHook("IVRCompositor::Submit", instance, methodPos, (void*)&IVRCompositor009Hook_Submit);
			}
			else if (g_compositorVersion == 8) {
				hooks::InstallVirtualFunctionHook("IVRCompositor::Submit", instance, 6, (void*)&IVRCompositor008Hook_Submit);
			}
			else if (g_compositorVersion == 7) {
				hooks::InstallVirtualFunctionHook("IVRCompositor::Submit", instance, 6, (void*)&IVRCompositor007Hook_Submit);
			}
			else {
				LOG_ERROR << "Don't know how to inject into version " << g_compositorVersion << " of IVRCompositor";
			}
		}

		if (g_systemVersion == 0 && std::sscanf(interfaceName, "IVRSystem_%u", &g_systemVersion)) {
			uint32_t methodPos = (g_systemVersion >= 9 ? 0 : 1);
			hooks::InstallVirtualFunctionHook("IVRSystem::GetRecommendedRenderTargetSize", instance, methodPos, (void*)&IVRSystemHook_GetRecommendedRenderTargetSize);
		}
	}

	vr::IVRSystem * GetOpenVrSystem() {
		if (g_clientCoreInstance == nullptr) {
			return nullptr;
		}

		vr::EVRInitError error;
		return (vr::IVRSystem*)IVRClientCoreHook_GetGenericInterface(g_clientCoreInstance, vr::IVRSystem_Version, &error);
	}
}
