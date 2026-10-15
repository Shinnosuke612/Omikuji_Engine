// 役割: 旧Display画像と断片演出のGPU資源を所有し、評価済みsampleを描画する。
#pragma once

#include "SceneTransitionAsset.h"
#include "../base/RenderFormats.h"
#include <d3d12.h>
#include <memory>

class DirectXCommon;
class SrvManager;

// main thread専用。device/managerは非所有で、このinstanceより長く生存させる。
// Scene、時計、Audio、Eventは扱わない。Runtime/Previewはそれぞれ別instanceを所有する。
class SwordShatterRenderer {
public:
    struct TextureView {
        D3D12_GPU_DESCRIPTOR_HANDLE srv{};
        std::uint32_t width = 0, height = 0;
        DXGI_FORMAT format = RenderFormats::kDisplayFormat;
    };
    struct CaptureReceipt { std::uint64_t generation = 0, frameSerial = 0; };

    // Rendererの技術/材質値。Assetの制作値や保存schemaには含めない。
    static constexpr float CameraDistance = 1.0f, NearClip = .01f, FarClip = 20000.0f;
    static constexpr Vector4 BackColor{.035f, .045f, .060f, 1};
    static constexpr Vector4 EdgeColor{.12f, .14f, .18f, 1};
    static constexpr float EdgeWidthPixels = 1.5f;
    static constexpr std::uint32_t MaxImageDimension = 4096;

    SwordShatterRenderer();
    // callerが最後のGPU使用完了を保証してから破棄する。
    ~SwordShatterRenderer();
    SwordShatterRenderer(const SwordShatterRenderer&) = delete;
    SwordShatterRenderer& operator=(const SwordShatterRenderer&) = delete;

    // 初期化一回。失敗では未初期化を維持し、diagnosticsは今回の結果へ置換する。
    bool TryInitialize(DirectXCommon&, SrvManager&, SceneTransitionAsset::Diagnostics&);
    // 前FrameのPostDraw正常復帰後に一回。serialは単調増加、演出時計ではない。
    void BeginFrame(std::uint64_t frameSerial);
    // safe Update専用。失敗時snapshot/画像/世代は不変。成功でCapture/出力を無効化する。
    bool TryPrepare(std::shared_ptr<const SceneTransitionAsset::CompiledTransition>,
        std::uint32_t width, std::uint32_t height, SceneTransitionAsset::Diagnostics&);
    // safe Update専用。成功時もCapture内容/寸法/snapshotを維持し、出力だけ無効化する。
    bool TryResizeOutput(std::uint32_t width, std::uint32_t height, SceneTransitionAsset::Diagnostics&);
    // Draw専用。一度のPrepareにつき一回のCapture。false時receiptは不変。
    // sourceはPSR状態のDisplay画像。callerがresource/descriptorをPostDrawまで保持する。
    // 成功はcommand記録でありGPU完了ではない。
    bool TryCapture(TextureView source, CaptureReceipt&, SceneTransitionAsset::Diagnostics&);
    // Draw専用、一Frame一回。sampleは同じsnapshotのTryEvaluate結果であること。
    // 全入力を検証後にcommandを記録し、失敗時は前の有効出力を維持する。
    bool TryCompose(TextureView background,
        std::shared_ptr<const SceneTransitionAsset::CompiledTransition> sourceAsset,
        const SceneTransitionAsset::FrameSample&, SceneTransitionAsset::Diagnostics&);

    // 借用SRV。無効なら空。成功Prepare/Resize/Clearによる交換をGUI使用Frame内に行わない。
    TextureView GetCaptureView() const;
    TextureView GetOutputView() const;
    bool HasCapture() const;
    std::uint64_t GetGeneration() const;
    // safe Update/最後のGPU使用完了後だけ。初期化済みpipelineは残し、演出資源を解放する。
    void ClearAtSafeBoundary();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
