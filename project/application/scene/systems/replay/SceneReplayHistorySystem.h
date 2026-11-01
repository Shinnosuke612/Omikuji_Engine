// 役割: 描画直前のCPU値を容量制限付きで所有する。時計/Scene/GPUを操作しない。
#pragma once
#include "../../../../engine/3d/Object3d.h"
#include "../../../../engine/scene/SceneReplayHistory.h"
#include "../../../../engine/scene/SceneSettings.h"
#include <list>
#include <memory>
#include <optional>
#include <utility>

struct SceneReplayIdentity {
    uint64_t instance = 0, generation = 0, owner = 0, deathSerial = 0;
    bool operator==(const SceneReplayIdentity&) const = default;
};
struct SceneReplaySubject {
    uint64_t entityId = 0, incarnation = 0;
    bool visible = true, castsShadow = true;
    std::shared_ptr<const Object3d::ReplayPose> pose;
};
struct SceneReplayOwnerBasis {
    Matrix4x4 world{}, cameraWorld{}, cameraProjection{};
};
struct SceneReplayOriginSubject { uint64_t entityId = 0; Object3d::ReplayBounds bounds; };
struct SceneReplayFrame {
    SceneReplayIdentity identity;
    double realSeconds = 0, worldElapsedSeconds = 0;
    float gameViewEffectTime = 0;
    std::optional<float> waterTime;
    ScenePostProcessSettings effectivePostProcess;
    SceneReplayOwnerBasis ownerBasis;
    // 最初のsampleだけが所有。同時刻の置換でも開始時の対象集合を維持する。
    std::vector<SceneReplayOriginSubject> originSubjects;
    std::vector<SceneReplaySubject> subjects;
};
class SceneReplayHistorySystem {
public:
    // Idleから一試行だけ開始。有限0<Hz<=60/正のMiBとbyte換算を検証する。失敗はFailed。
    bool Start(SceneReplayIdentity identity, float captureHz, uint64_t budgetMiB);
    // 巨大Poseを作る前に呼ぶ。scratch上限と始点/最新点の余地を予約し、中間点だけ間引く。
    bool PrepareCapture(uint64_t scratchBytes);
    // PrepareCapture成功後だけ。identity/非減少時刻を検証し、Sealは末尾採取と確定を一体化する。
    bool Append(SceneReplayFrame frame);
    bool Seal(SceneReplayFrame finalFrame);
    void Fail(const char* diagnostic);
    void Clear(); // 全CPU所有を解放。以前FindBracketで返した参照を無効にする。
    const SceneReplayHistoryStatus& Status() const { return status_; }
    const SceneReplayIdentity& Identity() const { return identity_; }
    // 直前sampleとの厳密な値比較だけで共有。追加intern表を持たず寿命と容量を有界にする。
    std::shared_ptr<const Object3d::ReplayPose> SharePose(uint64_t entity, uint64_t incarnation,
        Object3d::ReplayPose pose) const;
    // Sealedだけ。参照はClear/破棄まで有効。再生側はframeを跨いで生参照を保持しない。
    std::pair<const SceneReplayFrame*, const SceneReplayFrame*> FindBracket(double seconds) const;
    // Clearまでの同一呼出内だけ借用。再生側はこの参照を保存しない。
    const std::list<SceneReplayFrame>* SealedFrames() const;
    // 保持履歴+scratchの予算を事前確認。失敗はout不変、immutableな姿勢を返す。
    bool Evaluate(double seconds, SceneReplayFrame& out) const;
    bool FitsScratch(uint64_t bytes) const;
private:
    bool Commit(SceneReplayFrame frame, bool seal);
    uint64_t CountBytes() const;
    bool PruneFor(uint64_t additionalBytes);
    SceneReplayIdentity identity_;
    SceneReplayHistoryStatus status_;
    uint64_t budget_ = 0, scratchReservation_ = 0;
    std::list<SceneReplayFrame> frames_;
};
