// 役割: 全対象を先に用意し、全姿勢適用成功後だけReplay frameを公開する。
#include "SceneReplayRenderSystem.h"
#include "../../../../engine/3d/Model.h"
#include "../../../../engine/3d/ModelManager.h"
#include "../../../../engine/3d/Skybox.h"
#include "../../../../engine/base/DirectXCommon.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
void Merge(Object3d::ReplayBounds& target, const Object3d::ReplayBounds& source) {
    if (!source.valid) return;
    if (!target.valid) { target = source; return; }
    target.min = {(std::min)(target.min.x,source.min.x),(std::min)(target.min.y,source.min.y),(std::min)(target.min.z,source.min.z)};
    target.max = {(std::max)(target.max.x,source.max.x),(std::max)(target.max.y,source.max.y),(std::max)(target.max.z,source.max.z)};
}
bool AddBytes(uint64_t& total, uint64_t value) {
    if (value > (std::numeric_limits<uint64_t>::max)() - total) return false;
    total += value; return true;
}
}
SceneReplayRenderSystem::SceneReplayRenderSystem() = default;
SceneReplayRenderSystem::~SceneReplayRenderSystem() {
    // 所有RuntimeがFinalize前にdrainする。GPUが使用中のdescriptorを返却しない。
    assert(!pendingGpuUse_);
}
bool SceneReplayRenderSystem::Entry::Matches(const SceneReplaySubject& subject) const {
    if (!subject.pose || entity != subject.entityId || incarnation != subject.incarnation ||
        modelKey != subject.pose->modelKey || skeleton.size() != subject.pose->joints.size()) return false;
    for (size_t i=0;i<skeleton.size();++i)
        if (skeleton[i].first != subject.pose->joints[i].name || skeleton[i].second != subject.pose->joints[i].parent) return false;
    return true;
}
bool SceneReplayRenderSystem::Prepare(const SceneReplayHistorySystem& history, DirectXCommon* dx, SrvManager* srv,
    const SceneLightingSystem& lighting, const SceneEnvironmentSystem::ReplaySnapshot& environment) {
    if (pendingGpuUse_ || prepared_ || cancelled_) return false;
    bool result = false;
    try { result = PrepareInternal(history,dx,srv,lighting,environment); }
    catch (const std::bad_alloc&) {} catch (const std::length_error&) {}
    if (!result) { ReleaseAtSafeBoundary(); return false; }
    prepared_ = true;
    return true;
}
bool SceneReplayRenderSystem::PrepareInternal(const SceneReplayHistorySystem& history, DirectXCommon* dx, SrvManager* srv,
    const SceneLightingSystem& lighting, const SceneEnvironmentSystem::ReplaySnapshot& environment) {
    const auto* frames = history.SealedFrames();
    if (!frames || !dx || !srv) return false;
    identity_ = history.Identity(); originBasis_ = frames->front().ownerBasis;
    for (const auto& origin : frames->front().originSubjects) Merge(originBounds_,origin.bounds);
    uint64_t cpuBytes = sizeof(*this), maxScratch = 0;
    estimatedGpuBytes_ = lighting.EstimateReplayGpuBytes(); estimatedDescriptors_ = 1;
    for (const auto& frame : *frames) {
        if (!(frame.identity == identity_)) return false;
        for (const auto& subject : frame.subjects) {
            if (!subject.pose) return false;
            const auto& pose = *subject.pose;
            auto* model = ModelManager::GetInstance()->FindModel(pose.modelKey);
            if (!model) return false;
            const uint64_t scratch = uint64_t(model->GetVertexCount()) * sizeof(SkinCluster::VertexInfluence) +
                uint64_t(pose.joints.size()) * (sizeof(Matrix4x4) * 4 + sizeof(Object3d::ReplayJoint) * 4);
            if (!history.FitsScratch(cpuBytes) || !history.FitsScratch(scratch) ||
                scratch > (std::numeric_limits<uint64_t>::max)() - cpuBytes || !history.FitsScratch(cpuBytes+scratch)) return false;
            maxScratch=(std::max)(maxScratch,scratch);
            Object3d::ReplayBounds bounds;
            if (!Object3d::ComputeReplayBounds(pose,bounds)) return false;
            Merge(clipBounds_,bounds);
            const auto existing = std::find_if(entries_.begin(),entries_.end(),[&](const Entry& e){return e.Matches(subject);});
            if (existing != entries_.end()) continue;
            // 計数はbuffer payloadの推定。allocator/共有Model/texture/driver heapは含めない。
            uint64_t entryCpu = sizeof(Entry) + sizeof(Object3d) + pose.modelKey.capacity()+1;
            for (const auto& joint : pose.joints) {
                if (!AddBytes(entryCpu, sizeof(Object3d::ReplayJoint)*4 + joint.name.capacity()*4+4)) return false;
            }
            for (const auto& material : pose.materials)
                if (!AddBytes(entryCpu, sizeof(Object3d::ReplayMaterial)*4 + material.textureKey.capacity()*4+4)) return false;
            if (entryCpu > (std::numeric_limits<uint64_t>::max)()/2 || !AddBytes(cpuBytes,entryCpu*2) ||
                scratch > (std::numeric_limits<uint64_t>::max)()-cpuBytes || !history.FitsScratch(cpuBytes+scratch)) return false;
            uint64_t gpu = 512 + uint64_t(pose.materials.size())*256;
            if (model->HasSkinning()) {
                if (!AddBytes(gpu, uint64_t(model->GetVertexCount())*(sizeof(Model::VertexData)+sizeof(SkinCluster::VertexInfluence))) ||
                    !AddBytes(gpu, uint64_t(pose.joints.size())*sizeof(SkinCluster::PaletteWell)+256) ||
                    !AddBytes(estimatedDescriptors_,4)) return false;
            }
            if (!AddBytes(estimatedGpuBytes_,gpu)) return false;
            Entry entry; entry.entity=subject.entityId; entry.incarnation=subject.incarnation; entry.modelKey=pose.modelKey;
            for (const auto& joint : pose.joints) entry.skeleton.emplace_back(joint.name,joint.parent);
            entry.object=std::make_unique<Object3d>();
            if (!entry.object->TryInitializeReplay(Object3dCommon::GetInstance(),pose)) return false;
            entries_.push_back(std::move(entry));
        }
    }
    if (!lighting_.TryInitializeReplay(dx,srv,lighting)) return false;
    if (!environment.skyboxKey.empty()) {
        skybox_=std::make_unique<Skybox>();
        if (!skybox_->TryInitializeReplay(Object3dCommon::GetInstance(),environment.skyboxKey)) return false;
        skybox_->SetColor(environment.skyboxColor);
        if (!AddBytes(estimatedGpuBytes_,256)) return false;
    }
    for (const auto& value : environment.waters) {
        if (!AddBytes(cpuBytes,sizeof(Water)*2+uint64_t(72)*72*6*20) || !history.FitsScratch(cpuBytes)) return false;
        Water water; water.value=value; water.renderer=std::make_unique<WaterSurfaceRenderer>();
        if (!water.renderer->TryInitializeReplay(dx)) return false;
        waters_.push_back(std::move(water));
        if (!AddBytes(estimatedGpuBytes_,uint64_t(72)*72*6*20+256)) return false;
    }
    casters_.reserve(entries_.size());
    if (maxScratch > (std::numeric_limits<uint64_t>::max)()-cpuBytes || !history.FitsScratch(cpuBytes+maxScratch)) return false;
    return BuildConservativeClipBounds(history);
}
bool SceneReplayRenderSystem::ApplyEvaluatedFrame(const SceneReplayFrame& frame, const Camera& camera) {
    if (!prepared_ || cancelled_ || pendingGpuUse_ || !(frame.identity==identity_)) return false;
    frameReady_=false; shadowDrawn_=worldDrawn_=false;
    try {
        // 集合不一致は一体も適用する前に拒否。途中Apply失敗でもframeReadyは公開しない。
        for (const auto& subject : frame.subjects)
            if (std::none_of(entries_.begin(),entries_.end(),[&](const Entry& e){return e.Matches(subject);})) return false;
        camera_=camera;
        for (auto& entry : entries_) { entry.visible=false; entry.shadow=false; }
        casters_.clear();
        for (const auto& subject : frame.subjects) {
            auto found=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& e){return e.Matches(subject);});
            if (!found->object->ApplyReplayPose(*subject.pose)) return false;
            found->object->UpdateForCamera(&camera_);
            found->visible=subject.visible; found->shadow=subject.castsShadow;
            if (found->shadow) casters_.push_back(found->object.get());
        }
        if (skybox_) { skybox_->SetCamera(&camera_); skybox_->Update(); }
        waterTime_=frame.waterTime;
        frameReady_=true;
        return true;
    } catch (const std::bad_alloc&) { return false; } catch (const std::length_error&) { return false; }
}
bool SceneReplayRenderSystem::DrawShadow() {
    if (!frameReady_ || cancelled_ || shadowDrawn_) return false;
    pendingGpuUse_=shadowDrawn_=true;
    lighting_.RenderShadows(casters_);
    return true;
}
bool SceneReplayRenderSystem::DrawWorld() {
    if (!frameReady_ || cancelled_ || !shadowDrawn_ || worldDrawn_) return false;
    pendingGpuUse_=worldDrawn_=true;
    if (skybox_) skybox_->Draw();
    Object3dCommon::GetInstance()->SetCommonRenderState();
    lighting_.Bind();
    for (auto& entry : entries_) if (entry.visible) entry.object->Draw();
    if (waterTime_) for (auto& water : waters_)
        water.renderer->DrawAtTime(&camera_,water.value.center,water.value.halfSize,water.value.settings,*waterTime_);
    return true;
}
void SceneReplayRenderSystem::Cancel() { cancelled_=true; frameReady_=false; }
void SceneReplayRenderSystem::NotifyGpuCompleted() {
    pendingGpuUse_=false;
    if (cancelled_) ReleaseAtSafeBoundary();
}
bool SceneReplayRenderSystem::ReleaseAtSafeBoundary() {
    if (pendingGpuUse_) return false;
    std::vector<Entry>().swap(entries_);
    std::vector<Object3d*>().swap(casters_);
    std::vector<Water>().swap(waters_);
    skybox_.reset(); lighting_.Finalize();
    identity_={}; originBasis_={}; originBounds_={}; clipBounds_={}; waterTime_.reset();
    estimatedGpuBytes_=estimatedDescriptors_=0;
    prepared_=frameReady_=shadowDrawn_=worldDrawn_=cancelled_=false;
    return true;
}

