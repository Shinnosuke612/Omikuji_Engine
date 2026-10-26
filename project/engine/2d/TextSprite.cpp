// 役割: Text用のDepthなし2D Pipelineとquad描画を実装する。
#include "TextSprite.h"
#include <cstring>

#include <algorithm>
#include <cassert>

#include "TextureManager.h"
#include "../base/DirectXCommon.h"
#include "../base/RenderFormats.h"
#include "../math/Math.h"

namespace {
	class TextSpritePipeline {
	public:
		void Initialize(DirectXCommon* dxCommon) {
			if (initialized_) {
				return;
			}
			dxCommon_ = dxCommon;
			CreateRootSignature();
			CreatePipeline(RenderFormats::kDisplayFormat, displayPipelineState_);
			CreatePipeline(RenderFormats::kSceneHdrFormat, sceneHdrPipelineState_);
			initialized_ = true;
		}

		void SetCommonRenderState(TextSprite::OutputTarget target) const {
			dxCommon_->GetCommandList()->SetGraphicsRootSignature(rootSignature_.Get());
			dxCommon_->GetCommandList()->SetPipelineState(
				target == TextSprite::OutputTarget::SceneHdr
					? sceneHdrPipelineState_.Get()
					: displayPipelineState_.Get()
			);
			dxCommon_->GetCommandList()->IASetPrimitiveTopology(
				D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
			);
		}

	private:
		void CreateRootSignature() {
			D3D12_DESCRIPTOR_RANGE descriptorRange{};
			descriptorRange.BaseShaderRegister = 0;
			descriptorRange.NumDescriptors = 1;
			descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			descriptorRange.OffsetInDescriptorsFromTableStart =
				D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
			D3D12_ROOT_PARAMETER parameters[3]{};
			parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			parameters[0].Descriptor.ShaderRegister = 0;
			parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
			parameters[1].Descriptor.ShaderRegister = 0;
			parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			parameters[2].DescriptorTable.pDescriptorRanges = &descriptorRange;
			parameters[2].DescriptorTable.NumDescriptorRanges = 1;
			D3D12_STATIC_SAMPLER_DESC sampler{};
			sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.ShaderRegister = 0;
			sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			D3D12_ROOT_SIGNATURE_DESC description{};
			description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
			description.pParameters = parameters;
			description.NumParameters = _countof(parameters);
			description.pStaticSamplers = &sampler;
			description.NumStaticSamplers = 1;
			Microsoft::WRL::ComPtr<ID3DBlob> signature;
			Microsoft::WRL::ComPtr<ID3DBlob> error;
			const HRESULT result = D3D12SerializeRootSignature(
				&description,
				D3D_ROOT_SIGNATURE_VERSION_1,
				&signature,
				&error
			);
			assert(SUCCEEDED(result));
			const HRESULT rootSignatureResult = dxCommon_->GetDevice()->CreateRootSignature(
				0,
				signature->GetBufferPointer(),
				signature->GetBufferSize(),
				IID_PPV_ARGS(&rootSignature_)
			);
			assert(SUCCEEDED(rootSignatureResult));
		}

