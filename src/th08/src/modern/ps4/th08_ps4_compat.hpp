#pragma once

#ifndef TH_PS4
#error "This header is only for the modern PS4 build."
#endif

// Force-included (-include) into every reconstructed TH08 translation unit of the PS4
// target, exactly like src/modern/linux/linux_compat.hpp is for the Linux one. The Win32
// and Direct3D 8 declarations it pulls in are the portable shims under
// src/modern/linux/include; only their implementations differ per platform.
// The freegnm helpers this target also builds are C, and none of the declarations below
// are for them.
#ifdef __cplusplus

#include <strings.h>
#include <windows.h>
#include <mmsystem.h>
#include <d3d8.h>

// OpenOrbis' libc does not always expose this, and the POSIX adapter sizes path buffers
// with it.
#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#define _snwprintf swprintf
#define D3D_WRAPPER 0
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_RETURN 0x0d
#define VK_ESCAPE 0x1b
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69

#define TRANSPARENT 1
#define BI_RGB 0
#define DIB_RGB_COLORS 0
#define FW_NORMAL 400
#define FW_BOLD 700
#define FW_SEMIBOLD 600
#define DEFAULT_CHARSET 1
#define SHIFTJIS_CHARSET 128
#define OUT_DEFAULT_PRECIS 0
#define CLIP_DEFAULT_PRECIS 0
#define ANTIALIASED_QUALITY 4
#define DEFAULT_PITCH 0
#define FIXED_PITCH 1
#define FF_ROMAN 16

typedef LPDIRECT3D8 PDIRECT3D8;
typedef LPDIRECT3DDEVICE8 PDIRECT3DDEVICE8;

#endif // __cplusplus
