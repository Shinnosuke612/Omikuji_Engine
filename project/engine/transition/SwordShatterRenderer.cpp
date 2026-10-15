// 役割: Display Captureと独立depthを使い、Assetの断片・傷・発光・Flashを合成する。
#include "SwordShatterRenderer.h"

#include "../base/DirectXCommon.h"
#include "../base/SceneRenderTarget.h"
#include "../3d/SrvManager.h"
#include "../math/Matrix4x4.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>

namespace {
using namespace SceneTransitionAsset;
using Microsoft::WRL::ComPtr;
struct Vertex { Vector2 uv; std::uint32_t fragment; };
struct GpuFragment { Matrix4x4 localToClip; Vector2 centroid; std::uint32_t firstEdge, edgeCount; };
struct GpuEdge { Vector2 from, to; };
struct GpuCut { Vector4 segment, lineColor, glowColor, revealGlowWidths, intensity; };
struct Globals { Vector4 viewportAspectPhase, backColor, edgeColor, styleCutCount, flashColor, flashIntensity; };
static_assert(sizeof(Vertex) == 12 && offsetof(Vertex, fragment) == 8);
static_assert(sizeof(GpuFragment) == 80 && offsetof(GpuFragment, centroid) == 64 && offsetof(GpuFragment, firstEdge) == 72);
static_assert(sizeof(GpuEdge) == 16 && sizeof(GpuCut) == 80 && sizeof(Globals) == 96);
static_assert(offsetof(GpuCut, intensity) == 64 && offsetof(Globals, flashIntensity) == 80);
static_assert(MaxCuts == 16 && MaxFragments == 137 && MaxVertices == 548 && MaxIndices == 822);
constexpr std::size_t GlobalsBytes = 256;
static_assert(sizeof(Vertex)*MaxVertices + sizeof(std::uint32_t)*MaxIndices +
    sizeof(GpuFragment)*MaxFragments + sizeof(GpuEdge)*MaxVertices + sizeof(GpuCut)*MaxCuts + GlobalsBytes == 31128);

bool Fail(Diagnostics& diagnostics, const std::string& field, const std::string& message) {
    diagnostics.push_back({field, message});
    return false;
}
bool InRange(float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; }
bool InRange(Vector3 v, float lo, float hi) {
    return InRange(v.x, lo, hi) && InRange(v.y, lo, hi) && InRange(v.z, lo, hi);
}
bool Color(Vector4 v) { return InRange(v.x,0,1) && InRange(v.y,0,1) && InRange(v.z,0,1) && InRange(v.w,0,1); }
bool Same(Vector2 a, Vector2 b) { return a.x == b.x && a.y == b.y; }
bool Same(Vector4 a, Vector4 b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
bool ValidSize(std::uint32_t w, std::uint32_t h) {
    return w > 0 && h > 0 && w <= SwordShatterRenderer::MaxImageDimension && h <= SwordShatterRenderer::MaxImageDimension;
}
bool ValidTexture(SwordShatterRenderer::TextureView v, std::uint32_t w, std::uint32_t h) {
    return v.srv.ptr != 0 && v.format == RenderFormats::kDisplayFormat && v.width == w && v.height == h;
}

// 永続Mapのuploadは前Frame Fence完了後だけ再使用する。破棄も同じsafe boundaryに限る。
struct Upload {
    ComPtr<ID3D12Resource> resource;
    void* mapped = nullptr;
    bool Create(ID3D12Device* device, std::size_t bytes) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource));
        if (FAILED(hr) || !resource) return false;
        D3D12_RANGE readRange{0, 0};
        hr = resource->Map(0, &readRange, &mapped);
        return SUCCEEDED(hr) && mapped;
    }
};