		void CreatePipeline(
			DXGI_FORMAT format,
			Microsoft::WRL::ComPtr<ID3D12PipelineState>& pipelineState
		) {
			D3D12_INPUT_ELEMENT_DESC elements[3]{};
			elements[0] = { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
				D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
			elements[1] = { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
				D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
			elements[2] = { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
				D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
			D3D12_INPUT_LAYOUT_DESC inputLayout{ elements, _countof(elements) };
			const auto vertexShader = dxCommon_->CompileShader(
				L"resources/shaders/Sprite.VS.hlsl", L"vs_6_0"
			);
			const auto pixelShader = dxCommon_->CompileShader(
				L"resources/shaders/Sprite.PS.hlsl", L"ps_6_0"
			);
			assert(vertexShader && pixelShader);
			D3D12_BLEND_DESC blend{};
			blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			blend.RenderTarget[0].BlendEnable = TRUE;
			blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
			blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
			blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
			blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
			blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
			D3D12_RASTERIZER_DESC rasterizer{};
			rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
			rasterizer.CullMode = D3D12_CULL_MODE_NONE;
			rasterizer.DepthClipEnable = TRUE;
			D3D12_DEPTH_STENCIL_DESC depth{};
			depth.DepthEnable = FALSE;
			depth.StencilEnable = FALSE;
			D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
			desc.pRootSignature = rootSignature_.Get();
			desc.InputLayout = inputLayout;
			desc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
			desc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
			desc.BlendState = blend;
			desc.RasterizerState = rasterizer;
			desc.DepthStencilState = depth;
			desc.NumRenderTargets = 1;
			desc.RTVFormats[0] = format;
			desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			desc.SampleDesc.Count = 1;
			desc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
			const HRESULT pipelineResult = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
				&desc, IID_PPV_ARGS(&pipelineState)
			);
			assert(SUCCEEDED(pipelineResult));
		}

		DirectXCommon* dxCommon_ = nullptr;
		Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> displayPipelineState_;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> sceneHdrPipelineState_;
		bool initialized_ = false;
	};

	TextSpritePipeline& GetPipeline() {
		static TextSpritePipeline pipeline;
		return pipeline;
	}
}

void TextSprite::Initialize(DirectXCommon* dxCommon, std::string textureKey) {
	dxCommon_ = dxCommon;
	textureKey_ = std::move(textureKey);
	GetPipeline().Initialize(dxCommon_);
	CreateResources();
}

void TextSprite::CreateResources() {
	vertexResource_ = dxCommon_->CreateBufferResource(sizeof(VertexData) * 4);
	indexResource_ = dxCommon_->CreateBufferResource(sizeof(uint32_t) * 6);
	materialResource_ = dxCommon_->CreateBufferResource(sizeof(Material));
	transformationResource_ = dxCommon_->CreateBufferResource(sizeof(TransformationMatrix));
	vertexResource_->Map(0, nullptr, reinterpret_cast<void**>(&vertexData_));
	indexResource_->Map(0, nullptr, reinterpret_cast<void**>(&indexData_));
	materialResource_->Map(0, nullptr, reinterpret_cast<void**>(&materialData_));
	transformationResource_->Map(0, nullptr, reinterpret_cast<void**>(&transformationData_));
	vertexBufferView_.BufferLocation = vertexResource_->GetGPUVirtualAddress();
	vertexBufferView_.SizeInBytes = sizeof(VertexData) * 4;
	vertexBufferView_.StrideInBytes = sizeof(VertexData);
	indexBufferView_.BufferLocation = indexResource_->GetGPUVirtualAddress();
	indexBufferView_.SizeInBytes = sizeof(uint32_t) * 6;
	indexBufferView_.Format = DXGI_FORMAT_R32_UINT;
	indexData_[0] = 0; indexData_[1] = 1; indexData_[2] = 2;
	indexData_[3] = 1; indexData_[4] = 3; indexData_[5] = 2;
	materialData_->enableLighting = false;
	materialData_->uvTransform = MakeIdentity4x4();
	transformationData_->WVP = MakeIdentity4x4();
	transformationData_->World = MakeIdentity4x4();
}

void TextSprite::Update(
	const Vector2& position,
	float rotation,
	const Vector2& size,
	const Vector2& pivot,
	const Vector4& color,
	uint32_t canvasWidth,
	uint32_t canvasHeight
) {
	const float left = -pivot.x;
	const float right = 1.0f - pivot.x;
	const float top = -pivot.y;
	const float bottom = 1.0f - pivot.y;
	vertexData_[0] = { { left, bottom, 0.0f, 1.0f }, { 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f } };
	vertexData_[1] = { { left, top, 0.0f, 1.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f } };
	vertexData_[2] = { { right, bottom, 0.0f, 1.0f }, { 1.0f, 1.0f }, { 0.0f, 0.0f, -1.0f } };
	vertexData_[3] = { { right, top, 0.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f, -1.0f } };
	materialData_->color = color;
	const Matrix4x4 world = MakeAffineMatrix(
		{ size.x, size.y, 1.0f },
		{ 0.0f, 0.0f, rotation },
		{ position.x, position.y, 0.0f }
	);
	const Matrix4x4 projection = MakeOrthographicMatrix(
		0.0f,
		0.0f,
		static_cast<float>((std::max)(canvasWidth, 1u)),
		static_cast<float>((std::max)(canvasHeight, 1u)),
		0.0f,
		100.0f
	);
	transformationData_->WVP = Multiply(world, projection);
	transformationData_->World = MakeIdentity4x4();
}

void TextSprite::UpdateUI(const Matrix4x4& unitQuadToClip, const Vector4& color) {
	Update({ 0.0f, 0.0f }, 0.0f, { 1.0f, 1.0f }, { 0.0f, 0.0f }, color, 1, 1);
	transformationData_->WVP = unitQuadToClip;
}

void TextSprite::Draw(OutputTarget target, bool editorView) const {
	if (!TextureManager::GetInstance()->HasTexture(textureKey_)) {
		return;
	}
	// 描画済みのViewが参照するupload領域へ、後のViewのUpdateを書き込まない。
	auto& buffers = viewDrawBuffers_[editorView ? 1 : 0];
	if (!buffers.vertices) {
		buffers.vertices = dxCommon_->CreateBufferResource(sizeof(VertexData) * 4);
		buffers.material = dxCommon_->CreateBufferResource(sizeof(Material));
		buffers.transform = dxCommon_->CreateBufferResource(sizeof(TransformationMatrix));
	}
	auto snapshot = [](ID3D12Resource* resource, const void* data, size_t bytes) {
		void* mapped = nullptr;
		if (FAILED(resource->Map(0, nullptr, &mapped))) return false;
		std::memcpy(mapped, data, bytes);
		resource->Unmap(0, nullptr);
		return true;
	};
	if (!snapshot(buffers.vertices.Get(), vertexData_, sizeof(VertexData) * 4) ||
		!snapshot(buffers.material.Get(), materialData_, sizeof(Material)) ||
		!snapshot(buffers.transform.Get(), transformationData_, sizeof(TransformationMatrix))) return;
	auto viewVertices = vertexBufferView_;
	viewVertices.BufferLocation = buffers.vertices->GetGPUVirtualAddress();
	GetPipeline().SetCommonRenderState(target);
	ID3D12GraphicsCommandList* commandList = dxCommon_->GetCommandList();
	commandList->IASetVertexBuffers(0, 1, &viewVertices);
	commandList->IASetIndexBuffer(&indexBufferView_);
	commandList->SetGraphicsRootConstantBufferView(0, buffers.material->GetGPUVirtualAddress());
	commandList->SetGraphicsRootConstantBufferView(1, buffers.transform->GetGPUVirtualAddress());
	commandList->SetGraphicsRootDescriptorTable(
		2,
		TextureManager::GetInstance()->GetSrvHandleGPU(textureKey_)
	);
	commandList->DrawIndexedInstanced(6, 1, 0, 0, 0);
}
