///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// d3dcomp.cpp                                                               //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
// Licensed under the MIT license. See COPYRIGHT in the project root for     //
// full license information.                                                 //
//                                                                           //
// Provides functions to bridge from d3dcompiler_47 to dxcompiler.           //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include "dxc/Support/WinIncludes.h"
#include "dxc/Support/Global.h"
#include "dxc/HLSL/DxilContainer.h"
#include "dxc/dxcapi.h"

#include <d3dcompiler.h>
#include <vector>
#include <string>
#include <memory>

thread_local std::unique_ptr<IDxcCompiler> g_tlsCompiler;
thread_local std::unique_ptr<IDxcLibrary>  g_tlsLibrary;

HRESULT CreateContainerReflection(IDxcContainerReflection **ppReflection) {
  return DxcCreateInstance(CLSID_DxcContainerReflection,
                           __uuidof(IDxcContainerReflection),
                           (void **)ppReflection);
}

HRESULT CompileFromBlob(IDxcBlobEncoding *pSource, LPCWSTR pSourceName,
                        const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude,
                        LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1,
                        UINT Flags2, ID3DBlob **ppCode,
                        ID3DBlob **ppErrorMsgs) {
  CComPtr<IDxcOperationResult> operationResult;
  HRESULT hr;

  // Upconvert legacy targets
  char Target[7] = "?s_6_0";
  Target[6] = 0;
  Target[0] = pTarget[0];

  try {
    CA2W pEntrypointW(pEntrypoint);
    CA2W pTargetProfileW(Target);
    std::vector<std::wstring> defineValues;
    std::vector<DxcDefine> defines;
    if (pDefines) {
      CONST D3D_SHADER_MACRO *pCursor = pDefines;

      // Convert to UTF-16.
      while (pCursor->Name) {
        defineValues.push_back(std::wstring(CA2W(pCursor->Name)));
        if (pCursor->Definition)
          defineValues.push_back(std::wstring(CA2W(pCursor->Definition)));
        else
          defineValues.push_back(std::wstring());
        ++pCursor;
      }

      // Build up array.
      pCursor = pDefines;
      size_t i = 0;
      while (pCursor->Name) {
        defines.push_back(
            DxcDefine{defineValues[i++].c_str(), defineValues[i++].c_str()});
        ++pCursor;
      }
    }

    std::vector<LPCWSTR> arguments;
    // /Gec, /Ges Not implemented:
    //if(Flags1 & D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY) arguments.push_back(L"/Gec");
    //if(Flags1 & D3DCOMPILE_ENABLE_STRICTNESS) arguments.push_back(L"/Ges");
    if(Flags1 & D3DCOMPILE_IEEE_STRICTNESS) arguments.push_back(L"/Gis");
    if(Flags1 & D3DCOMPILE_OPTIMIZATION_LEVEL2)
    {
      switch(Flags1 & D3DCOMPILE_OPTIMIZATION_LEVEL2)
      {
      case D3DCOMPILE_OPTIMIZATION_LEVEL0: arguments.push_back(L"/O0"); break;
      case D3DCOMPILE_OPTIMIZATION_LEVEL2: arguments.push_back(L"/O2"); break;
      case D3DCOMPILE_OPTIMIZATION_LEVEL3: arguments.push_back(L"/O3"); break;
      }
    }
    // Currently, /Od turns off too many optimization passes, causing incorrect DXIL to be generated.
    // Re-enable once /Od is implemented properly:
    //if(Flags1 & D3DCOMPILE_SKIP_OPTIMIZATION) arguments.push_back(L"/Od");
    if(Flags1 & D3DCOMPILE_DEBUG) arguments.push_back(L"/Zi");
    if(Flags1 & D3DCOMPILE_PACK_MATRIX_ROW_MAJOR) arguments.push_back(L"/Zpr");
    if(Flags1 & D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR) arguments.push_back(L"/Zpc");
    if(Flags1 & D3DCOMPILE_AVOID_FLOW_CONTROL) arguments.push_back(L"/Gfa");
    if(Flags1 & D3DCOMPILE_PREFER_FLOW_CONTROL) arguments.push_back(L"/Gfp");
    // We don't implement this:
    //if(Flags1 & D3DCOMPILE_PARTIAL_PRECISION) arguments.push_back(L"/Gpp");
    if(Flags1 & D3DCOMPILE_RESOURCES_MAY_ALIAS) arguments.push_back(L"/res_may_alias");

    IFR(g_tlsCompiler->Compile(pSource, pSourceName, pEntrypointW, pTargetProfileW,
                          arguments.data(), (UINT)arguments.size(),
                          defines.data(), (UINT)defines.size(), nullptr,
                          &operationResult));
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  } catch (const CAtlException &err) {
    return err.m_hr;
  }

  operationResult->GetStatus(&hr);
  if (SUCCEEDED(hr)) {
    return operationResult->GetResult((IDxcBlob **)ppCode);
  } else {
    if (ppErrorMsgs)
      operationResult->GetErrorBuffer((IDxcBlobEncoding **)ppErrorMsgs);
    return hr;
  }
}

