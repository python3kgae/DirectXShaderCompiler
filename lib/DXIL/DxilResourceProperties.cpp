///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// DxilResourceProperites.cpp                                                //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
// This file is distributed under the University of Illinois Open Source     //
// License. See LICENSE.TXT for details.                                     //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include "dxc/DXIL/DxilResourceProperties.h"
#include "llvm/IR/Constant.h"
#include "dxc/DXIL/DxilShaderModel.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Constants.h"
#include "dxc/DXIL/DxilResourceBase.h"
#include "dxc/DXIL/DxilResource.h"
#include "dxc/DXIL/DxilCBuffer.h"
#include "dxc/DXIL/DxilSampler.h"
#include "dxc/DXIL/DxilOperations.h"
#include "dxc/DXIL/DxilInstructions.h"

using namespace llvm;

namespace hlsl {

bool DxilResourceProperties::operator==(const DxilResourceProperties &RP) {
  return Class == RP.Class && Kind == RP.Kind && RawDword0 == RP.RawDword0 &&
         RawDword1 == RP.RawDword1;
}

bool DxilResourceProperties::operator!=(const DxilResourceProperties &RP) {
  return !(*this == RP) ;
}

namespace resource_helper {
// Resource Class and Resource Kind is used as seperate parameter, other fileds
// are saved in constant.
// The constant is as struct with int32 fields.
// ShaderModel 6.6 has 2 fileds.
Constant *getAsConstant(const DxilResourceProperties &RP, Type *Ty,
                        const ShaderModel &) {
  StructType *ST = cast<StructType>(Ty);
  switch (ST->getNumElements()) {
  case 2: {
    Constant *RawDwords[] = {
        ConstantInt::get(ST->getElementType(0), RP.RawDword0),
        ConstantInt::get(ST->getElementType(1), RP.RawDword1)};
    return ConstantStruct::get(ST, RawDwords);
  } break;
  default:
    return nullptr;
    break;
  }
  return nullptr;
}

DxilResourceProperties loadFromConstant(const Constant &C,
                                        DXIL::ResourceClass RC,
                                        DXIL::ResourceKind RK, Type *Ty,
                                        const ShaderModel &) {
  DxilResourceProperties RP;
  RP.Class = RC;
  RP.Kind = RK;
  // Ty Should match C.getType().
  StructType *ST = cast<StructType>(Ty);
  switch (ST->getNumElements()) {
  case 2: {
    const ConstantStruct *CS = cast<ConstantStruct>(&C);
    const Constant *RawDword0 = CS->getOperand(0);
    const Constant *RawDword1 = CS->getOperand(1);
    RP.RawDword0 = cast<ConstantInt>(RawDword0)->getLimitedValue();
    RP.RawDword1 = cast<ConstantInt>(RawDword1)->getLimitedValue();
  } break;
  default:
    RP.Class = DXIL::ResourceClass::Invalid;
    break;
  }
  return RP;
}

DxilResourceProperties
loadFromAnnotateHandle(DxilInst_AnnotateHandle &annotateHandle, llvm::Type *Ty,
                       const ShaderModel &SM) {

  ConstantStruct *ResProp =
      cast<ConstantStruct>(annotateHandle.get_HandleAnnotation());
  return loadFromConstant(
      *ResProp, (DXIL::ResourceClass)annotateHandle.get_resourceClass_val(),
      (DXIL::ResourceKind)annotateHandle.get_resourceKind_val(), Ty, SM);
}

DxilResourceProperties loadFromResourceBase(DxilResourceBase *Res) {

  DxilResourceProperties RP;
  RP.Class = DXIL::ResourceClass::Invalid;
  if (!Res) {
    return RP;
  }

  RP.RawDword0 = 0;
  RP.RawDword1 = 0;

  auto SetResProperties = [&RP](DxilResource &Res) {
    switch (Res.GetKind()) {
    default:
      break;
    case DXIL::ResourceKind::FeedbackTexture2D:
    case DXIL::ResourceKind::FeedbackTexture2DArray:
      RP.SamplerFeedbackType = Res.GetSamplerFeedbackType();
      break;
    case DXIL::ResourceKind::RTAccelerationStructure:

      break;
    case DXIL::ResourceKind::StructuredBuffer:
    case DXIL::ResourceKind::StructuredBufferWithCounter:
      RP.ElementStride = Res.GetElementStride();
      break;
    case DXIL::ResourceKind::Texture2DMS:
    case DXIL::ResourceKind::Texture2DMSArray:
      switch (Res.GetSampleCount()) {
      default:
        RP.Typed.SampleCountPow2 =
            DxilResourceProperties::kSampleCountUndefined;
        break;
      case 1:
        RP.Typed.SampleCountPow2 = 0;
        break;
      case 2:
        RP.Typed.SampleCountPow2 = 1;
        break;
      case 4:
        RP.Typed.SampleCountPow2 = 2;
        break;
      case 8:
        RP.Typed.SampleCountPow2 = 3;
        break;
      case 16:
        RP.Typed.SampleCountPow2 = 4;
        break;
      case 32:
        RP.Typed.SampleCountPow2 = 5;
        break;
      }
      break;
    case DXIL::ResourceKind::TypedBuffer:
    case DXIL::ResourceKind::Texture1D:
    case DXIL::ResourceKind::Texture2D:
    case DXIL::ResourceKind::TextureCube:
    case DXIL::ResourceKind::Texture1DArray:
    case DXIL::ResourceKind::Texture2DArray:
    case DXIL::ResourceKind::TextureCubeArray:
    case DXIL::ResourceKind::Texture3D:
      Type *Ty = Res.GetRetType();
      RP.Typed.SingleComponent = IsResourceSingleComponent(Ty);
      RP.Typed.CompType = Res.GetCompType().GetKind();
      break;
    }
  };

  switch (Res->GetClass()) { case DXIL::ResourceClass::Invalid: return RP;
  case DXIL::ResourceClass::SRV: {
    DxilResource *SRV = (DxilResource*)(Res);
    RP.Kind = Res->GetKind();
    RP.Class = Res->GetClass();
    SetResProperties(*SRV);
  } break;
  case DXIL::ResourceClass::UAV: {
    DxilResource *UAV = (DxilResource *)(Res);
    RP.Kind = Res->GetKind();
    RP.Class = Res->GetClass();
    RP.UAV.bGloballyCoherent = UAV->IsGloballyCoherent();
    if (UAV->HasCounter()) {
      RP.Kind = DXIL::ResourceKind::StructuredBufferWithCounter;
    }
    RP.UAV.bROV = UAV->IsROV();
    SetResProperties(*UAV);
  } break;
  case DXIL::ResourceClass::Sampler: {
    RP.Class = DXIL::ResourceClass::Sampler;
    RP.Kind = Res->GetKind();
    DxilSampler *Sampler = (DxilSampler*)Res;
    if (Sampler->GetSamplerKind() == DXIL::SamplerKind::Comparison)
      RP.Kind = DXIL::ResourceKind::SamplerComparison;
    else if (Sampler->GetSamplerKind() == DXIL::SamplerKind::Invalid)
      RP.Kind = DXIL::ResourceKind::Invalid;
  } break;
  case DXIL::ResourceClass::CBuffer: {
    RP.Class = DXIL::ResourceClass::CBuffer;
    RP.Kind = Res->GetKind();
    DxilCBuffer *CB = (DxilCBuffer *)Res;
    RP.SizeInBytes = CB->GetSize();
  } break;
  }
  return RP;
}

bool IsResourceSingleComponent(llvm::Type *Ty) {
  if (llvm::ArrayType *arrType = llvm::dyn_cast<llvm::ArrayType>(Ty)) {
    if (arrType->getArrayNumElements() > 1) {
      return false;
    }
    return IsResourceSingleComponent(arrType->getArrayElementType());
  } else if (llvm::StructType *structType =
                 llvm::dyn_cast<llvm::StructType>(Ty)) {
    if (structType->getStructNumElements() > 1) {
      return false;
    }
    return IsResourceSingleComponent(structType->getStructElementType(0));
  } else if (llvm::VectorType *vectorType =
                 llvm::dyn_cast<llvm::VectorType>(Ty)) {
    if (vectorType->getNumElements() > 1) {
      return false;
    }
    return IsResourceSingleComponent(vectorType->getVectorElementType());
  }
  return true;
}

bool IsAnyTexture(DXIL::ResourceKind ResourceKind) {
  return DXIL::ResourceKind::Texture1D <= ResourceKind &&
         ResourceKind <= DXIL::ResourceKind::TextureCubeArray;
}

bool IsStructuredBuffer(DXIL::ResourceKind ResourceKind) {
  return ResourceKind == DXIL::ResourceKind::StructuredBuffer ||
         ResourceKind == DXIL::ResourceKind::StructuredBufferWithCounter;
}

bool IsTypedBuffer(DXIL::ResourceKind ResourceKind) {
  return ResourceKind == DXIL::ResourceKind::TypedBuffer;
}

bool IsTyped(DXIL::ResourceKind ResourceKind) {
  return IsTypedBuffer(ResourceKind) || IsAnyTexture(ResourceKind);
}

bool IsRawBuffer(DXIL::ResourceKind ResourceKind) {
  return ResourceKind == DXIL::ResourceKind::RawBuffer;
}

bool IsTBuffer(DXIL::ResourceKind ResourceKind) {
  return ResourceKind == DXIL::ResourceKind::TBuffer;
}

bool IsFeedbackTexture(DXIL::ResourceKind ResourceKind) {
  return ResourceKind == DXIL::ResourceKind::FeedbackTexture2D ||
         ResourceKind == DXIL::ResourceKind::FeedbackTexture2DArray;
}

} // namespace resource_helper
} // namespace hlsl
