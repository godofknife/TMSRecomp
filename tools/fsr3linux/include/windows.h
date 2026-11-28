// FSR 3 for Linux (s131): the FidelityFX SDK includes <windows.h> in a few files (ffx_assert.cpp, ffx_message.cpp)
// for OutputDebugString only; ffx_linux_compat.h (force-included) provides it. Nothing else here.
#pragma once
