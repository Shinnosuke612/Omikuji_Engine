// 役割: フルスクリーン三角形を使った画像コピー描画を実装する。
#include "FullscreenCopy.h"

#include <cassert>

#include "DirectXCommon.h"
#include "RenderFormats.h"
#include "../utility/Logger.h"

void FullscreenCopy::Initialize(DirectXCommon* dxCommon) {
	assert(dxCommon);
	dxCommon_ = dxCommon;
	CreateRootSignature();
	CreatePipelineState();

	for (uint32_t index = 0; index < kMaxDrawsPerFrame; ++index) {
		parameterResources_[index] =
			dxCommon_->CreateBufferResource(sizeof(Parameters));
		parameterResources_[index]->Map(
			0,
			nullptr,
			reinterpret_cast<void**>(&parameterData_[index])
		);
		*parameterData_[index] = {};
	}
}

void FullscreenCopy::BeginFrame() {
	drawIndex_ = 0;
	pendingParameters_ = {};
}

void FullscreenCopy::SetParameters(
	const Parameters& parameters
) {
	pendingParameters_ = parameters;
}

void FullscreenCopy::Draw(
	D3D12_GPU_DESCRIPTOR_HANDLE textureHandle,
	D3D12_GPU_DESCRIPTOR_HANDLE depthTextureHandle,
	D3D12_GPU_DESCRIPTOR_HANDLE maskTextureHandle,
	Effect effect,
	OutputFormat outputFormat
) {
	Draw(
		textureHandle,
		depthTextureHandle,
		maskTextureHandle,
		textureHandle,
		effect,
		outputFormat
	);
}

void FullscreenCopy::Draw(
	D3D12_GPU_DESCRIPTOR_HANDLE textureHandle,
	D3D12_GPU_DESCRIPTOR_HANDLE depthTextureHandle,
	D3D12_GPU_DESCRIPTOR_HANDLE maskTextureHandle,
	D3D12_GPU_DESCRIPTOR_HANDLE historyTextureHandle,
	Effect effect,
	OutputFormat outputFormat
) {
	assert(dxCommon_);
	assert(drawIndex_ < kMaxDrawsPerFrame);

	const uint32_t parameterIndex = drawIndex_++;
	*parameterData_[parameterIndex] = pendingParameters_;

	ID3D12GraphicsCommandList* commandList = dxCommon_->GetCommandList();
	commandList->SetGraphicsRootSignature(rootSignature_.Get());
	ID3D12PipelineState* pipelineState = copyPipelineState_.Get();
	if (outputFormat == OutputFormat::kSceneHdr) {
		if (effect == Effect::kWaterRefraction) {
			pipelineState = waterRefractionSceneHdrPipelineState_.Get();
		} else if (effect == Effect::kWaterLightShafts) {
			pipelineState = waterLightShaftsSceneHdrPipelineState_.Get();
		} else {
			pipelineState = copySceneHdrPipelineState_.Get();
		}
	}
	else if (effect == Effect::kGrayscale) {
		pipelineState = grayscalePipelineState_.Get();
	}
	else if (effect == Effect::kVignette) {
		pipelineState = vignettePipelineState_.Get();
	}
	else if (effect == Effect::kBoxBlur) {
		pipelineState = boxBlurPipelineState_.Get();
	}
	else if (effect == Effect::kGaussianBlur) {
		pipelineState = gaussianBlurPipelineState_.Get();
	}
	else if (effect == Effect::kRadialBlur) {
		pipelineState = radialBlurPipelineState_.Get();
	}
	else if (effect == Effect::kNoise) {
		pipelineState = noisePipelineState_.Get();
	}
	else if (effect == Effect::kDissolve) {
		pipelineState = dissolvePipelineState_.Get();
	}
	else if (effect == Effect::kOutline) {
		pipelineState = outlinePipelineState_.Get();
	}
	else if (effect == Effect::kDepthOfField) {
		pipelineState = depthOfFieldPipelineState_.Get();
	}
	else if (effect == Effect::kUnderwater) {
		pipelineState = underwaterPipelineState_.Get();
	}
	else if (effect == Effect::kWaterRefraction) {
		pipelineState = waterRefractionPipelineState_.Get();
	}
	else if (effect == Effect::kWaterLightShafts) {
		pipelineState = waterLightShaftsPipelineState_.Get();
	}
	else if (effect == Effect::kPixelation) {
		pipelineState = pixelationPipelineState_.Get();
	}
	else if (effect == Effect::kChromaticAberration) {
		pipelineState = chromaticAberrationPipelineState_.Get();
	}
	else if (effect == Effect::kMotionBlur) {
		pipelineState = motionBlurPipelineState_.Get();
	}
	else if (effect == Effect::kIrisTransition) {
		pipelineState = irisTransitionPipelineState_.Get();
	}
	commandList->SetPipelineState(pipelineState);
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	commandList->SetGraphicsRootDescriptorTable(0, textureHandle);
	commandList->SetGraphicsRootDescriptorTable(1, depthTextureHandle);
	commandList->SetGraphicsRootDescriptorTable(2, maskTextureHandle);
	commandList->SetGraphicsRootDescriptorTable(3, historyTextureHandle);
	commandList->SetGraphicsRootConstantBufferView(
		4,
		parameterResources_[parameterIndex]->GetGPUVirtualAddress()
	);
	commandList->DrawInstanced(3, 1, 0, 0);
}

