// 役割: 履歴専用Object/影/環境を所有し、元世界を変更せず描画する。Sequenceは担当しない。
#pragma once
#include "SceneReplayHistorySystem.h"
#include "../rendering/SceneEnvironmentSystem.h"
#include "../rendering/SceneLightingSystem.h"
#include "../../../../engine/3d/Camera.h"
class Skybox;
class DirectXCommon;
class SrvManager;

class SceneReplayRenderSystem {
public:
    SceneReplayRenderSystem();
    ~SceneReplayRenderSystem();
    SceneReplayRenderSystem(const SceneReplayRenderSystem&) = delete;
    SceneReplayRenderSystem& operator=(const SceneReplayRenderSystem&) = delete;
    // 前frame全GPU利用完了後、最初のcommand記録前に呼ぶ。失敗時の途中資源は即破棄可能。
    bool Prepare(const SceneReplayHistorySystem&, DirectXCommon*, SrvManager*,
        const SceneLightingSystem&, const SceneEnvironmentSystem::ReplaySnapshot&);
    bool ApplyEvaluatedFrame(const SceneReplayFrame&, const Camera&);
    // 同一Applyにつき影→色を一回ずつ。Game View以外へこのCamera/bufferを流用しない。
    bool DrawShadow();
    bool DrawWorld();
    void Cancel(); // 受付を止めるだけ。提出済み資源は完了通知まで保持。
    void NotifyGpuCompleted(); // GameのPostDraw+切離しWindowの最終利用完了後だけ。
    bool ReleaseAtSafeBoundary();
    bool HasPendingGpuUse() const { return pendingGpuUse_; }
    bool IsPrepared() const { return prepared_; }
    const SceneReplayIdentity& Identity() const { return identity_; }
    const Object3d::ReplayBounds& OriginBounds() const { return originBounds_; }
    const Object3d::ReplayBounds& ClipBounds() const { return clipBounds_; }
    const SceneReplayOwnerBasis& OriginBasis() const { return originBasis_; }
    uint64_t EstimatedGpuBytes() const { return estimatedGpuBytes_; }
    uint64_t EstimatedDescriptors() const { return estimatedDescriptors_; }
private:
    struct Entry {
        uint64_t entity = 0, incarnation = 0;
        std::string modelKey;
        std::vector<std::pair<std::string, int32_t>> skeleton;
        std::unique_ptr<Object3d> object;
        bool visible = false, shadow = false;
        bool Matches(const SceneReplaySubject&) const;
    };
    struct Water {
        SceneEnvironmentSystem::ReplayWater value;
        std::unique_ptr<WaterSurfaceRenderer> renderer;
    };
    bool BuildConservativeClipBounds(const SceneReplayHistorySystem&);
    bool PrepareInternal(const SceneReplayHistorySystem&, DirectXCommon*, SrvManager*,
        const SceneLightingSystem&, const SceneEnvironmentSystem::ReplaySnapshot&);
    SceneReplayIdentity identity_;
    SceneReplayOwnerBasis originBasis_;
    Object3d::ReplayBounds originBounds_, clipBounds_;
    std::vector<Entry> entries_;
    std::vector<Object3d*> casters_;
    std::vector<Water> waters_;
    std::unique_ptr<Skybox> skybox_;
    SceneLightingSystem lighting_;
    Camera camera_;
    std::optional<float> waterTime_;
    uint64_t estimatedGpuBytes_ = 0, estimatedDescriptors_ = 0;
    bool prepared_ = false, frameReady_ = false, pendingGpuUse_ = false;
    bool shadowDrawn_ = false, worldDrawn_ = false, cancelled_ = false;
};
