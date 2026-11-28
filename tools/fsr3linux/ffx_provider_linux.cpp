// FSR 3 for Linux (s131): the provider list of the FidelityFX API (replaces ffx-api/src/ffx_provider.cpp of SDK
// v1.1.4 in the Linux build). That file also asks AMD's Windows D3D12 driver (amdxc64.dll) for newer providers, which
// does not exist here; this keeps the same four functions with the built-in FSR 3.1 upscaler as the only provider.
#include "ffx_provider.h"
#include "ffx_provider_fsr3upscale.h"

static const ffxProvider* const providers[] = {&ffxProvider_FSR3Upscale::Instance};

const ffxProvider* GetffxProvider(ffxStructType_t descType, uint64_t overrideId, void*)
{
    for (const ffxProvider* p : providers)
        if ((!overrideId || p->GetId() == overrideId) && p->CanProvide(descType)) return p;
    return nullptr;
}

const ffxProvider* GetAssociatedProvider(ffxContext* context)
{
    return ((const InternalContextHeader*)(*context))->provider;
}

uint64_t GetProviderVersions(ffxStructType_t descType, void*, uint64_t capacity, uint64_t* versionIds, const char** versionNames)
{
    uint64_t count = 0;
    for (const ffxProvider* p : providers) {
        if (count >= capacity) break;
        if (!p->CanProvide(descType)) continue;
        if (versionIds) versionIds[count] = p->GetId();
        if (versionNames) versionNames[count] = p->GetVersionName();
        count++;
    }
    return count;
}

uint64_t GetProviderCount(ffxStructType_t descType, void* device)
{
    return GetProviderVersions(descType, device, UINT64_MAX, nullptr, nullptr);
}