// candidateの破棄だけで、途中確保したslot/RT/bufferを回収できるよう所有をまとめる。
struct Prepared {
    SrvManager* manager = nullptr;
    std::array<std::uint32_t,3> slots{SrvManager::kInvalidIndex, SrvManager::kInvalidIndex, SrvManager::kInvalidIndex};
    Upload vertices, indices, fragments, edges, cuts, globals;
    std::unique_ptr<SceneRenderTarget> capture, output;
    std::shared_ptr<const CompiledTransition> asset;
    ~Prepared() {
        if (manager) for (auto slot : slots) if (slot != SrvManager::kInvalidIndex) manager->Free(slot);
    }
    bool CreateBuffers(DirectXCommon& dx, SrvManager& srv) {
        manager = &srv;
        auto* device = dx.GetDevice();
        if (!vertices.Create(device, sizeof(Vertex)*MaxVertices) || !indices.Create(device, sizeof(std::uint32_t)*MaxIndices) ||
            !fragments.Create(device, sizeof(GpuFragment)*MaxFragments) || !edges.Create(device, sizeof(GpuEdge)*MaxVertices) ||
            !cuts.Create(device, sizeof(GpuCut)*MaxCuts) || !globals.Create(device, GlobalsBytes)) return false;
        for (auto& slot : slots) if (!srv.TryAllocate(slot)) return false;
        return srv.CreateSRVforStructuredBuffer(slots[0], fragments.resource.Get(), static_cast<UINT>(MaxFragments), sizeof(GpuFragment)) &&
            srv.CreateSRVforStructuredBuffer(slots[1], cuts.resource.Get(), static_cast<UINT>(MaxCuts), sizeof(GpuCut)) &&
            srv.CreateSRVforStructuredBuffer(slots[2], edges.resource.Get(), static_cast<UINT>(MaxVertices), sizeof(GpuEdge));
    }
};

std::unique_ptr<SceneRenderTarget> CreateTarget(DirectXCommon& dx, SrvManager& srv, std::uint32_t w, std::uint32_t h, bool depth) {
    auto target = std::make_unique<SceneRenderTarget>();
    SceneRenderTarget::Desc desc{};
    desc.width = w; desc.height = h; desc.format = RenderFormats::kDisplayFormat; desc.createDepth = depth;
    if (!target->Initialize(&dx, &srv, desc)) return {};
    return target;
}

bool FillGeometry(const CompiledTransition& compiled, Prepared& prepared, Diagnostics& diagnostics) {
    const auto& g = compiled.GetGeometry();
    if (g.fragments.empty() || g.fragments.size() > MaxFragments || g.vertices.empty() || g.vertices.size() > MaxVertices ||
        g.indices.empty() || g.indices.size() > MaxIndices || g.indices.size()%3 != 0 ||
        g.cutSegments.size() != compiled.GetAsset().cuts.size() || g.cutSegments.size() > MaxCuts)
        return Fail(diagnostics,"geometry","Geometryの容量/件数が不正です。");
    std::array<Vertex,MaxVertices> vertices{};
    std::array<GpuEdge,MaxVertices> edges{};
    std::size_t vertexEnd = 0, indexEnd = 0, edgeEnd = 0;
    for (std::size_t i=0; i<g.fragments.size(); ++i) {
        const auto& f = g.fragments[i];
        if ((i && g.fragments[i-1].fragmentId >= f.fragmentId) || f.polygon.size() < 3 ||
            f.polygon.size() > MaxVertices-edgeEnd || f.firstVertex != vertexEnd || f.firstIndex != indexEnd ||
            f.polygon.size() > g.vertices.size()-vertexEnd || f.indexCount > g.indices.size()-indexEnd ||
            f.indexCount != (f.polygon.size()-2)*3 || !InRange(f.centroid.x,0,1) || !InRange(f.centroid.y,0,1))
            return Fail(diagnostics,"geometry.fragments","断片のID/offset/容量が不正です。");
        for (std::size_t j=0; j<f.polygon.size(); ++j) {
            const auto& v = g.vertices[vertexEnd+j];
            if (v.fragmentId != f.fragmentId || !Same(v.uv,f.polygon[j]) || !InRange(v.uv.x,0,1) || !InRange(v.uv.y,0,1))
                return Fail(diagnostics,"geometry.vertices","断片の頂点対応が不正です。");
            vertices[vertexEnd+j] = {v.uv, static_cast<std::uint32_t>(i)};
            edges[edgeEnd+j] = {f.polygon[j],f.polygon[(j+1)%f.polygon.size()]};
        }
        for (std::size_t j=0; j<f.indexCount; ++j) {
            const auto index = g.indices[indexEnd+j];
            if (index < vertexEnd || index >= vertexEnd+f.polygon.size())
                return Fail(diagnostics,"geometry.indices","断片外のindexです。");
        }
        vertexEnd += f.polygon.size(); indexEnd += f.indexCount; edgeEnd += f.polygon.size();
    }
    if (vertexEnd != g.vertices.size() || indexEnd != g.indices.size()) return Fail(diagnostics,"geometry","未対応の頂点/indexです。");
    // signed areaやfanを再生成せず、Assetの生成結果をそのままdense indexへ接続する。
    std::memcpy(prepared.vertices.mapped,vertices.data(),g.vertices.size()*sizeof(Vertex));
    std::memcpy(prepared.indices.mapped,g.indices.data(),g.indices.size()*sizeof(std::uint32_t));
    std::memcpy(prepared.edges.mapped,edges.data(),edgeEnd*sizeof(GpuEdge));
    return true;
}
} // namespace