bool SceneReplayRenderSystem::BuildConservativeClipBounds(const SceneReplayHistorySystem& history) {
    const auto* frames=history.SealedFrames();
    if (!frames) return false;
    auto length=[](const Vector3& v) { return std::hypot(double(v.x),double(v.y),double(v.z)); };
    auto linearNorm=[](const Matrix4x4& m) {
        double sum=0; for(int r=0;r<3;++r) for(int c=0;c<3;++c) sum+=double(m.m[r][c])*m.m[r][c];
        return std::sqrt(sum); // Frobenius normは非一様scale/shearでも上界。
    };
    Object3d::ReplayBounds result;
    for (const auto& entry : entries_) {
        const auto* model=ModelManager::GetInstance()->FindModel(entry.modelKey);
        if (!model) return false;
        double vertexRadius=0;
        for (const auto& v : model->GetVertices()) vertexRadius=(std::max)(vertexRadius,length({v.position.x,v.position.y,v.position.z}));
        double bindRadius=vertexRadius;
        for (const auto& pair : model->GetSkinClusterData()) {
            const auto& m=pair.second.inverseBindPoseMatrix;
            bindRadius=(std::max)(bindRadius,linearNorm(m)*vertexRadius+length({m.m[3][0],m.m[3][1],m.m[3][2]}));
        }
        double weightFactor=1;
        if(model->HasSkinning()) {
            auto skeleton=CreateSkeleton(model->GetRootNode());
            std::vector<SkinCluster::VertexInfluence> influences(model->GetVertexCount());
            std::vector<Matrix4x4> inverseBind;
            SkinCluster::BuildInfluences(skeleton,*model,influences,inverseBind);
            for(const auto& influence : influences) {
                double absoluteSum=0;
                for(float weight : influence.weights) absoluteSum+=std::abs(double(weight));
                if(!std::isfinite(absoluteSum)) return false;
                weightFactor=(std::max)(weightFactor,absoluteSum);
            }
        }
        std::vector<double> scales(entry.skeleton.size()), offsets(entry.skeleton.size());
        double worldScale=0;
        Object3d::ReplayBounds centers;
        for (const auto& frame : *frames) for (const auto& subject : frame.subjects) if (entry.Matches(subject)) {
            const auto& pose=*subject.pose;
            worldScale=(std::max)(worldScale,linearNorm(pose.renderWorld));
            Object3d::ReplayBounds center; center.valid=true;
            center.min=center.max={pose.renderWorld.m[3][0],pose.renderWorld.m[3][1],pose.renderWorld.m[3][2]};
            Merge(centers,center);
            for(size_t j=0;j<scales.size();++j) {
                const auto& transform=pose.joints[j].transform;
                scales[j]=(std::max)(scales[j],(std::max)({std::abs(double(transform.scale.x)),std::abs(double(transform.scale.y)),std::abs(double(transform.scale.z))}));
                offsets[j]=(std::max)(offsets[j],length(transform.translate));
            }
        }
        double poseRadius=vertexRadius;
        if(model->HasSkinning()) for(size_t j=0;j<scales.size();++j) {
            const int parent=entry.skeleton[j].second;
            if(parent>=0) {
                if(static_cast<size_t>(parent)>=j) return false;
                offsets[j]=offsets[parent]+scales[parent]*offsets[j];
                scales[j]*=scales[parent];
            }
            poseRadius=(std::max)(poseRadius,offsets[j]+scales[j]*bindRadius);
        }
        // 補間中の回転も球の中に入る。sample AABBの和だけでは回転の途中を覆えない。
        const double radius=poseRadius*worldScale*weightFactor;
        if(!centers.valid || !std::isfinite(radius) || radius>(std::numeric_limits<float>::max)()/4) return false;
        const float r=static_cast<float>(radius);
        centers.min={centers.min.x-r,centers.min.y-r,centers.min.z-r};
        centers.max={centers.max.x+r,centers.max.y+r,centers.max.z+r};
        if (!std::isfinite(centers.min.x) || !std::isfinite(centers.min.y) || !std::isfinite(centers.min.z) ||
            !std::isfinite(centers.max.x) || !std::isfinite(centers.max.y) || !std::isfinite(centers.max.z)) return false;
        Merge(result,centers);
    }
    // WaterSurfaceのGerstner式の最大変位も含める。Skyboxはfullscreenのためclip対象外。
    for (const auto& water : waters_) {
        const auto& v=water.value;
        const float fit=(std::clamp)((std::max)(v.halfSize.x,v.halfSize.z)/8.0f,0.45f,1.6f);
        const float lateral=0.52f*(5.8f+3.1f+1.65f)*(std::max)(fit,0.65f)/(6.0f*3.1415926535f);
        const float height=(0.12f+0.075f+0.045f)*std::abs(v.settings.waveScale)*fit+0.0003f;
        Object3d::ReplayBounds bounds;
        bounds.valid=true;
        bounds.min={v.center.x-v.halfSize.x-lateral,v.center.y+v.halfSize.y-height,v.center.z-v.halfSize.z-lateral};
        bounds.max={v.center.x+v.halfSize.x+lateral,v.center.y+v.halfSize.y+height,v.center.z+v.halfSize.z+lateral};
        if (!std::isfinite(bounds.min.x) || !std::isfinite(bounds.min.y) || !std::isfinite(bounds.min.z) ||
            !std::isfinite(bounds.max.x) || !std::isfinite(bounds.max.y) || !std::isfinite(bounds.max.z)) return false;
        Merge(result,bounds);
    }
    clipBounds_=result;
    return true;
}
