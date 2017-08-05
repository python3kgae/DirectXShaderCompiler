///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// dxclinker.cpp                                                             //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
// This file is distributed under the University of Illinois Open Source     //
// License. See LICENSE.TXT for details.                                     //
//                                                                           //
// Implements the Dxil Linker.                                               //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include "dxc/Support/WinIncludes.h"
#include "dxc/HLSL/DxilContainer.h"
#include "dxc/Support/ErrorCodes.h"
#include "dxc/Support/Global.h"
#include "dxc/Support/FileIOHelper.h"
#include "dxc/Support/dxcapi.impl.h"
#include "dxc/Support/microcom.h"
#include "dxc/dxcapi.h"
#include "dxillib.h"

#include "llvm/ADT/SmallVector.h"
#include <algorithm>

#include "dxc/HLSL/DxilLinker.h"
#include "dxc/HLSL/DxilValidation.h"
#include "dxc/Support/Unicode.h"
#include "dxc/Support/microcom.h"
#include "dxc/dxcapi.internal.h"
#include "dxcutil.h"
#include "clang/Basic/Diagnostic.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"

using namespace hlsl;
using namespace llvm;

// This declaration is used for the locally-linked validator.
HRESULT CreateDxcValidator(_In_ REFIID riid, _Out_ LPVOID *ppv);

class DxcLinker : public IDxcLinker, public IDxcContainerEvent {
public:
  DXC_MICROCOM_TM_ADDREF_RELEASE_IMPL()
  DXC_MICROCOM_TM_CTOR(DxcLinker)

  // Register a library with name to ref it later.
  __override HRESULT RegisterLibrary(
      _In_opt_ LPCWSTR pLibName, // Name of the library.
      _In_ IDxcBlob *pLib        // Library to add.
  );

  // Links the shader and produces a shader blob that the Direct3D runtime can
  // use.
  __override HRESULT STDMETHODCALLTYPE Link(
      _In_opt_ LPCWSTR pEntryName, // Entry point name
      _In_ LPCWSTR pTargetProfile, // shader profile to link
      _In_count_(libCount)
          const LPCWSTR *pLibNames, // Array of library names to link
      UINT32 libCount,              // Number of libraries to link
      _In_count_(argCount)
          const LPCWSTR *pArguments, // Array of pointers to arguments
      _In_ UINT32 argCount,          // Number of arguments
      _COM_Outptr_ IDxcOperationResult *
          *ppResult // Linker output status, buffer, and errors
  );

  __override HRESULT STDMETHODCALLTYPE RegisterDxilContainerEventHandler(
      IDxcContainerEventsHandler *pHandler, UINT64 *pCookie) {
    DxcThreadMalloc TM(m_pMalloc);
    DXASSERT(m_pDxcContainerEventsHandler == nullptr,
             "else events handler is already registered");
    *pCookie = 1; // Only one EventsHandler supported
    m_pDxcContainerEventsHandler = pHandler;
    return S_OK;
  };
  __override HRESULT STDMETHODCALLTYPE
  UnRegisterDxilContainerEventHandler(UINT64 cookie) {
    DxcThreadMalloc TM(m_pMalloc);
    DXASSERT(m_pDxcContainerEventsHandler != nullptr,
             "else unregister should not have been called");
    m_pDxcContainerEventsHandler.Release();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) {
    return DoBasicQueryInterface<IDxcLinker>(this, riid, ppvObject);
  }

  void Initialize() {
    m_pLinker.reset(DxilLinker::CreateLinker(m_Ctx));
  }

  ~DxcLinker() {
    // Make sure DxilLinker is released before LLVMContext.
    m_pLinker.reset();
  }

private:
  DXC_MICROCOM_TM_REF_FIELDS()
  LLVMContext m_Ctx;
  std::unique_ptr<DxilLinker> m_pLinker;
  CComPtr<IDxcContainerEventsHandler> m_pDxcContainerEventsHandler;
  std::vector<CComPtr<IDxcBlob>> m_blobs; // Keep blobs live for lazy load.
};

HRESULT
DxcLinker::RegisterLibrary(_In_opt_ LPCWSTR pLibName, // Name of the library.
                           _In_ IDxcBlob *pBlob       // Library to add.
) {
  DXASSERT(m_pLinker.get(), "else Initialize() not called or failed silently");
  DxcThreadMalloc TM(m_pMalloc);
  // Prepare UTF8-encoded versions of API values.
  CW2A pUtf8LibName(pLibName, CP_UTF8);
  // Already exist lib with same name.
  if (m_pLinker->HasLibNameRegistered(pUtf8LibName.m_psz))
    return E_INVALIDARG;

  try {
    std::unique_ptr<llvm::Module> pModule, pDebugModule;

    CComPtr<IMalloc> pMalloc;
    CComPtr<AbstractMemoryStream> pDiagStream;

    IFT(CoGetMalloc(1, &pMalloc));
    IFT(CreateMemoryStream(pMalloc, &pDiagStream));

    raw_stream_ostream DiagStream(pDiagStream);

    IFR(ValidateLoadModuleFromContainerLazy(
        pBlob->GetBufferPointer(), pBlob->GetBufferSize(), pModule,
        pDebugModule, m_Ctx, m_Ctx, DiagStream));

    if (m_pLinker->RegisterLib(pUtf8LibName.m_psz, std::move(pModule),
                               std::move(pDebugModule))) {
      m_blobs.emplace_back(pBlob);
      return S_OK;
    } else {
      return E_INVALIDARG;
    }
  } catch (hlsl::Exception &) {
    return E_INVALIDARG;
  }
}