HRESULT WINAPI BridgeD3DCompile(LPCVOID pSrcData, SIZE_T SrcDataSize,
                                LPCSTR pSourceName,
                                const D3D_SHADER_MACRO *pDefines,
                                ID3DInclude *pInclude, LPCSTR pEntrypoint,
                                LPCSTR pTarget, UINT Flags1, UINT Flags2,
                                ID3DBlob **ppCode, ID3DBlob **ppErrorMsgs) {
  CComPtr<IDxcBlobEncoding> source;

  *ppCode = nullptr;
  if (ppErrorMsgs != nullptr)
    *ppErrorMsgs = nullptr;

  IFR(g_tlsLibrary->CreateBlobWithEncodingFromPinned((LPBYTE)pSrcData, SrcDataSize,
                                                CP_ACP, &source));

  try {
    CA2W pFileName(pSourceName);
    return CompileFromBlob(source, pFileName, pDefines, pInclude, pEntrypoint,
                           pTarget, Flags1, Flags2, ppCode, ppErrorMsgs);
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  } catch (const CAtlException &err) {
    return err.m_hr;
  }
}

HRESULT WINAPI BridgeD3DCompile2(
    LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
    const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude, LPCSTR pEntrypoint,
    LPCSTR pTarget, UINT Flags1, UINT Flags2, UINT SecondaryDataFlags,
    LPCVOID pSecondaryData, SIZE_T SecondaryDataSize, ID3DBlob **ppCode,
    ID3DBlob **ppErrorMsgs) {
  if (SecondaryDataFlags == 0 || pSecondaryData == nullptr) {
    return BridgeD3DCompile(pSrcData, SrcDataSize, pSourceName, pDefines,
                            pInclude, pEntrypoint, pTarget, Flags1, Flags2,
                            ppCode, ppErrorMsgs);
  }
  return E_NOTIMPL;
}

HRESULT WINAPI BridgeD3DCompileFromFile(
    LPCWSTR pFileName, const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude,
    LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2,
    ID3DBlob **ppCode, ID3DBlob **ppErrorMsgs) {
  CComPtr<IDxcBlobEncoding> source;
  HRESULT hr;

  *ppCode = nullptr;
  if (ppErrorMsgs != nullptr)
    *ppErrorMsgs = nullptr;

  hr = g_tlsLibrary->CreateBlobFromFile(pFileName, nullptr, &source);
  if (FAILED(hr))
    return hr;

  return CompileFromBlob(source, pFileName, pDefines, pInclude, pEntrypoint,
                         pTarget, Flags1, Flags2, ppCode, ppErrorMsgs);
}

HRESULT WINAPI BridgeD3DDisassemble(
  _In_reads_bytes_(SrcDataSize) LPCVOID pSrcData,
  _In_ SIZE_T SrcDataSize,
  _In_ UINT Flags,
  _In_opt_ LPCSTR szComments,
  _Out_ ID3DBlob** ppDisassembly) {
  CComPtr<IDxcBlobEncoding> source;
  CComPtr<IDxcBlobEncoding> disassemblyText;

  *ppDisassembly = nullptr;

  UNREFERENCED_PARAMETER(szComments);
  UNREFERENCED_PARAMETER(Flags);

  IFR(g_tlsLibrary->CreateBlobWithEncodingFromPinned((LPBYTE)pSrcData, SrcDataSize,
                                                CP_ACP, &source));

  IFR(g_tlsCompiler->Disassemble(source, &disassemblyText));
  IFR(disassemblyText.QueryInterface(ppDisassembly));

  return S_OK;
}

HRESULT WINAPI BridgeD3DReflect(
  _In_reads_bytes_(SrcDataSize) LPCVOID pSrcData,
  _In_ SIZE_T SrcDataSize,
  _In_ REFIID pInterface,
  _Out_ void** ppReflector) {
  CComPtr<IDxcBlobEncoding> source;
  CComPtr<IDxcContainerReflection> reflection;
  UINT shaderIdx;

  *ppReflector = nullptr;

  IFR(g_tlsLibrary->CreateBlobWithEncodingOnHeapCopy((LPBYTE)pSrcData, SrcDataSize,
                                                CP_ACP, &source));
  IFR(CreateContainerReflection(&reflection));
  IFR(reflection->Load(source));
  IFR(reflection->FindFirstPartKind(hlsl::DFCC_DXIL, &shaderIdx));
  IFR(reflection->GetPartReflection(shaderIdx, pInterface,
                                    (void **)ppReflector));
  return S_OK;
}


HRESULT CreateLibrary(IDxcLibrary **pLibrary) {
    return DxcCreateInstance(CLSID_DxcLibrary, __uuidof(IDxcLibrary),
        (void **)pLibrary);
}

HRESULT CreateCompiler(IDxcCompiler **ppCompiler) {
    return DxcCreateInstance(CLSID_DxcCompiler, __uuidof(IDxcCompiler),
        (void **)ppCompiler);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD Reason, LPVOID) {
  BOOL result = TRUE;
  if (Reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hinstDLL);
    IDxcCompiler *tmpCompiler;
    IFR(CreateCompiler(&tmpCompiler));
    g_tlsCompiler.reset(tmpCompiler);

    IDxcLibrary *tmpLibrary;
    IFR(CreateLibrary(&tmpLibrary));
    g_tlsLibrary.reset(tmpLibrary);


  } else if (Reason == DLL_PROCESS_DETACH) {
    // Nothing to clean-up.
  }

  return result;
}