struct SwordShatterRenderer::Impl {
    DirectXCommon* dx = nullptr;
    SrvManager* srv = nullptr;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> copy, mesh, slash, flash;
    std::unique_ptr<Prepared> prepared;
    std::uint64_t generation = 0, frameSerial = 0;
    bool initialized = false, frameStarted = false, captureValid = false, outputValid = false;
    bool capturedThisFrame = false, composedThisFrame = false, recordedThisFrame = false;

    bool Safe() const { return !recordedThisFrame; }
    bool CreatePipelines(Diagnostics& diagnostics);
    void Bind() const {
        auto* list = dx->GetCommandList();
        ID3D12DescriptorHeap* heaps[] = {srv->GetDescriptorHeap()};
        list->SetDescriptorHeaps(1,heaps);
        list->SetGraphicsRootSignature(root.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
    void CopyTexture(D3D12_GPU_DESCRIPTOR_HANDLE source) const {
        auto* list = dx->GetCommandList();
        Bind();
        list->SetPipelineState(copy.Get());
        list->SetGraphicsRootDescriptorTable(0,source);
        list->DrawInstanced(3,1,0,0);
    }
};

bool SwordShatterRenderer::Impl::CreatePipelines(Diagnostics& diagnostics) {
    std::array<ComPtr<IDxcBlob>,6> shaders;
    const wchar_t* paths[] = {L"resources/shaders/Fullscreen.VS.hlsl", L"resources/shaders/CopyImage.PS.hlsl",
        L"resources/shaders/SwordShatter.VS.hlsl", L"resources/shaders/SwordShatter.PS.hlsl",
        L"resources/shaders/SwordSlash.PS.hlsl", L"resources/shaders/SwordFlash.PS.hlsl"};
    for (std::size_t i=0; i<_countof(paths); ++i) {
        std::string diagnostic;
        if (!dx->TryCompileShader(paths[i], (i==0 || i==2) ? L"vs_6_0" : L"ps_6_0", shaders[i], diagnostic))
            return Fail(diagnostics,"shader",diagnostic);
    }
    D3D12_DESCRIPTOR_RANGE ranges[4]{};
    D3D12_ROOT_PARAMETER params[5]{};
    for (UINT i=0; i<4; ++i) {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = 1; ranges[i].BaseShaderRegister = i;
        ranges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].ShaderVisibility = i==1 ? D3D12_SHADER_VISIBILITY_ALL : D3D12_SHADER_VISIBILITY_PIXEL;
        params[i].DescriptorTable = {1,&ranges[i]};
    }
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[4].Descriptor.ShaderRegister = 0;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxAnisotropy = 1;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    rootDesc.NumParameters = _countof(params); rootDesc.pParameters = params;
    rootDesc.NumStaticSamplers = 1; rootDesc.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> signature, error;
    HRESULT hr = D3D12SerializeRootSignature(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&error);
    if (FAILED(hr) || !signature) return Fail(diagnostics,"root",error ? std::string(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()) : "Root Signature serialization失敗。");
    hr = dx->GetDevice()->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root));
    if (FAILED(hr) || !root) return Fail(diagnostics,"root","Root Signature生成失敗。");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root.Get();
    pso.VS = {shaders[0]->GetBufferPointer(),shaders[0]->GetBufferSize()};
    pso.PS = {shaders[1]->GetBufferPointer(),shaders[1]->GetBufferSize()};
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.FrontCounterClockwise = FALSE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    auto& blend = pso.BlendState.RenderTarget[0];
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pso.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    pso.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    pso.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
    pso.DepthStencilState.BackFace = pso.DepthStencilState.FrontFace;
    pso.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1; pso.RTVFormats[0] = RenderFormats::kDisplayFormat; pso.SampleDesc.Count = 1;
    auto create = [&](ComPtr<ID3D12PipelineState>& target, const char* name) {
        const HRESULT result = dx->GetDevice()->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(&target));
        return (SUCCEEDED(result) && target) || Fail(diagnostics,name,"PSO生成失敗。");
    };
    if (!create(copy,"copy")) return false;
    pso.PS = {shaders[5]->GetBufferPointer(),shaders[5]->GetBufferSize()};
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    if (!create(flash,"flash")) return false;
    const D3D12_INPUT_ELEMENT_DESC elements[] = {
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"FRAGMENT",0,DXGI_FORMAT_R32_UINT,0,8,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    pso.InputLayout = {elements,_countof(elements)};
    pso.VS = {shaders[2]->GetBufferPointer(),shaders[2]->GetBufferSize()};
    pso.PS = {shaders[3]->GetBufferPointer(),shaders[3]->GetBufferSize()};
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DSVFormat = RenderFormats::kDepthDsvFormat;
    blend.BlendEnable = FALSE;
    if (!create(mesh,"mesh")) return false;
    pso.PS = {shaders[4]->GetBufferPointer(),shaders[4]->GetBufferSize()};
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    blend.BlendEnable = TRUE;
    blend.SrcBlend = blend.DestBlend = blend.SrcBlendAlpha = blend.DestBlendAlpha = D3D12_BLEND_ONE;
    return create(slash,"slash");
}

