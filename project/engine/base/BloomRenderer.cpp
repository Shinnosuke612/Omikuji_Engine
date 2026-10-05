// 役割: Bloom用RenderTargetと各ポストエフェクトパスを実装する。
#include "BloomRenderer.h"

#include <algorithm>
#include <cassert>

#include "DirectXCommon.h"
#include "RenderFormats.h"
#include "../3d/SrvManager.h"
#include "../utility/Logger.h"

bool BloomRenderer::Initialize(
	DirectXCommon* dxCommon,
	SrvManager* srvManager
) {
	assert(dxCommon);
	assert(srvManager);
	if (!dxCommon || !srvManager || initialized_) {
		return false;
	}

	dxCommon_ = dxCommon;
	srvManager_ = srvManager;

	CreateRootSignature();
	CreatePipelineStates();

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

	SceneRenderTarget::Desc desc{};
	desc.width = 1;
	desc.height = 1;
	desc.format = RenderFormats::kSceneHdrFormat;
	desc.createDepth = false;
	desc.clearColor[0] = 0.0f;
	desc.clearColor[1] = 0.0f;
	desc.clearColor[2] = 0.0f;
	desc.clearColor[3] = 1.0f;
	if (
		!brightTarget_.Initialize(dxCommon_, srvManager_, desc) ||
		!blurTargets_[0].Initialize(dxCommon_, srvManager_, desc) ||
		!blurTargets_[1].Initialize(dxCommon_, srvManager_, desc)
	) {
		return false;
	}
	initialized_ = true;
	return true;
}

void BloomRenderer::Resize(
	uint32_t width,
	uint32_t height,
	uint32_t downsampleScale
) {
	if (!initialized_) {
		return;
	}
	width_ = (std::max)(width, 1u);
	height_ = (std::max)(height, 1u);
	downsampleScale_ = std::clamp(downsampleScale, 1u, 8u);

	const uint32_t bloomWidth =
		(std::max)(width_ / downsampleScale_, 1u);
	const uint32_t bloomHeight =
		(std::max)(height_ / downsampleScale_, 1u);

	brightTarget_.Resize(bloomWidth, bloomHeight);
	blurTargets_[0].Resize(bloomWidth, bloomHeight);
	blurTargets_[1].Resize(bloomWidth, bloomHeight);
}

void BloomRenderer::BeginFrame() {
	drawIndex_ = 0;
}

void BloomRenderer::SetParameters(const Parameters& parameters) {
	pendingParameters_ = parameters;
	pendingParameters_.blurIterations =
		std::clamp(pendingParameters_.blurIterations, 0, 12);
	pendingParameters_.downsampleScale =
		std::clamp(pendingParameters_.downsampleScale, 1, 8);
}

void BloomRenderer::Apply(
	D3D12_GPU_DESCRIPTOR_HANDLE sceneTexture,
	SceneRenderTarget* outputTarget
) {
	if (!initialized_ || !outputTarget) {
		return;
	}

	Resize(
		outputTarget->GetWidth(),
		outputTarget->GetHeight(),
		static_cast<uint32_t>(pendingParameters_.downsampleScale)
	);

	brightTarget_.Begin();
	srvManager_->PreDraw();
	pendingParameters_.texelSize[0] =
		1.0f / static_cast<float>(brightTarget_.GetWidth());
	pendingParameters_.texelSize[1] =
		1.0f / static_cast<float>(brightTarget_.GetHeight());
	DrawFullscreen(sceneTexture, sceneTexture, Pass::kExtract);
	brightTarget_.End();

	D3D12_GPU_DESCRIPTOR_HANDLE bloomSource =
		brightTarget_.GetSrvGpuHandle();
	for (int32_t iteration = 0;
		iteration < pendingParameters_.blurIterations;
		++iteration) {
		for (uint32_t axis = 0; axis < 2; ++axis) {
			SceneRenderTarget& destination = blurTargets_[axis];
			destination.Begin();
			srvManager_->PreDraw();
			pendingParameters_.horizontal = axis == 0 ? 1 : 0;
			pendingParameters_.texelSize[0] =
				1.0f / static_cast<float>(destination.GetWidth());
			pendingParameters_.texelSize[1] =
				1.0f / static_cast<float>(destination.GetHeight());
			DrawFullscreen(
				bloomSource,
				bloomSource,
				Pass::kBlur
			);
			destination.End();
			bloomSource = destination.GetSrvGpuHandle();
		}
	}

	outputTarget->Begin();
	srvManager_->PreDraw();
	pendingParameters_.texelSize[0] =
		1.0f / static_cast<float>(outputTarget->GetWidth());
	pendingParameters_.texelSize[1] =
		1.0f / static_cast<float>(outputTarget->GetHeight());
	DrawFullscreen(sceneTexture, bloomSource, Pass::kComposite);
	outputTarget->End();
}

