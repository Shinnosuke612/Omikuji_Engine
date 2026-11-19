// 役割: Box/Circleの地面に沿った帯を作り、Viewごとに分離した領域で描画する。
#include "MovementBoundaryRenderer.h"
#include "../3d/Camera.h"
#include "../base/DirectXCommon.h"
#include "../base/RenderFormats.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <d3dcompiler.h>
#include <exception>
#include <numbers>

void MovementBoundaryRenderer::BeginFrame(uint64_t serial) {
	if (serial != serial_) {
		serial_ = serial;
		slot_ = 0;
	}
}

void MovementBoundaryRenderer::Draw(const Camera* camera, const SceneMovementBoundsFrame& frame) {
	const auto& s = frame.settings;
	if (!camera || !device_ || !serial_ || slot_ >= kViews || frame.opacity <= 0.0f ||
		s.color.w <= 0.0f || SceneMovementBounds::SettingsIssue(s)) return;
	if (!attempted_) {
		// 通常Sceneは従来どおり最初に描く時点で全資源を確保する。
		SceneRuntimePrepareBudget immediate{0, 4, UINT64_MAX};
		std::string diagnostic;
		StepPrepared(immediate, diagnostic);
	}
	auto* list = device_->GetCommandList();
	if (prepareStage_ != PrepareStage::Ready || !list) return;
	const float halfWidth = s.lineWidth * 0.5f;
	const bool circle = s.shape == "Circle";
	const UINT segments = circle ? kCircleSegments : 4;
	constexpr std::array<Vector2, 4> corners{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
	auto point = [&](UINT index, float width) -> Vector3 {
		Vector2 offset;
		if (circle) {
			const float angle = 2.0f * std::numbers::pi_v<float> * (index % segments) / segments;
			const float radius = (std::max)(s.radius + width, 0.0f);
			offset = {radius * std::cos(angle), radius * std::sin(angle)};
		} else {
			const auto corner = corners[index % segments];
			offset = {corner.x * (std::max)(s.halfExtents.x + width, 0.0f),
				corner.y * (std::max)(s.halfExtents.y + width, 0.0f)};
		}
		return {s.center.x + offset.x, s.boundaryY, s.center.y + offset.y};
	};
	const Vector4 color{s.color.x, s.color.y, s.color.z, s.color.w * std::clamp(frame.opacity, 0.0f, 1.0f)};
	const UINT firstVertex = slot_ * kVerticesPerView;
	Vertex* output = vertexData_ + firstVertex;
	for (UINT i = 0; i < segments; ++i) {
		const auto a = point(i, halfWidth), b = point(i + 1, halfWidth);
		const auto c = point(i, -halfWidth), d = point(i + 1, -halfWidth);
		for (const auto& p : {a, b, c, b, d, c}) {
			if (!std::isfinite(p.x) || !std::isfinite(p.z)) return;
			*output++ = {p, color};
		}
	}
	const UINT cameraOffset = slot_++ * kCameraStride;
	const auto matrix = camera->GetViewProjectionMatrix();
	std::memcpy(cameraData_ + cameraOffset, &matrix, sizeof(matrix));
	const D3D12_VERTEX_BUFFER_VIEW view{vertices_->GetGPUVirtualAddress() + UINT64(firstVertex) * sizeof(Vertex),
		static_cast<UINT>(segments * 6 * sizeof(Vertex)), sizeof(Vertex)};
	list->SetGraphicsRootSignature(root_.Get());
	list->SetPipelineState(pipeline_.Get());
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	list->IASetVertexBuffers(0, 1, &view);
	list->SetGraphicsRootConstantBufferView(0, cameras_->GetGPUVirtualAddress() + cameraOffset);
	list->DrawInstanced(segments * 6, 1, 0, 0);
}

RuntimePrepareStep MovementBoundaryRenderer::StepPrepared(
	SceneRuntimePrepareBudget& remaining, std::string& diagnostic) {
	using Step = RuntimePrepareStep;
	attempted_ = true;
	diagnostic.clear();
	if (prepareStage_ == PrepareStage::Ready) return Step::Ready;
	const auto fail = [&](const char* reason) {
		prepareStage_ = PrepareStage::Failed;
		if (diagnostic.empty()) diagnostic = reason;
		return Step::Failed;
	};
	if (prepareStage_ == PrepareStage::Failed || !device_ || !device_->GetDevice())
		return fail("Movement boundary preparation is unavailable.");
	if (!remaining.maxCopyBytes) return Step::Pending;
	try {
		while (remaining.maxResourceCreates) {
			--remaining.maxResourceCreates;
			D3D12_RANGE readRange{0, 0};
			switch (prepareStage_) {
			case PrepareStage::Root:
				if (!CreateRoot()) return fail("Failed to create the movement boundary root signature.");
				prepareStage_ = PrepareStage::Pipeline;
				break;
			case PrepareStage::Pipeline:
				if (!CreatePipeline(diagnostic)) return fail("Failed to create the movement boundary pipeline.");
				prepareStage_ = PrepareStage::Vertices;
				break;
			case PrepareStage::Vertices:
				if (!device_->TryCreateBufferResource(sizeof(Vertex) * kVerticesPerView * kViews, vertices_) ||
					!vertices_ || FAILED(vertices_->Map(0, &readRange, reinterpret_cast<void**>(&vertexData_))) ||
					!vertexData_) return fail("Failed to create or map movement boundary vertices.");
				prepareStage_ = PrepareStage::Cameras;
				break;
			case PrepareStage::Cameras:
				if (!device_->TryCreateBufferResource(kCameraStride * kViews, cameras_) ||
					!cameras_ || FAILED(cameras_->Map(0, &readRange, reinterpret_cast<void**>(&cameraData_))) ||
					!cameraData_) return fail("Failed to create or map movement boundary camera constants.");
				// Uploadへの初回書込みは既存DrawのView別領域で行う。
				prepareStage_ = PrepareStage::Ready;
				return Step::Ready;
			default:
				return fail("Invalid movement boundary preparation stage.");
			}
		}
		return Step::Pending;
	} catch (const std::exception&) {
		return fail("Failed to prepare movement boundary resources.");
	}
}

bool MovementBoundaryRenderer::CreateRoot() {
	D3D12_ROOT_PARAMETER camera{};
	camera.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	camera.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	D3D12_ROOT_SIGNATURE_DESC signature{};
	signature.NumParameters = 1;
	signature.pParameters = &camera;
	signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	Microsoft::WRL::ComPtr<ID3DBlob> blob;
	return SUCCEEDED(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)) &&
		blob && SUCCEEDED(device_->GetDevice()->CreateRootSignature(0, blob->GetBufferPointer(),
			blob->GetBufferSize(), IID_PPV_ARGS(&root_))) && root_;
}

bool MovementBoundaryRenderer::CreatePipeline(std::string& diagnostic) {
	Microsoft::WRL::ComPtr<IDxcBlob> vs, ps;
	if (!device_->TryCompileShader(L"resources/shaders/Lightning.VS.hlsl", L"vs_6_0", vs, diagnostic) ||
		!device_->TryCompileShader(L"resources/shaders/Lightning.PS.hlsl", L"ps_6_0", ps, diagnostic))
		return false;
	const D3D12_INPUT_ELEMENT_DESC input[] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
	};
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = root_.Get();
	desc.InputLayout = {input, _countof(input)};
	desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
	desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
	auto& blend = desc.BlendState.RenderTarget[0];
	blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	blend.BlendEnable = TRUE;
	blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
	blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	blend.BlendOp = D3D12_BLEND_OP_ADD;
	blend.SrcBlendAlpha = D3D12_BLEND_ONE;
	blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.DepthClipEnable = TRUE;
	desc.DepthStencilState.DepthEnable = TRUE;
	desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	desc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = RenderFormats::kSceneHdrFormat;
	desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	desc.SampleDesc.Count = 1;
	return SUCCEEDED(device_->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline_))) &&
		pipeline_;
}
