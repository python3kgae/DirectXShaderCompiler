///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// dxexp.cpp                                                                 //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
// This file is distributed under the University of Illinois Open Source     //
// License. See LICENSE.TXT for details.                                     //
//                                                                           //
// Provides a command-line tool to detect the status of D3D experimental     //
// feature support for experimental shaders.                                 //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <atlbase.h>
#include <stdio.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

// A more recent Windows SDK than currently required is needed for these.
typedef HRESULT (WINAPI *D3D12EnableExperimentalFeaturesFn)(
    UINT                                    NumFeatures,
    __in_ecount(NumFeatures) const IID*     pIIDs,
    __in_ecount_opt(NumFeatures) void*      pConfigurationStructs,
    __in_ecount_opt(NumFeatures) UINT*      pConfigurationStructSizes);

static const GUID D3D12ExperimentalShaderModelsID = { /* 76f5573e-f13a-40f5-b297-81ce9e18933f */
    0x76f5573e,
    0xf13a,
    0x40f5,
    { 0xb2, 0x97, 0x81, 0xce, 0x9e, 0x18, 0x93, 0x3f }
};

static HRESULT AtlCheck(HRESULT hr) {
  if (FAILED(hr))
    AtlThrow(hr);
  return hr;
}

static char *BoolToStr(bool value) {
  return value ? "YES" : "NO";
}

static void PrintAdapters() {
  try {
    CComPtr<IDXGIFactory2> pFactory;
    AtlCheck(CreateDXGIFactory2(0, IID_PPV_ARGS(&pFactory)));
    UINT AdapterIndex = 0;
    for (;;) {
      CComPtr<IDXGIAdapter1> pAdapter;
      CComPtr<ID3D12Device> pDevice;
      HRESULT hrEnum = pFactory->EnumAdapters1(AdapterIndex, &pAdapter);
      if (hrEnum == DXGI_ERROR_NOT_FOUND)
        break;
      AtlCheck(hrEnum);
      DXGI_ADAPTER_DESC1 AdapterDesc;
      D3D12_FEATURE_DATA_D3D12_OPTIONS1 DeviceOptions;
      D3D12_FEATURE_DATA_SHADER_MODEL DeviceSM;
      AtlCheck(pAdapter->GetDesc1(&AdapterDesc));
      AtlCheck(D3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pDevice)));
      AtlCheck(pDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &DeviceOptions, sizeof(DeviceOptions)));
      DeviceSM.HighestShaderModel = D3D_SHADER_MODEL_6_0;
      AtlCheck(pDevice->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &DeviceSM, sizeof(DeviceSM)));
      printf("%S - SM6 [%s] Wave [%s] I64 [%s]\n", AdapterDesc.Description,
             BoolToStr(DeviceSM.HighestShaderModel >= D3D_SHADER_MODEL_6_0),
             BoolToStr(DeviceOptions.WaveOps),
             BoolToStr(DeviceOptions.Int64ShaderOps));
      AdapterIndex++;
    }
  }
  catch (ATL::CAtlException &) {
    printf("%s", "Unable to print information for adapters.\n");
  }
}


// Return codes:
// 0 - experimental mode worked
// 1 - cannot load d3d12.dll
// 2 - cannot find D3D12EnableExperimentalFeatures
// 3 - experimental shader mode interface unsupported
// 4 - other error
int main(int argc, const char *argv[]) {
  if (argc > 1) {
    const char *pArg = argv[1];
    if (0 == strcmp(pArg, "-?") || 0 == strcmp(pArg, "/?") || 0 == strcmp(pArg, "/?")) {
      printf("Checks the available of D3D support for experimental shader models.\n\n");
      printf("dxexp\n\n");
      printf("Sets errorlevel to 0 on success, non-zero for failure cases.\n");
    }
    else {
      printf("Unrecognized command line arguments.\n");
    }
    return 4;
  }

  DWORD err;
  HMODULE hRuntime;
  hRuntime = LoadLibraryW(L"d3d12.dll");
  if (hRuntime == NULL) {
    err = GetLastError();
    printf("Failed to load library d3d12.dll - Win32 error %u\n", err);
    return 1;
  }

  D3D12EnableExperimentalFeaturesFn pD3D12EnableExperimentalFeatures =
    (D3D12EnableExperimentalFeaturesFn)GetProcAddress(hRuntime, "D3D12EnableExperimentalFeatures");
  if (pD3D12EnableExperimentalFeatures == nullptr) {
    err = GetLastError();
    printf("Failed to find export 'D3D12EnableExperimentalFeatures' in "
           "d3d12.dll - Win32 error %u%s\n", err,
           err == ERROR_PROC_NOT_FOUND ? " (The specified procedure could not be found.)" : "");
    printf("Consider verifying the operating system version - Creators Update or newer "
           "is currently required.\n");
    PrintAdapters();
    return 2;
  }

  HRESULT hr = pD3D12EnableExperimentalFeatures(1, &D3D12ExperimentalShaderModelsID, nullptr, nullptr);
  if (SUCCEEDED(hr)) {
    printf("Experimental shader model feature succeeded.\n");
    PrintAdapters();
    return 0;
  }
  else if (hr == E_NOINTERFACE) {
    printf("Experimental shader model feature failed with error E_NOINTERFACE.\n");
    printf("The most likely cause is that Windows Developer mode is not on.\n");
    printf("See https://msdn.microsoft.com/en-us/windows/uwp/get-started/enable-your-device-for-development\n");
    return 3;
  }
  else if (hr == E_INVALIDARG) {
    printf("Experimental shader model feature failed with error E_INVALIDARG.\n");
    printf("This means the configuration of a feature is incorrect, the set of features passed\n"
      "in are known to be incompatible with each other, or other errors occured,\n"
      "and is generally unexpected for the experimental shader model feature.\n");
    return 4;
  }
  else {
    printf("Experimental shader model feature failed with unexpected HRESULT 0x%08x.\n", hr);
    return 4;
  }
}
