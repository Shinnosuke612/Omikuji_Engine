// 役割: 水面メッシュと水中ポストエフェクトの描画を実装する。
#include "WaterSurfaceRenderer.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>

#include <d3dcompiler.h>

#include "../3d/Camera.h"
#include "../base/DirectXCommon.h"
#include "../base/RenderFormats.h"
#include "../math/Math.h"
#include "../utility/Logger.h"

void WaterSurfaceRenderer::Initialize(
	DirectXCommon* dxCommon,
	uint32_t gridResolution
) {
	dxCommon_ = dxCommon;
	assert(dxCommon_);

	CreateRootSignature();
	CreateGraphicsPipeline();
	CreateResources((std::clamp)(gridResolution, 4u, 160u));
}

bool WaterSurfaceRenderer::TryInitializeReplay(DirectXCommon* dx, uint32_t resolution) {
    if (dxCommon_ || !dx || !dx->GetDevice()) return false;
    dxCommon_ = dx;
    if (CreateRootSignature(true) && CreateGraphicsPipeline(true) &&
        CreateResources((std::clamp)(resolution, 4u, 160u), true)) return true;
    Finalize();
    return false;
}

void WaterSurfaceRenderer::Finalize() {
	surfaceData_ = nullptr;
	surfaceResource_.Reset();
	vertexResource_.Reset();
	pipelineState_.Reset();
	rootSignature_.Reset();
	vertexBufferView_ = {};
	vertexCount_ = 0;
	time_ = 0.0f;
	dxCommon_ = nullptr;
}

void WaterSurfaceRenderer::Update(float deltaTime) {
	time_ += deltaTime;
}

void WaterSurfaceRenderer::Draw(
	const Camera* camera,
	const Vector3& center,
	const Vector3& halfSize,
	const Settings& settings
) {
    DrawAtTime(camera, center, halfSize, settings, time_);
}

void WaterSurfaceRenderer::DrawAtTime(const Camera* camera, const Vector3& center,
    const Vector3& halfSize, const Settings& settings, float time) {
	if (
		!settings.enabled ||
		!dxCommon_ ||
		!camera ||
		!surfaceData_ ||
		vertexCount_ == 0
	) {
		return;
	}

	const float safeHalfX = (std::max)(halfSize.x, 0.05f);
	const float safeHalfY = (std::max)(halfSize.y, 0.05f);
	const float safeHalfZ = (std::max)(halfSize.z, 0.05f);
	const float span = (std::max)(safeHalfX, safeHalfZ);
	const float waveFitScale = (std::clamp)(span / 8.0f, 0.45f, 1.6f);
	const float amplitudeScale = settings.waveScale * waveFitScale;
	const float wavelengthScale = (std::max)(waveFitScale, 0.65f);
	const Vector3 cameraPosition = camera->GetTranslate();

	surfaceData_->viewProjection = camera->GetViewProjectionMatrix();
	surfaceData_->centerTime = { center.x, center.y, center.z, time };
	surfaceData_->halfSizeAlpha = {
		safeHalfX,
		safeHalfY,
		safeHalfZ,
		(std::clamp)(settings.alpha, 0.0f, 1.0f)
	};
	surfaceData_->cameraPositionFresnel = {
		cameraPosition.x,
		cameraPosition.y,
		cameraPosition.z,
		(std::max)(settings.fresnelPower, 0.1f)
	};
	surfaceData_->waveA = {
		0.86f,
		0.32f,
		0.12f * amplitudeScale,
		5.8f * wavelengthScale
	};
	surfaceData_->waveB = {
		-0.28f,
		0.96f,
		0.075f * amplitudeScale,
		3.1f * wavelengthScale
	};
	surfaceData_->waveC = {
		0.58f,
		-0.74f,
		0.045f * amplitudeScale,
		1.65f * wavelengthScale
	};
	surfaceData_->baseColor = settings.baseColor;
	surfaceData_->highlightColorNormal = {
		settings.highlightColor.x,
		settings.highlightColor.y,
		settings.highlightColor.z,
		(std::clamp)(settings.normalStrength, 0.0f, 2.0f)
	};

	ID3D12GraphicsCommandList* commandList = dxCommon_->GetCommandList();
	commandList->SetGraphicsRootSignature(rootSignature_.Get());
	commandList->SetPipelineState(pipelineState_.Get());
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	commandList->IASetVertexBuffers(0, 1, &vertexBufferView_);
	commandList->SetGraphicsRootConstantBufferView(
		0,
		surfaceResource_->GetGPUVirtualAddress()
	);
	commandList->DrawInstanced(vertexCount_, 1, 0, 0);
}