void BloomRenderer::CreateRootSignature() {
	D3D12_DESCRIPTOR_RANGE descriptorRanges[2]{};
	for (uint32_t index = 0; index < 2; ++index) {
		descriptorRanges[index].RangeType =
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		descriptorRanges[index].NumDescriptors = 1;
		descriptorRanges[index].BaseShaderRegister = index;
		descriptorRanges[index].OffsetInDescriptorsFromTableStart =
			D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	}

	D3D12_ROOT_PARAMETER rootParameters[3]{};
	for (uint32_t index = 0; index < 2; ++index) {
		rootParameters[index].ParameterType =
			D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParameters[index].ShaderVisibility =
			D3D12_SHADER_VISIBILITY_PIXEL;
		rootParameters[index].DescriptorTable.pDescriptorRanges =
			&descriptorRanges[index];
		rootParameters[index].DescriptorTable.NumDescriptorRanges = 1;
	}
	rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	rootParameters[2].Descriptor.ShaderRegister = 0;

	D3D12_STATIC_SAMPLER_DESC sampler{};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.ShaderRegister = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;

	D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
	rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
	rootSignatureDesc.pParameters = rootParameters;
	rootSignatureDesc.NumParameters = _countof(rootParameters);
	rootSignatureDesc.pStaticSamplers = &sampler;
	rootSignatureDesc.NumStaticSamplers = 1;

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

void BloomRenderer::CreatePipelineStates() {
	const auto vertexShader = dxCommon_->CompileShader(
		L"resources/shaders/Fullscreen.VS.hlsl",
		L"vs_6_0"
	);
	const auto extractPixelShader = dxCommon_->CompileShader(
		L"resources/shaders/BloomExtract.PS.hlsl",
		L"ps_6_0"
	);
	const auto blurPixelShader = dxCommon_->CompileShader(
		L"resources/shaders/BloomBlur.PS.hlsl",
		L"ps_6_0"
	);
	const auto compositePixelShader = dxCommon_->CompileShader(
		L"resources/shaders/BloomComposite.PS.hlsl",
		L"ps_6_0"
	);
	assert(vertexShader);
	assert(extractPixelShader);
	assert(blurPixelShader);
	assert(compositePixelShader);

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
	pipelineDesc.SampleDesc.Count = 1;

	pipelineDesc.RTVFormats[0] = RenderFormats::kSceneHdrFormat;
	pipelineDesc.PS = {
		extractPixelShader->GetBufferPointer(),
		extractPixelShader->GetBufferSize()
	};
	HRESULT result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
		&pipelineDesc,
		IID_PPV_ARGS(&extractPipelineState_)
	);
	assert(SUCCEEDED(result));

	pipelineDesc.PS = {
		blurPixelShader->GetBufferPointer(),
		blurPixelShader->GetBufferSize()
	};
	result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
		&pipelineDesc,
		IID_PPV_ARGS(&blurPipelineState_)
	);
	assert(SUCCEEDED(result));

	pipelineDesc.RTVFormats[0] = RenderFormats::kDisplayFormat;
	pipelineDesc.PS = {
		compositePixelShader->GetBufferPointer(),
		compositePixelShader->GetBufferSize()
	};
	result = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
		&pipelineDesc,
		IID_PPV_ARGS(&compositePipelineState_)
	);
	assert(SUCCEEDED(result));
}

void BloomRenderer::DrawFullscreen(
	D3D12_GPU_DESCRIPTOR_HANDLE textureHandle,
	D3D12_GPU_DESCRIPTOR_HANDLE bloomHandle,
	Pass pass
) {
	assert(drawIndex_ < kMaxDrawsPerFrame);
	const uint32_t parameterIndex = drawIndex_++;
	*parameterData_[parameterIndex] = pendingParameters_;

	ID3D12PipelineState* pipelineState = extractPipelineState_.Get();
	if (pass == Pass::kBlur) {
		pipelineState = blurPipelineState_.Get();
	} else if (pass == Pass::kComposite) {
		pipelineState = compositePipelineState_.Get();
	}

	auto* commandList = dxCommon_->GetCommandList();
	commandList->SetGraphicsRootSignature(rootSignature_.Get());
	commandList->SetPipelineState(pipelineState);
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	commandList->SetGraphicsRootDescriptorTable(0, textureHandle);
	commandList->SetGraphicsRootDescriptorTable(1, bloomHandle);
	commandList->SetGraphicsRootConstantBufferView(
		2,
		parameterResources_[parameterIndex]->GetGPUVirtualAddress()
	);
	commandList->DrawInstanced(3, 1, 0, 0);
}
