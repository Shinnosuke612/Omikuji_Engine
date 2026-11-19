// 役割: ロード連動演出の純CPU生成物と、切断候補・本数別幾何・散乱係数の段階生成を定義する。
#pragma once

#include "SceneTransitionAsset.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace SceneTransitionAsset {
struct LoadLinkedGenerationInput {
    std::uint64_t requestId = 0; // 非0。callerは未失効の要求IDを再利用しない。Sceneの失効検査は別途必要。
    std::uint64_t settingsRevision = 0; // 0も有効なcallerの値。
    std::shared_ptr<const LoadLinkedAsset> settings;
    std::uint32_t captureWidth = 0, captureHeight = 0;
    std::uint32_t cutSeed = 0, motionSeed = 0; // Fixedは保存値と一致。AuthoredのcutSeedは内部で0にする。
};
enum class LoadLinkedGenerationState {
    Idle, GeneratingCuts, GeneratingMotion, Completed, Failed, Cancelled
};
enum class LoadLinkedGenerationFailureKind {
    None, InvalidInput, TrialLimit, Geometry, Motion, Internal, Allocation
};
enum class LoadLinkedCandidateRejection {
    Clip, FloatCoordinates, TooShort, AngleSeparation,
    CutSeparation, Geometry, Count
};
// 内部の生成進捗。ロード全体の百分率やGPU/Scene Readyを表さない。
struct LoadLinkedGenerationStatus {
    LoadLinkedGenerationState state = LoadLinkedGenerationState::Idle;
    std::uint64_t requestId = 0, settingsRevision = 0;
    std::uint32_t captureWidth = 0, captureHeight = 0;
    std::uint32_t cutSeed = 0, motionSeed = 0;
    std::size_t acceptedCuts = 0, completedMotions = 0, totalMotions = 0;
    std::uint32_t attemptsForCurrentCut = 0, totalAttempts = 0;
    std::uint32_t randomGenerationVersion = 0; // Authoredは非該当0。
    bool resultTaken = false;
};
struct LoadLinkedGenerationFailure {
    LoadLinkedGenerationFailureKind kind = LoadLinkedGenerationFailureKind::None;
    std::size_t cutIndex = 0, prefixCount = 0;
    std::uint16_t fragmentId = 0; // 0も有効。hasFragmentで区別。
    bool hasFragment = false;
    GeometryBuildStatus geometryStatus = GeometryBuildStatus::Success;
    std::array<std::uint32_t,
        static_cast<std::size_t>(LoadLinkedCandidateRejection::Count)> rejections{};
    Diagnostics diagnostics;
    LoadLinkedDiagnostics validationDiagnostics;
};
struct LoadLinkedResolvedCut { std::string id; Vector2 from{}, to{}; };
struct LoadLinkedFragmentMotion {
    std::uint16_t fragmentId = 0;
    // 高さ1・X右/Y上/Z奥の初速と度/秒。重力・duration・fadeはSettingsの値を使う。
    std::array<double, 3> initialVelocity{}, angularVelocityDegrees{};
};
struct LoadLinkedPrefix {
    std::size_t count = 0;
    Geometry geometry;
    std::vector<LoadLinkedFragmentMotion> motions;
};
class LoadLinkedGeneration;
// 全生成成功時だけ公開。設定はBegin時のdeep copy、幾何と係数は本数ごとに単一所有する。
// getter/FindPrefixのborrowは完成物のshared所有が残る間だけ有効。
class LoadLinkedGenerated final {
public:
    ~LoadLinkedGenerated();
    std::uint64_t RequestId() const noexcept;
    std::uint64_t SettingsRevision() const noexcept;
    const LoadLinkedAsset& Settings() const noexcept;
    std::uint32_t CaptureWidth() const noexcept;
    std::uint32_t CaptureHeight() const noexcept;
    std::uint32_t CutSeed() const noexcept;
    std::uint32_t MotionSeed() const noexcept;
    const std::vector<LoadLinkedResolvedCut>& Cuts() const noexcept;
    const std::vector<CutSegment>& CutSegments() const noexcept;
    const LoadLinkedPrefix* FindPrefix(std::size_t count) const noexcept;
private:
    struct Data;
    explicit LoadLinkedGenerated(std::unique_ptr<Data> data) noexcept;
    LoadLinkedGenerated(const LoadLinkedGenerated&) = delete;
    LoadLinkedGenerated& operator=(const LoadLinkedGenerated&) = delete;
    std::unique_ptr<Data> data_;
    friend class LoadLinkedGeneration;
};
// 全APIは単一実行主体で直列利用する。コピー中の設定変更や、StepとCancel等の並行呼出しは禁止。
// BeginのValidatorは同期。Stepの単位は候補1件/係数1件で、実行時間の上限は未保証。
class LoadLinkedGeneration final {
public:
    LoadLinkedGeneration();
    ~LoadLinkedGeneration();
    LoadLinkedGeneration(const LoadLinkedGeneration&) = delete;
    LoadLinkedGeneration& operator=(const LoadLinkedGeneration&) = delete;
    LoadLinkedGeneration(LoadLinkedGeneration&&) = delete;
    LoadLinkedGeneration& operator=(LoadLinkedGeneration&&) = delete;
    // 成功時だけdeep copyした要求へ交換。拒否/例外時は旧workspace・status・結果を保持する。
    // failure引数だけ今回の診断へ置換。診断割当失敗・非標準例外は伝播し得る。
    bool TryBegin(const LoadLinkedGenerationInput& input, LoadLinkedGenerationFailure& failure);
    // 失敗時は未公開値を破棄してFailed。診断割当失敗は伝播し得るが途中状態へ戻らない。
    LoadLinkedGenerationState Step();
    // 生成中/未取得Completedだけ破棄。取得先のshared_ptrと最後の計数へ干渉しない。
    void Cancel() noexcept;
    LoadLinkedGenerationStatus GetStatus() const noexcept;
    // 次の変更操作/破棄まで有効。Begin拒否では上書きしない。
    const LoadLinkedGenerationFailure& GetFailure() const noexcept;
    // Completed・未取得・ID一致でだけ一回move。false時はoutと自状態を保持する。
    bool TryTakeResult(std::uint64_t expectedRequestId, std::shared_ptr<const LoadLinkedGenerated>& out) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace SceneTransitionAsset