SwordShatterRenderer::SwordShatterRenderer() : impl_(std::make_unique<Impl>()) {}
SwordShatterRenderer::~SwordShatterRenderer() = default;

bool SwordShatterRenderer::TryInitialize(DirectXCommon& dx, SrvManager& srv, Diagnostics& diagnostics) {
    diagnostics.clear();
    if (impl_->initialized || !dx.GetDevice() || !dx.GetCommandList() || !srv.GetDescriptorHeap())
        return Fail(diagnostics,"initialize","初期化済み、またはdevice/managerが未準備です。");
    auto candidate = std::make_unique<Impl>();
    candidate->dx = &dx; candidate->srv = &srv;
    if (!candidate->CreatePipelines(diagnostics)) return false;
    candidate->prepared = std::make_unique<Prepared>();
    if (!candidate->prepared->CreateBuffers(dx,srv)) return Fail(diagnostics,"buffers","Upload/Map/SRV準備失敗。");
    candidate->initialized = true;
    impl_.swap(candidate);
    return true;
}

void SwordShatterRenderer::BeginFrame(std::uint64_t serial) {
    auto& r = *impl_;
    if (!r.initialized || (r.frameStarted && serial <= r.frameSerial)) return;
    r.frameSerial = serial; r.frameStarted = true;
    r.capturedThisFrame = r.composedThisFrame = r.recordedThisFrame = false;
}