void FullscreenCopy::CreateRootSignature() {
	D3D12_DESCRIPTOR_RANGE descriptorRanges[4]{};
	for (uint32_t index = 0; index < 4; ++index) {
		descriptorRanges[index].RangeType =
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		descriptorRanges[index].NumDescriptors = 1;
		descriptorRanges[index].BaseShaderRegister = index;
		descriptorRanges[index].OffsetInDescriptorsFromTableStart =
			D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	}

	D3D12_ROOT_PARAMETER rootParameters[5]{};
	rootParameters[0].ParameterType =
		D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	rootParameters[0].DescriptorTable.pDescriptorRanges =
		&descriptorRanges[0];
	rootParameters[0].DescriptorTable.NumDescriptorRanges = 1;

	rootParameters[1].ParameterType =
		D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	rootParameters[1].DescriptorTable.pDescriptorRanges =
		&descriptorRanges[1];
	rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;

	rootParameters[2].ParameterType =
		D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	rootParameters[2].DescriptorTable.pDescriptorRanges =
		&descriptorRanges[2];
	rootParameters[2].DescriptorTable.NumDescriptorRanges = 1;

	rootParameters[3].ParameterType =
		D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	rootParameters[3].DescriptorTable.pDescriptorRanges =
		&descriptorRanges[3];
	rootParameters[3].DescriptorTable.NumDescriptorRanges = 1;

	rootParameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	rootParameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	rootParameters[4].Descriptor.ShaderRegister = 0;

	D3D12_STATIC_SAMPLER_DESC samplers[2]{};
	for (uint32_t index = 0; index < 2; ++index) {
		samplers[index].Filter = index == 0
			? D3D12_FILTER_MIN_MAG_MIP_LINEAR
			: D3D12_FILTER_MIN_MAG_MIP_POINT;
		samplers[index].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[index].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[index].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[index].ShaderRegister = index;
		samplers[index].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		samplers[index].MaxLOD = D3D12_FLOAT32_MAX;
	}

	D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
	rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
	rootSignatureDesc.pParameters = rootParameters;
	rootSignatureDesc.NumParameters = _countof(rootParameters);
	rootSignatureDesc.pStaticSamplers = samplers;
	rootSignatureDesc.NumStaticSamplers = _countof(samplers);

	Microsoft::WRL::ComPtr<ID3DBlob> signatureBlob;
	Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
	HRESULT result = D3D12SerializeRootSignature(
		&rootSignatureDesc,
		D3D_ROOT_SIGNATURE_VERSION_1,
		&signatureBlob,
		&errorBlob
	);
	if (FAILED(result)) {
		if (errorBlob) {
			Logger::Log(
				static_cast<const char*>(errorBlob->GetBufferPointer())
			);
		}
		assert(false);
	}

	result = dxCommon_->GetDevice()->CreateRootSignature(
		0,
		signatureBlob->GetBufferPointer(),
		signatureBlob->GetBufferSize(),
		IID_PPV_ARGS(&rootSignature_)
	);
	assert(SUCCEEDED(result));
}

