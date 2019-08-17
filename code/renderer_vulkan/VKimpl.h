#ifndef VK_IMPL_H_
#define VK_IMPL_H_

/*
==========================================================

      IMPLEMENTATION PLATFORM SPECIFIC FUNCTIONS

Different platform use different libs, therefore can have
different implementation, however the interface should be
consistant.
==========================================================
*/

#define VK_NO_PROTOTYPES 1

#if defined(_WIN32) || defined(_WIN64)
#define CINTERFACE

#define VK_USE_PLATFORM_WIN32_KHR 1
#include <windows.h>

// #pragma comment(linker, "/subsystem:windows")

#elif defined(__unix__) || defined(__linux) || defined(__linux__)


#define VK_USE_PLATFORM_XCB_KHR 1
#include <xcb/xcb.h>
#endif


#include "../vulkan/vulkan.h"


void* VK_GetInstanceProcAddrImpl(void);
void VK_CreateSurfaceImpl(VkInstance hInstance, void * pCtx, VkSurfaceKHR* const pSurface);
void VK_CleanInstanceProcAddrImpl(void);
void VK_FillRequiredInstanceExtention(
	const VkExtensionProperties * const pInsExt,
	const uint32_t nInsExt,
	const char* ppInsExt[16],
    uint32_t * nExt );

#endif