bool SwordShatterRenderer::TryPrepare(std::shared_ptr<const CompiledTransition> asset,
    std::uint32_t w, std::uint32_t h, Diagnostics& diagnostics) {
    diagnostics.clear(); auto& r = *impl_;
    if (!r.initialized || !r.Safe() || !asset || !ValidSize(w,h) || r.generation == (std::numeric_limits<std::uint64_t>::max)())
        return Fail(diagnostics,"prepare","初期化/safe boundary/snapshot/寸法/世代を確認してください。");
    auto candidate = std::make_unique<Prepared>();
    if (!candidate->CreateBuffers(*r.dx,*r.srv)) return Fail(diagnostics,"buffers","Upload/Map/SRV準備失敗。");
    if (!FillGeometry(*asset,*candidate,diagnostics)) return false;
    candidate->capture = CreateTarget(*r.dx,*r.srv,w,h,false);
    candidate->output = CreateTarget(*r.dx,*r.srv,w,h,true);
    if (!candidate->capture || !candidate->output) return Fail(diagnostics,"targets","Capture/Compose RT準備失敗。");
    candidate->asset = std::move(asset);
    r.prepared.swap(candidate);
    ++r.generation; r.captureValid = r.outputValid = false;
    return true;
}

bool SwordShatterRenderer::TryResizeOutput(std::uint32_t w, std::uint32_t h, Diagnostics& diagnostics) {
    diagnostics.clear(); auto& r = *impl_;
    if (!r.initialized || !r.Safe() || !r.prepared || !r.prepared->asset || !ValidSize(w,h))
        return Fail(diagnostics,"resize","未準備、safe boundary外、または寸法が不正です。");
    auto& output = r.prepared->output;
    if (output->GetWidth() == w && output->GetHeight() == h) return true;
    auto candidate = CreateTarget(*r.dx,*r.srv,w,h,true);
    if (!candidate) return Fail(diagnostics,"resize","Compose RT準備失敗。旧出力を維持します。");
    output.swap(candidate); r.outputValid = false;
    return true;
}

bool SwordShatterRenderer::TryCapture(TextureView source, CaptureReceipt& receipt, Diagnostics& diagnostics) {
    diagnostics.clear(); auto& r = *impl_;
    if (!r.initialized || !r.frameStarted || !r.prepared || !r.prepared->asset || r.captureValid || r.capturedThisFrame)
        return Fail(diagnostics,"capture","未準備、またはCapture済みです。再Captureにはsafe Prepareが必要です。");
    auto& p = *r.prepared;
    if (!ValidTexture(source,p.capture->GetWidth(),p.capture->GetHeight()) ||
        source.srv.ptr == p.capture->GetSrvGpuHandle().ptr || source.srv.ptr == p.output->GetSrvGpuHandle().ptr)
        return Fail(diagnostics,"capture.source","Display入力の寸法/format/null/aliasが不正です。");
    p.capture->Begin(false,false);
    r.CopyTexture(source.srv);
    p.capture->End();
    r.captureValid = r.capturedThisFrame = r.recordedThisFrame = true;
    receipt = {r.generation,r.frameSerial};
    return true;
}