// Links the shader and produces a shader blob that the Direct3D runtime can
// use.
HRESULT STDMETHODCALLTYPE DxcLinker::Link(
    _In_opt_ LPCWSTR pEntryName, // Entry point name
    _In_ LPCWSTR pTargetProfile, // shader profile to link
    _In_count_(libCount)
        const LPCWSTR *pLibNames, // Array of library names to link
    UINT32 libCount,              // Number of libraries to link
    _In_count_(argCount)
        const LPCWSTR *pArguments, // Array of pointers to arguments
    _In_ UINT32 argCount,          // Number of arguments
    _COM_Outptr_ IDxcOperationResult *
        *ppResult // Linker output status, buffer, and errors
) {
  DxcThreadMalloc TM(m_pMalloc);
  // Prepare UTF8-encoded versions of API values.
  CW2A pUtf8EntryPoint(pEntryName, CP_UTF8);
  CW2A pUtf8TargetProfile(pTargetProfile, CP_UTF8);
  // TODO: read and validate options.

  CComPtr<AbstractMemoryStream> pOutputStream;

  // Detach previous libraries.
  m_pLinker->DetachAll();

  HRESULT hr = S_OK;
  try {
    CComPtr<IMalloc> pMalloc;
    CComPtr<IDxcBlob> pOutputBlob;
    CComPtr<AbstractMemoryStream> pDiagStream;

    IFT(CoGetMalloc(1, &pMalloc));
    IFT(CreateMemoryStream(pMalloc, &pOutputStream));

    std::string warnings;
    llvm::raw_string_ostream w(warnings);

    {
      UINT32 majorVer, minorVer;
      dxcutil::GetValidatorVersion(&majorVer, &minorVer);
      // TODO: use validation version when link.
      (majorVer);
      (minorVer);
    }

    IFT(CreateMemoryStream(pMalloc, &pDiagStream));

    raw_stream_ostream DiagStream(pDiagStream);

    llvm::DiagnosticPrinterRawOStream DiagPrinter(DiagStream);
    PrintDiagnosticContext DiagContext(DiagPrinter);
    m_Ctx.setDiagnosticHandler(PrintDiagnosticContext::PrintDiagnosticHandler,
                               &DiagContext, true);

    // Attach libraries.
    bool bSuccess = true;
    for (unsigned i = 0; i < libCount; i++) {
      CW2A pUtf8LibName(pLibNames[i], CP_UTF8);
      bSuccess &= m_pLinker->AttachLib(pUtf8LibName.m_psz);
    }

    bool hasErrorOccurred = !bSuccess;
    if (bSuccess) {
      std::unique_ptr<Module> pM =
          m_pLinker->Link(pUtf8EntryPoint.m_psz, pUtf8TargetProfile.m_psz);
      if (pM) {
        const IntrusiveRefCntPtr<clang::DiagnosticIDs> Diags(
            new clang::DiagnosticIDs);
        IntrusiveRefCntPtr<clang::DiagnosticOptions> DiagOpts =
            new clang::DiagnosticOptions();
        // Construct our diagnostic client.
        clang::TextDiagnosticPrinter *DiagClient =
            new clang::TextDiagnosticPrinter(DiagStream, &*DiagOpts);
        clang::DiagnosticsEngine Diag(Diags, &*DiagOpts, DiagClient);

        raw_stream_ostream outStream(pOutputStream.p);
        // Create bitcode of M.
        WriteBitcodeToFile(pM.get(), outStream);
        outStream.flush();

        // Validation.
        HRESULT valHR = dxcutil::ValidateAndAssembleToContainer(
            std::move(pM), pOutputBlob, pMalloc, SerializeDxilFlags::None,
            pOutputStream,
            /*bDebugInfo*/ false, Diag);

        // Callback after valid DXIL is produced
        if (SUCCEEDED(valHR)) {
          CComPtr<IDxcBlob> pTargetBlob;
          if (m_pDxcContainerEventsHandler != nullptr) {
            HRESULT hr = m_pDxcContainerEventsHandler->OnDxilContainerBuilt(
                pOutputBlob, &pTargetBlob);
            if (SUCCEEDED(hr) && pTargetBlob != nullptr) {
              std::swap(pOutputBlob, pTargetBlob);
            }
          }
          // TODO: DFCC_ShaderDebugName
        }

        hasErrorOccurred = Diag.hasErrorOccurred();

      } else {
        hasErrorOccurred = true;
      }
    }
    DiagStream.flush();
    CComPtr<IStream> pStream = pDiagStream;
    dxcutil::CreateOperationResultFromOutputs(pOutputBlob, pStream, warnings,
                                              hasErrorOccurred, ppResult);
  }
  CATCH_CPP_ASSIGN_HRESULT();
  return hr;
}

HRESULT CreateDxcLinker(_In_ REFIID riid, _Out_ LPVOID *ppv) {
  *ppv = nullptr;
  CComPtr<DxcLinker> result;
  try {
    result = DxcLinker::Alloc(DxcGetThreadMallocNoRef());
    IFROOM(result.p);
    result->Initialize();
  }
  CATCH_CPP_RETURN_HRESULT();
  return result.p->QueryInterface(riid, ppv);
}