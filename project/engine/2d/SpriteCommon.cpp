// 役割: スプライト共通の描画パイプライン初期化を実装する。
#include "SpriteCommon.h"
#include "../base/DirectXCommon.h"
#include "../base/RenderFormats.h"
#include "../utility/Logger.h"
#include <cassert>

SpriteCommon* SpriteCommon::instance_ = nullptr;

SpriteCommon* SpriteCommon::GetInstance() {
	if (instance_ == nullptr) {
		instance_ = new SpriteCommon();
	}
	return instance_;
}

void SpriteCommon::DeleteInstance() {
	delete instance_;
	instance_ = nullptr;
}

void SpriteCommon::Initialize(DirectXCommon* dxCommon) {
	if (isInitialized_) {
		return;
	}

	dxCommon_ = dxCommon;
	GenerateGraphicsPipeline();
	isInitialized_ = true;
}

void SpriteCommon::SetCommonRenderState(){
	dxCommon_->GetCommandList()->SetGraphicsRootSignature(rootSignature_.Get());
	dxCommon_->GetCommandList()->SetPipelineState(graphicsPipelineState_.Get());
	dxCommon_->GetCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void SpriteCommon::MakeRootSignature(){
	HRESULT hr;
	D3D12_DESCRIPTOR_RANGE descriptorRange[1] = {};
	descriptorRange[0].BaseShaderRegister = 0;
	descriptorRange[0].NumDescriptors = 1;
	descriptorRange[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	descriptorRange[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	// RootSignature作成
	D3D12_ROOT_SIGNATURE_DESC descriptionRootSignature{};
	descriptionRootSignature.Flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;


	//RootParameter作成、複数指定できるので配列。
	D3D12_ROOT_PARAMETER rootParameters[3] = {};
	rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	rootParameters[0].Descriptor.ShaderRegister = 0;
	rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	rootParameters[1].Descriptor.ShaderRegister = 0;
	rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	rootParameters[2].DescriptorTable.pDescriptorRanges = descriptorRange;
	rootParameters[2].DescriptorTable.NumDescriptorRanges = _countof(descriptorRange);

	descriptionRootSignature.pParameters = rootParameters;
	descriptionRootSignature.NumParameters = _countof(rootParameters);
	D3D12_STATIC_SAMPLER_DESC staticSampler = MakeStaticSamplerS0();
	descriptionRootSignature.pStaticSamplers = &staticSampler;   // ★追加
	descriptionRootSignature.NumStaticSamplers = 1;                // ★追加

	// シリアライズしてバイナリにする
	ID3DBlob* signatureBlob = nullptr;
	ID3DBlob* errorBlob = nullptr;
	hr = D3D12SerializeRootSignature(&descriptionRootSignature,
									 D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob);
	if(FAILED(hr)){
		if (errorBlob) {
			Logger::Log(reinterpret_cast<char*>(errorBlob->GetBufferPointer()));
		}
		assert(false);
	}

	// バイナリを元に生成
	
	hr = dxCommon_->GetDevice()->CreateRootSignature(0,
													signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(),
													IID_PPV_ARGS(&rootSignature_));
	assert(SUCCEEDED(hr));
}

void SpriteCommon::GenerateGraphicsPipeline(){
	MakeRootSignature();
	HRESULT hr;
	//DepthStencilStateの設定
	D3D12_DEPTH_STENCIL_DESC depthStencilDesc{};
	//Depth機能を有効化する
	depthStencilDesc.DepthEnable = true;
	//書き込みします
	depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	//比較関数はLessEqual。つまり、近ければ描画される
	depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

	//InputLayout
	D3D12_INPUT_ELEMENT_DESC inputElementDescs[3] = {};
	inputElementDescs[0].SemanticName = "POSITION";
	inputElementDescs[0].SemanticIndex = 0;
	inputElementDescs[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	inputElementDescs[0].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
	inputElementDescs[1].SemanticName = "TEXCOORD";
	inputElementDescs[1].SemanticIndex = 0;
	inputElementDescs[1].Format = DXGI_FORMAT_R32G32_FLOAT;
	inputElementDescs[1].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
	inputElementDescs[2].SemanticName = "NORMAL";
	inputElementDescs[2].SemanticIndex = 0;
	inputElementDescs[2].Format = DXGI_FORMAT_R32G32B32_FLOAT;
	inputElementDescs[2].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
	D3D12_INPUT_LAYOUT_DESC inputLayoutDesc = {};
	inputLayoutDesc.pInputElementDescs = inputElementDescs;
	inputLayoutDesc.NumElements = _countof(inputElementDescs);

	BlendMode currentBlendMode = BlendMode::kBlendModeNormal;

	// BlendStateの設定
	D3D12_BLEND_DESC blendDesc{};
	// すべての色要素を書き込む
	blendDesc.RenderTarget[0].RenderTargetWriteMask =
		D3D12_COLOR_WRITE_ENABLE_ALL;
	blendDesc.RenderTarget[0].BlendEnable = TRUE;
	if(currentBlendMode == BlendMode::kBlendModeNormal){
		blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	} else if(currentBlendMode == BlendMode::kBlendModeAdd){
		blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
	} else if(currentBlendMode == BlendMode::kBlendModeSubtract){
		blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_REV_SUBTRACT;
		blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
	} else if(currentBlendMode == BlendMode::kBlendModeMultiply){
		blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_SRC_COLOR;
	}

	else if(currentBlendMode == BlendMode::kBlendModeScreen){
		blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_INV_DEST_COLOR;
		blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
	}
	blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;

	// RasterizerStateの設定
	D3D12_RASTERIZER_DESC rasterizerDesc{};
	// 裏面（時計回り）を表示しない
	rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
	// 三角形の中を塗りつぶす
	rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;

	rasterizerDesc.DepthClipEnable = TRUE;

	// Shaderをコンパイルする
	Microsoft::WRL::ComPtr<IDxcBlob> vertexShaderBlob = dxCommon_->CompileShader(
		L"resources/shaders/Sprite.VS.hlsl",
		L"vs_6_0"
	);
	assert(vertexShaderBlob != nullptr);

	Microsoft::WRL::ComPtr<IDxcBlob> pixelShaderBlob = dxCommon_->CompileShader(
		L"resources/shaders/Sprite.PS.hlsl",
		L"ps_6_0"
	);
	assert(pixelShaderBlob != nullptr);

	D3D12_GRAPHICS_PIPELINE_STATE_DESC graphicsPipelineStateDesc{};
	graphicsPipelineStateDesc.pRootSignature = *&rootSignature_; // RootSignature
	graphicsPipelineStateDesc.InputLayout = inputLayoutDesc;  // InputLayout
	graphicsPipelineStateDesc.VS = { vertexShaderBlob->GetBufferPointer(),
									 vertexShaderBlob->GetBufferSize() }; // VertexShader
	graphicsPipelineStateDesc.PS = { pixelShaderBlob->GetBufferPointer(),
									 pixelShaderBlob->GetBufferSize() };  // PixelShader
	graphicsPipelineStateDesc.BlendState = blendDesc;         // BlendState
	graphicsPipelineStateDesc.RasterizerState = rasterizerDesc; // RasterizerState

	// 書き込むRTVの情報
	graphicsPipelineStateDesc.NumRenderTargets = 1;
	graphicsPipelineStateDesc.RTVFormats[0] = RenderFormats::kSceneHdrFormat;

	// 利用するトポロジ（形状）のタイプ。三角形
	graphicsPipelineStateDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

	//どのように画面に色を打ち込むかの設定（気にしなくていい）
	graphicsPipelineStateDesc.SampleDesc.Count = 1;
	graphicsPipelineStateDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;

	//DepthStencilの設定
	graphicsPipelineStateDesc.DepthStencilState = depthStencilDesc;
	graphicsPipelineStateDesc.DSVFormat = RenderFormats::kDepthDsvFormat;

	//実際に生成
	hr = dxCommon_->GetDevice()->CreateGraphicsPipelineState(&graphicsPipelineStateDesc,
															IID_PPV_ARGS(&graphicsPipelineState_));
	assert(SUCCEEDED(hr));

}

inline D3D12_STATIC_SAMPLER_DESC SpriteCommon::MakeStaticSamplerS0(){
	D3D12_STATIC_SAMPLER_DESC s{};
	s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	s.ShaderRegister = 0; // s0
	s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	return s;
}