bool WaterSurfaceRenderer::CreateRootSignature(bool recoverable) {
	D3D12_ROOT_PARAMETER rootParameter{};
	rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	rootParameter.Descriptor.ShaderRegister = 0;

	D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
	rootSignatureDesc.Flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	rootSignatureDesc.pParameters = &rootParameter;
	rootSignatureDesc.NumParameters = 1;

	Microsoft::WRL::ComPtr<ID3DBlob> signatureBlob;
	Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
	const HRESULT serializeResult = D3D12SerializeRootSignature(
		&rootSignatureDesc,
		D3D_ROOT_SIGNATURE_VERSION_1,
		&signatureBlob,
		&errorBlob
	);
	if (FAILED(serializeResult)) {
		if (errorBlob) {
			Logger::Log(static_cast<const char*>(errorBlob->GetBufferPointer()));
		}
		if (!recoverable) assert(false);
		return false;
	}

	const HRESULT createResult = dxCommon_->GetDevice()->CreateRootSignature(
		0,
		signatureBlob->GetBufferPointer(),
		signatureBlob->GetBufferSize(),
		IID_PPV_ARGS(&rootSignature_)
	);
	if (!recoverable) assert(SUCCEEDED(createResult));
	return SUCCEEDED(createResult);
}

bool WaterSurfaceRenderer::CreateGraphicsPipeline(bool recoverable) {
	D3D12_INPUT_ELEMENT_DESC inputElements[2]{};
	inputElements[0].SemanticName = "POSITION";
	inputElements[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
	inputElements[0].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
	inputElements[1].SemanticName = "TEXCOORD";
	inputElements[1].Format = DXGI_FORMAT_R32G32_FLOAT;
	inputElements[1].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;

	const D3D12_INPUT_LAYOUT_DESC inputLayout{
		inputElements,
		_countof(inputElements)
	};

    Microsoft::WRL::ComPtr<IDxcBlob> vertexShader, pixelShader;
    if (recoverable) {
        std::string diagnostic;
        if (!dxCommon_->TryCompileShader(L"resources/shaders/WaterSurface.VS.hlsl", L"vs_6_0", vertexShader, diagnostic) ||
            !dxCommon_->TryCompileShader(L"resources/shaders/WaterSurface.PS.hlsl", L"ps_6_0", pixelShader, diagnostic)) return false;
    } else {
        vertexShader = dxCommon_->CompileShader(L"resources/shaders/WaterSurface.VS.hlsl", L"vs_6_0");
        pixelShader = dxCommon_->CompileShader(L"resources/shaders/WaterSurface.PS.hlsl", L"ps_6_0");
        assert(vertexShader && pixelShader);
        if (!vertexShader || !pixelShader) return false;
    }

	D3D12_BLEND_DESC blendDesc{};
	blendDesc.RenderTarget[0].RenderTargetWriteMask =
		D3D12_COLOR_WRITE_ENABLE_ALL;
	blendDesc.RenderTarget[0].BlendEnable = TRUE;
	blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;

	D3D12_RASTERIZER_DESC rasterizerDesc{};
	rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
	rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
	rasterizerDesc.DepthClipEnable = TRUE;

	D3D12_DEPTH_STENCIL_DESC depthStencilDesc{};
	depthStencilDesc.DepthEnable = TRUE;
	depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
	pipelineDesc.pRootSignature = rootSignature_.Get();
	pipelineDesc.InputLayout = inputLayout;
	pipelineDesc.VS = {
		vertexShader->GetBufferPointer(),
		vertexShader->GetBufferSize()
	};
	pipelineDesc.PS = {
		pixelShader->GetBufferPointer(),
		pixelShader->GetBufferSize()
	};
	pipelineDesc.BlendState = blendDesc;
	pipelineDesc.RasterizerState = rasterizerDesc;
	pipelineDesc.DepthStencilState = depthStencilDesc;
	pipelineDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
	pipelineDesc.PrimitiveTopologyType =
		D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pipelineDesc.NumRenderTargets = 1;
	pipelineDesc.RTVFormats[0] = RenderFormats::kSceneHdrFormat;
	pipelineDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	pipelineDesc.SampleDesc.Count = 1;

	const HRESULT result =
		dxCommon_->GetDevice()->CreateGraphicsPipelineState(
			&pipelineDesc,
			IID_PPV_ARGS(&pipelineState_)
		);
	if (!recoverable) assert(SUCCEEDED(result));
	return SUCCEEDED(result);
}

bool WaterSurfaceRenderer::CreateResources(uint32_t gridResolution, bool recoverable) {
	std::vector<Vertex> vertices;
	vertices.reserve(
		static_cast<size_t>(gridResolution) *
		static_cast<size_t>(gridResolution) *
		6u
	);

	auto makeVertex = [](float u, float v) {
		return Vertex{
			{ u * 2.0f - 1.0f, 0.0f, v * 2.0f - 1.0f },
			{ u, v }
		};
	};

	for (uint32_t z = 0; z < gridResolution; ++z) {
		const float v0 = static_cast<float>(z) /
			static_cast<float>(gridResolution);
		const float v1 = static_cast<float>(z + 1) /
			static_cast<float>(gridResolution);
		for (uint32_t x = 0; x < gridResolution; ++x) {
			const float u0 = static_cast<float>(x) /
				static_cast<float>(gridResolution);
			const float u1 = static_cast<float>(x + 1) /
				static_cast<float>(gridResolution);

			vertices.push_back(makeVertex(u0, v0));
			vertices.push_back(makeVertex(u1, v0));
			vertices.push_back(makeVertex(u0, v1));

			vertices.push_back(makeVertex(u0, v1));
			vertices.push_back(makeVertex(u1, v0));
			vertices.push_back(makeVertex(u1, v1));
		}
	}

	vertexCount_ = static_cast<uint32_t>(vertices.size());
    if (recoverable) {
        if (!dxCommon_->TryCreateBufferResource(sizeof(Vertex) * vertices.size(), vertexResource_)) return false;
    } else vertexResource_ = dxCommon_->CreateBufferResource(sizeof(Vertex) * vertices.size());
    Vertex* mappedVertices = nullptr;
    if (!vertexResource_ || FAILED(vertexResource_->Map(0, nullptr,
        reinterpret_cast<void**>(&mappedVertices))) || !mappedVertices) return false;

	std::memcpy(
		mappedVertices,
		vertices.data(),
		sizeof(Vertex) * vertices.size()
	);
	vertexResource_->Unmap(0, nullptr);

	vertexBufferView_.BufferLocation =
		vertexResource_->GetGPUVirtualAddress();
	vertexBufferView_.SizeInBytes =
		static_cast<UINT>(sizeof(Vertex) * vertices.size());
	vertexBufferView_.StrideInBytes = sizeof(Vertex);

    if (recoverable) {
        if (!dxCommon_->TryCreateBufferResource(sizeof(SurfaceData), surfaceResource_)) return false;
    } else surfaceResource_ = dxCommon_->CreateBufferResource(sizeof(SurfaceData));
    if (!surfaceResource_ || FAILED(surfaceResource_->Map(0, nullptr,
        reinterpret_cast<void**>(&surfaceData_))) || !surfaceData_) return false;
    surfaceData_->viewProjection = MakeIdentity4x4();
    return true;
}