bool SwordShatterRenderer::TryCompose(TextureView background, std::shared_ptr<const CompiledTransition> asset,
    const FrameSample& sample, Diagnostics& diagnostics) {
    diagnostics.clear(); auto& r = *impl_;
    if (!r.initialized || !r.frameStarted || !r.captureValid || !r.prepared || !asset ||
        asset.get() != r.prepared->asset.get() || r.composedThisFrame)
        return Fail(diagnostics,"compose","未Capture、異なるsnapshot、または同Frame重複Composeです。");
    auto& p = *r.prepared;
    const auto w = p.output->GetWidth(), h = p.output->GetHeight();
    if (!ValidTexture(background,w,h) || background.srv.ptr == p.output->GetSrvGpuHandle().ptr)
        return Fail(diagnostics,"compose.background","Display背景の寸法/format/null/aliasが不正です。");
    const auto& g = asset->GetGeometry(); const auto& authoring = asset->GetAsset();
    if ((sample.phase != Phase::Cutting && sample.phase != Phase::Collapse) ||
        !InRange(sample.localTime,0,sample.phase == Phase::Cutting ? authoring.cutPhaseDuration : authoring.collapseDuration) ||
        sample.fragments.size() != g.fragments.size() || sample.cuts.size() != g.cutSegments.size() ||
        !Color(sample.flash.color) || !InRange(sample.flash.intensity,0,32))
        return Fail(diagnostics,"sample","phase/time/件数/Flash範囲が不正です。");

    std::array<GpuFragment,MaxFragments> fragments{};
    std::array<GpuCut,MaxCuts> cuts{};
    const float aspect = static_cast<float>(w)/static_cast<float>(h);
    const auto projection = MakePerspectiveFovMatrix(2*std::atan(.5f/CameraDistance),aspect,NearClip,FarClip);
    const auto viewProjection = Multiply(MakeTranslateMatrix({0,0,CameraDistance}),projection);
    constexpr float toRadians = 3.14159265358979323846f/180.0f;
    std::uint32_t firstEdge = 0;
    for (std::size_t i=0; i<sample.fragments.size(); ++i) {
        const auto& pose = sample.fragments[i]; const auto& f = g.fragments[i];
        if (pose.fragmentId != f.fragmentId || !InRange(pose.position,-10000,10000) ||
            !InRange(pose.rotationEulerDegrees,-360000,360000) || !InRange(pose.scale,0,1000))
            return Fail(diagnostics,"sample.fragments","ID/TRS範囲が不正です。");
        if (sample.phase == Phase::Cutting && (pose.position.x != 0 || pose.position.y != 0 || pose.position.z != 0 ||
            pose.rotationEulerDegrees.x != 0 || pose.rotationEulerDegrees.y != 0 || pose.rotationEulerDegrees.z != 0 ||
            pose.scale.x != 1 || pose.scale.y != 1 || pose.scale.z != 1))
            return Fail(diagnostics,"sample.fragments","Cuttingの初期全面被覆が不正です。");
        const Vector3 rotation{std::remainder(pose.rotationEulerDegrees.x,360.0f)*toRadians,
            std::remainder(pose.rotationEulerDegrees.y,360.0f)*toRadians,std::remainder(pose.rotationEulerDegrees.z,360.0f)*toRadians};
        const Vector3 center{(f.centroid.x-.5f)*aspect+pose.position.x,.5f-f.centroid.y+pose.position.y,pose.position.z};
        auto& record = fragments[i];
        record.localToClip = Multiply(MakeAffineMatrix(pose.scale,rotation,center),viewProjection);
        for (const auto& row : record.localToClip.m) for (float value : row)
            if (!std::isfinite(value)) return Fail(diagnostics,"matrix","非有限の描画行列です。");
        record.centroid = f.centroid; record.firstEdge = firstEdge;
        record.edgeCount = static_cast<std::uint32_t>(f.polygon.size()); firstEdge += record.edgeCount;
    }
    bool hasGlow = false;
    for (std::size_t i=0; i<sample.cuts.size(); ++i) {
        const auto& c = sample.cuts[i]; const auto& a = authoring.cuts[i]; const auto& segment = g.cutSegments[i];
        if (!Same(c.segment.from,segment.from) || !Same(c.segment.to,segment.to) || !Color(c.lineColor) || !Color(c.glowColor) ||
            !Same(c.lineColor,a.lineColor) || !Same(c.glowColor,a.glowColor) || c.lineWidth != a.lineWidth ||
            c.glowWidth != a.glowWidth || c.glowIntensity != a.glowIntensity || !InRange(c.reveal,0,1) || !InRange(c.glow,0,1) ||
            !InRange(c.lineWidth,0,1) || c.lineWidth == 0 || !InRange(c.glowWidth,0,1) || c.glowWidth == 0 || !InRange(c.glowIntensity,0,32) ||
            (sample.phase == Phase::Collapse && (c.reveal != 1 || c.glow != 0)))
            return Fail(diagnostics,"sample.cuts","segment/style/reveal/glowがsnapshot契約と不一致です。");
        cuts[i] = {{c.segment.from.x,c.segment.from.y,c.segment.to.x,c.segment.to.y},c.lineColor,c.glowColor,
            {c.reveal,c.glow,c.lineWidth,c.glowWidth},{c.glowIntensity,0,0,0}};
        hasGlow |= c.glow > 0 && c.glowIntensity > 0 && c.glowColor.w > 0;
    }
    const Globals globals{{static_cast<float>(w),static_cast<float>(h),aspect,sample.phase == Phase::Collapse ? 1.0f : 0.0f},
        BackColor,EdgeColor,{EdgeWidthPixels/static_cast<float>(h),1.0f/static_cast<float>(h),static_cast<float>(sample.cuts.size()),0},
        sample.flash.color,{sample.flash.intensity,0,0,0}};

    // 検証後は失敗する準備APIを呼ばない。全Passが同じupload値を読む。
    std::memcpy(p.fragments.mapped,fragments.data(),sample.fragments.size()*sizeof(GpuFragment));
    std::memcpy(p.cuts.mapped,cuts.data(),sample.cuts.size()*sizeof(GpuCut));
    std::memcpy(p.globals.mapped,&globals,sizeof(globals));
    p.output->Begin(false,true);
    r.CopyTexture(background.srv);
    auto* list = r.dx->GetCommandList();
    list->SetGraphicsRootDescriptorTable(0,p.capture->GetSrvGpuHandle());
    for (UINT i=0; i<3; ++i) list->SetGraphicsRootDescriptorTable(i+1,r.srv->GetGPUDescriptorHandle(p.slots[i]));
    list->SetGraphicsRootConstantBufferView(4,p.globals.resource->GetGPUVirtualAddress());
    const D3D12_VERTEX_BUFFER_VIEW vb{p.vertices.resource->GetGPUVirtualAddress(),static_cast<UINT>(g.vertices.size()*sizeof(Vertex)),sizeof(Vertex)};
    const D3D12_INDEX_BUFFER_VIEW ib{p.indices.resource->GetGPUVirtualAddress(),static_cast<UINT>(g.indices.size()*sizeof(std::uint32_t)),DXGI_FORMAT_R32_UINT};
    list->IASetVertexBuffers(0,1,&vb); list->IASetIndexBuffer(&ib);
    list->SetPipelineState(r.mesh.Get());
    list->DrawIndexedInstanced(static_cast<UINT>(g.indices.size()),1,0,0,0);
    if (hasGlow) {
        list->SetPipelineState(r.slash.Get());
        list->DrawIndexedInstanced(static_cast<UINT>(g.indices.size()),1,0,0,0);
    }
    if (sample.flash.intensity > 0 && sample.flash.color.w > 0) {
        list->SetPipelineState(r.flash.Get());
        list->DrawInstanced(3,1,0,0);
    }
    p.output->End();
    r.outputValid = r.composedThisFrame = r.recordedThisFrame = true;
    return true;
}

