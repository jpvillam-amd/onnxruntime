// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"

#include "winml_adapter_c_api.h"
#include "winml_adapter_apis.h"
#include "core/session/ort_apis.h"

#include <core/providers/winml/winml_provider_factory.h>
#include <core/providers/cpu/cpu_provider_factory.h>
#include <core/providers/dml/dml_provider_factory.h>

const OrtApi* GetVersion1Api();

namespace winmla = Windows::AI::MachineLearning::Adapter;

static constexpr WinmlAdapterApi winml_adapter_api_1 = {
  // Schema override
  &winmla::OverrideSchema,

  // OrtEnv methods
  &winmla::EnvConfigureCustomLoggerAndProfiler,

  // OrtTypeInfo Casting methods
  &OrtApis::GetDenotationFromTypeInfo,
  &OrtApis::CastTypeInfoToMapTypeInfo,
  &OrtApis::CastTypeInfoToSequenceTypeInfo,

  // OrtMapTypeInfo Accessors
  &OrtApis::GetMapKeyType, 
  &OrtApis::GetMapValueType,

  // OrtSequenceTypeInfo Accessors
  &OrtApis::GetSequenceElementType, 

  // OrtModel methods
  &winmla::CreateModelFromPath,
  &winmla::CreateModelFromData,  
  &winmla::CloneModel,
  &winmla::ModelGetAuthor,
  &winmla::ModelGetName,
  &winmla::ModelGetDomain,
  &winmla::ModelGetDescription,
  &winmla::ModelGetVersion,
  &winmla::ModelGetInputCount,
  &winmla::ModelGetOutputCount,
  &winmla::ModelGetInputName,
  &winmla::ModelGetOutputName,
  &winmla::ModelGetInputDescription,
  &winmla::ModelGetOutputDescription,
  &winmla::ModelGetInputTypeInfo,
  &winmla::ModelGetOutputTypeInfo,
  &winmla::ModelGetMetadataCount,
  &winmla::ModelGetMetadata,
  &winmla::ModelEnsureNoFloat16,

  // OrtSessionOptions methods
  &OrtSessionOptionsAppendExecutionProvider_CPU,
  &winmla::OrtSessionOptionsAppendExecutionProviderEx_DML, 

  // OrtSession methods
  &winmla::CreateSessionWithoutModel,
  &winmla::SessionGetExecutionProvider,
  &winmla::SessionInitialize,
  &winmla::SessionRegisterGraphTransformers,
  &winmla::SessionRegisterCustomRegistry,
  &winmla::SessionLoadAndPurloinModel,
  &winmla::SessionStartProfiling,
  &winmla::SessionEndProfiling,
  &winmla::SessionCopyOneInputAcrossDevices,
      
  // Dml methods (TODO need to figure out how these need to move to session somehow...)
  &winmla::DmlExecutionProviderSetDefaultRoundingMode,
  &winmla::DmlExecutionProviderFlushContext,
  &winmla::DmlExecutionProviderTrimUploadHeap,
  &winmla::DmlExecutionProviderReleaseCompletedReferences,  
  &winmla::DmlCreateGPUAllocationFromD3DResource,
  &winmla::DmlFreeGPUAllocation,
  &winmla::DmlGetD3D12ResourceFromAllocation,
  &winmla::DmlCopyTensor,

  &winmla::GetProviderMemoryInfo,
  &winmla::GetProviderAllocator,
  &winmla::FreeProviderAllocator,
  &winmla::GetValueMemoryInfo,

  &winmla::ExecutionProviderSync,
  
  &winmla::CreateCustomRegistry,

  &winmla::ValueGetDeviceId,
  &winmla::SessionGetInputRequiredDeviceId,

  // Release
  &winmla::ReleaseModel,
  &OrtApis::ReleaseMapTypeInfo,
  &OrtApis::ReleaseSequenceTypeInfo
};

const WinmlAdapterApi* ORT_API_CALL OrtGetWinMLAdapter(const OrtApi* ort_api) NO_EXCEPTION {
  if (GetVersion1Api() == ort_api) {
    return &winml_adapter_api_1;
  }

  return nullptr;
}