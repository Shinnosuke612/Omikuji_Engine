// 役割: ロード連動演出の不変生成物と表示時計入力を、CPU描画sampleへ評価する契約を定義する。
#pragma once

#include "LoadLinkedGeneration.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace SceneTransitionAsset {
enum class LoadLinkedEvaluationPhase { Cutting, Interval, Waiting, Frozen, Collapse };
enum class LoadLinkedDrawMode { CaptureCover, Fragments };
// Controllerが時計/本数/receiptと現在世代を所有する。Evaluatorは入力の整合と値計算だけを行う。
struct LoadLinkedPresentationInput {
    std::uint64_t expectedRequestId = 0, expectedSettingsRevision = 0;
    std::uint64_t presentationGeneration = 0, sampleRevision = 0;
    LoadLinkedEvaluationPhase phase = LoadLinkedEvaluationPhase::Cutting;
    std::size_t completedCuts = 0;
    std::size_t cutIndex = 0; // Cuttingだけ。0始まりでcompletedCutsと一致。
    std::size_t frozenCuts = 0; // Frozen/CollapseだけでcompletedCutsと一致。他は0。
    double localTime = 0; // Cutting/Collapseの局所秒。有限値をそのphaseの区間へclampする。
    std::array<double, MaxCuts> completedCutAges{}; // 先頭completedCutsのreceipt以後秒。他とCollapseは0。
    double textAge = 0; // 全phaseで有限/非負。非表示/Pauseの停止とresetはcallerの責務。
    double waitAge = 0; // Waiting/Frozenだけで使用。他は0。
    std::array<float, MaxCuts> waitStartGlows{}; // 待機開始値[0,1]。使用しない要素は0。
};
// generatedが全borrowを延命する。prefixCount非0ならGeometry/bindingは同ownerのFindPrefix(N)が正本。
// CaptureCoverは全面CaptureとM線、Fragmentsは選択Nの線/姿勢。CPU ownerはGPU寿命を保証しない。
struct LoadLinkedFrameSample {
    std::shared_ptr<const LoadLinkedGenerated> generated;
    std::uint64_t presentationGeneration = 0, sampleRevision = 0;
    LoadLinkedEvaluationPhase phase = LoadLinkedEvaluationPhase::Cutting;
    LoadLinkedDrawMode drawMode = LoadLinkedDrawMode::CaptureCover;
    std::size_t completedCuts = 0, prefixCount = 0;
    double evaluatedLocalTime = 0;
    std::vector<CutSample> cuts;
    std::vector<FragmentPose> fragments;
    float fragmentOpacity = 1; // 全破片部に一回適用。次SceneやCover/文字へ掛けない。
    bool textVisible = false;
    float textOpacity = 0; // Rasterize済み色alphaを再乗算しない。
};
// 成功時だけsample全体を交換し診断は空。false/例外時もoutSampleの全値を保持する。
// outSample.generatedを入力参照として渡せる。診断割当失敗/非標準例外は伝播し得る。
// 入出力/診断への同時書込は禁止。別out/診断なら同じ不変ownerを並行共有できる。
// presentationGeneration/sampleRevisionはecho値。採用時の現在世代/取消照合はhostが行う。
bool TryEvaluateLoadLinked(const std::shared_ptr<const LoadLinkedGenerated>& generated,
    const LoadLinkedPresentationInput& input, LoadLinkedFrameSample& outSample, LoadLinkedDiagnostics& diagnostics);
} // namespace SceneTransitionAsset