SwordShatterRenderer::TextureView SwordShatterRenderer::GetCaptureView() const {
    const auto& r = *impl_;
    if (!r.captureValid) return {};
    const auto& rt = *r.prepared->capture;
    return {rt.GetSrvGpuHandle(),rt.GetWidth(),rt.GetHeight(),rt.GetFormat()};
}
SwordShatterRenderer::TextureView SwordShatterRenderer::GetOutputView() const {
    const auto& r = *impl_;
    if (!r.outputValid) return {};
    const auto& rt = *r.prepared->output;
    return {rt.GetSrvGpuHandle(),rt.GetWidth(),rt.GetHeight(),rt.GetFormat()};
}
bool SwordShatterRenderer::HasCapture() const { return impl_->captureValid; }
std::uint64_t SwordShatterRenderer::GetGeneration() const { return impl_->generation; }
void SwordShatterRenderer::ClearAtSafeBoundary() {
    auto& r = *impl_;
    // 終了時には次のBeginFrameがないため、最後のFence完了もcallerが保証する。
    r.captureValid = r.outputValid = false;
    r.prepared.reset();
    r.capturedThisFrame = r.composedThisFrame = r.recordedThisFrame = false;
    if (r.generation != (std::numeric_limits<std::uint64_t>::max)()) ++r.generation;
}