void FullscreenCopy::CreatePipelineState() {
	const auto vertexShader = dxCommon_->CompileShader(
		L"resources/shaders/Fullscreen.VS.hlsl",
		L"vs_6_0"
	);
	const auto copyPixelShader = dxCommon_->CompileShader(
		L"resources/shaders/CopyImage.PS.hlsl",
		L"ps_6_0"
	);
	const auto grayscalePixelShader = dxCommon_->CompileShader(
		L"resources/shaders/Grayscale.PS.hlsl",
		L"ps_6_0"
	);
	const auto vignettePixelShader = dxCommon_->CompileShader(
		L"resources/shaders/Vignette.PS.hlsl",
		L"ps_6_0"
	);
	const auto boxBlurPixelShader = dxCommon_->CompileShader(
		L"resources/shaders/BoxBlur.PS.hlsl",
		L"ps_6_0"
	);
	const auto gaussianBlurPixelShader = dxCommon_->CompileShader(
		L"resources/shaders/GaussianBlur.PS.hlsl",
		L"ps_6_0"
	);
	const auto radialBlurPixelShader = dxCommon_->CompileShader(
		L"resources/shaders/RadialBlur.PS.hlsl",
		L"ps_6_0"
	);
	const auto noisePixelShader = dxCommon_->CompileShader(
		L"resources/shaders/Noise.PS.hlsl",
		L"ps_6_0"
	);
	const auto dissolvePixelShader = dxCommon_->CompileShader(
		L"resources/shaders/Dissolve.PS.hlsl",
		L"ps_6_0"
	);
	const auto outlinePixelShader = dxCommon_->CompileShader(
		L"resources/shaders/Outline.PS.hlsl",
		L"ps_6_0"
	);
	const auto depthOfFieldPixelShader = dxCommon_->CompileShader(
		L"resources/shaders/DepthOfField.PS.hlsl",
		L"ps_6_0"
	);
	const auto underwaterPixelShader = dxCommon_->CompileShader(
		L"resources/shaders/Underwater.PS.hlsl",
		L"ps_6_0"
	);
	const auto waterRefractionPixelShader = dxCommon_->CompileShader(
		L"resources/shaders/WaterRefraction.PS.hlsl",
		L"ps_6_0"
	);
	const auto waterLightShaftsPixelShader = dxCommon_->CompileShader(
		L"resources/shaders/WaterLightShafts.PS.hlsl",
		L"ps_6_0"
	);
	const auto chromaticAberrationPixelShader = dxCommon_->CompileShader(L"resources/shaders/ChromaticAberration.PS.hlsl", L"ps_6_0");
	const auto pixelationPixelShader = dxCommon_->CompileShader(L"resources/shaders/Pixelation.PS.hlsl", L"ps_6_0");
	const auto motionBlurPixelShader = dxCommon_->CompileShader(L"resources/shaders/MotionBlur.PS.hlsl", L"ps_6_0");
	const auto irisTransitionPixelShader = dxCommon_->CompileShader(
		L"resources/shaders/IrisTransition.PS.hlsl",
		L"ps_6_0"
	);
	assert(vertexShader);
	assert(copyPixelShader);
	assert(grayscalePixelShader);
	assert(vignettePixelShader);
	assert(boxBlurPixelShader);
	assert(gaussianBlurPixelShader);
	assert(radialBlurPixelShader);
	assert(noisePixelShader);
	assert(dissolvePixelShader);
	assert(outlinePixelShader);
	assert(depthOfFieldPixelShader);
	assert(underwaterPixelShader);
	assert(waterRefractionPixelShader);
	assert(waterLightShaftsPixelShader);
	assert(chromaticAberrationPixelShader);
	assert(pixelationPixelShader);
	assert(motionBlurPixelShader);
	assert(irisTransitionPixelShader);

	D3D12_RASTERIZER_DESC rasterizerDesc{};
	rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
	rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
	rasterizerDesc.DepthClipEnable = TRUE;

	D3D12_BLEND_DESC blendDesc{};
	blendDesc.RenderTarget[0].RenderTargetWriteMask =
		D3D12_COLOR_WRITE_ENABLE_ALL;

	D3D12_DEPTH_STENCIL_DESC depthStencilDesc{};
	depthStencilDesc.DepthEnable = FALSE;
	depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	depthStencilDesc.StencilEnable = FALSE;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
	pipelineDesc.pRootSignature = rootSignature_.Get();
	pipelineDesc.InputLayout = { nullptr, 0 };
	pipelineDesc.VS = {
		vertexShader->GetBufferPointer(),
		vertexShader->GetBufferSize()
	};
	pipelineDesc.BlendState = blendDesc;
	pipelineDesc.RasterizerState = rasterizerDesc;
	pipelineDesc.DepthStencilState = depthStencilDesc;
	pipelineDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
	pipelineDesc.PrimitiveTopologyType =
		D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pipelineDesc.NumRenderTargets = 1;
	pipelineDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	pipelineDesc.SampleDesc.Count = 1;

	pipelineDesc.PS = {
		copyPixelShader->GetBufferPointer(),
		copyPixelShader->GetBufferSize()
	};
	HRESULT result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
		&pipelineDesc,
		IID_PPV_ARGS(&copyPipelineState_)
	);
	assert(SUCCEEDED(result));

	pipelineDesc.PS = {
		grayscalePixelShader->GetBufferPointer(),
		grayscalePixelShader->GetBufferSize()
	};
	result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
		&pipelineDesc,
		IID_PPV_ARGS(&grayscalePipelineState_)
	);
	assert(SUCCEEDED(result));

	pipelineDesc.PS = {
		vignettePixelShader->GetBufferPointer(),
		vignettePixelShader->GetBufferSize()
	};
	result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
		&pipelineDesc,
		IID_PPV_ARGS(&vignettePipelineState_)
	);
	assert(SUCCEEDED(result));

	pipelineDesc.PS = {
		boxBlurPixelShader->GetBufferPointer(),
		boxBlurPixelShader->GetBufferSize()
	};
	result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
		&pipelineDesc,
		IID_PPV_ARGS(&boxBlurPipelineState_)
	);
	assert(SUCCEEDED(result));

	pipelineDesc.PS = {
		gaussianBlurPixelShader->GetBufferPointer(),
		gaussianBlurPixelShader->GetBufferSize()
	};
	result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
		&pipelineDesc,
		IID_PPV_ARGS(&gaussianBlurPipelineState_)
	);
	assert(SUCCEEDED(result));

	pipelineDesc.PS = {
		radialBlurPixelShader->GetBufferPointer(),
		radialBlurPixelShader->GetBufferSize()
	};
	result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
		&pipelineDesc,
		IID_PPV_ARGS(&radialBlurPipelineState_)
	);
	assert(SUCCEEDED(result));

	pipelineDesc.PS = {
		noisePixelShader->GetBufferPointer(),
		noisePixelShader->GetBufferSize()
	};
	result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
		&pipelineDesc,
		IID_PPV_ARGS(&noisePipelineState_)
	);
	assert(SUCCEEDED(result));

	pipelineDesc.PS = {
		dissolvePixelShader->GetBufferPointer(),
		dissolvePixelShader->GetBufferSize()
	};
	result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
		&pipelineDesc,
		IID_PPV_ARGS(&dissolvePipelineState_)
	);
	assert(SUCCEEDED(result));

	pipelineDesc.PS = {
		outlinePixelShader->GetBufferPointer(),
		outlinePixelShader->GetBufferSize()
	};
	result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
		&pipelineDesc,
		IID_PPV_ARGS(&outlinePipelineState_)
	);
	assert(SUCCEEDED(result));

	pipelineDesc.PS = {
		depthOfFieldPixelShader->GetBufferPointer(),
		depthOfFieldPixelShader->GetBufferSize()
	};
	result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
		&pipelineDesc,
		IID_PPV_ARGS(&depthOfFieldPipelineState_)
	);
	assert(SUCCEEDED(result));

	pipelineDesc.PS = {
		underwaterPixelShader->GetBufferPointer(),
		underwaterPixelShader->GetBufferSize()
	};
	result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
		&pipelineDesc,
		IID_PPV_ARGS(&underwaterPipelineState_)
	);
	assert(SUCCEEDED(result));

	pipelineDesc.PS = {
		waterRefractionPixelShader->GetBufferPointer(),
		waterRefractionPixelShader->GetBufferSize()
	};
	result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
		&pipelineDesc,
		IID_PPV_ARGS(&waterRefractionPipelineState_)
	);
	assert(SUCCEEDED(result));

	pipelineDesc.PS = {
		waterLightShaftsPixelShader->GetBufferPointer(),
		waterLightShaftsPixelShader->GetBufferSize()
	};
	result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
		&pipelineDesc,
		IID_PPV_ARGS(&waterLightShaftsPipelineState_)
	);
	assert(SUCCEEDED(result));

	pipelineDesc.PS = { chromaticAberrationPixelShader->GetBufferPointer(), chromaticAberrationPixelShader->GetBufferSize() };
	result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(&chromaticAberrationPipelineState_));
	assert(SUCCEEDED(result));

	pipelineDesc.PS = { pixelationPixelShader->GetBufferPointer(), pixelationPixelShader->GetBufferSize() };
	result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(&pixelationPipelineState_));
	assert(SUCCEEDED(result));

	pipelineDesc.PS = { motionBlurPixelShader->GetBufferPointer(), motionBlurPixelShader->GetBufferSize() };
	result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(&motionBlurPipelineState_));
	assert(SUCCEEDED(result));

	pipelineDesc.PS = {
		irisTransitionPixelShader->GetBufferPointer(),
		irisTransitionPixelShader->GetBufferSize()
	};
	result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
		&pipelineDesc,
		IID_PPV_ARGS(&irisTransitionPipelineState_)
	);
	assert(SUCCEEDED(result));

	pipelineDesc.RTVFormats[0] = RenderFormats::kSceneHdrFormat;
	pipelineDesc.PS = {
		copyPixelShader->GetBufferPointer(),
		copyPixelShader->GetBufferSize()
	};
	result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
		&pipelineDesc,
		IID_PPV_ARGS(&copySceneHdrPipelineState_)
	);
	assert(SUCCEEDED(result));

	pipelineDesc.PS = {
		waterRefractionPixelShader->GetBufferPointer(),
		waterRefractionPixelShader->GetBufferSize()
	};
	result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
		&pipelineDesc,
		IID_PPV_ARGS(&waterRefractionSceneHdrPipelineState_)
	);
	assert(SUCCEEDED(result));

	pipelineDesc.PS = {
		waterLightShaftsPixelShader->GetBufferPointer(),
		waterLightShaftsPixelShader->GetBufferSize()
	};
	result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
		&pipelineDesc,
		IID_PPV_ARGS(&waterLightShaftsSceneHdrPipelineState_)
	);
	assert(SUCCEEDED(result));
}
