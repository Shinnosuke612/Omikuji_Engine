// 役割: 履歴の原子的な追記、端点保持、CPU容量計数を実装する。
#include "SceneReplayHistorySystem.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <iterator>
#include <stdexcept>

namespace {
constexpr uint64_t overflow = (std::numeric_limits<uint64_t>::max)();
uint64_t Add(uint64_t a, uint64_t b) { return b > overflow - a ? overflow : a + b; }
uint64_t MultiplyBytes(uint64_t a, uint64_t b) { return a && b > overflow / a ? overflow : a * b; }
bool Equal(const Vector3& a, const Vector3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
bool Equal(const Vector4& a, const Vector4& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
bool Equal(const Matrix4x4& a, const Matrix4x4& b) {
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) if (a.m[r][c] != b.m[r][c]) return false;
    return true;
}
bool Finite(const Matrix4x4& value) {
    for (const auto& row : value.m) for (float x : row) if (!std::isfinite(x)) return false;
    return true;
}
bool Equal(const Object3d::ReplayPose& a, const Object3d::ReplayPose& b) {
    if (a.modelKey != b.modelKey || a.environmentKey != b.environmentKey || !Equal(a.renderWorld, b.renderWorld) ||
        a.environmentCoefficient != b.environmentCoefficient || a.cullMode != b.cullMode ||
        a.joints.size() != b.joints.size() || a.materials.size() != b.materials.size()) return false;
    for (size_t i = 0; i < a.joints.size(); ++i) {
        const auto& x = a.joints[i]; const auto& y = b.joints[i];
        if (x.name != y.name || x.parent != y.parent || !Equal(x.transform.scale, y.transform.scale) ||
            !Equal(x.transform.translate, y.transform.translate) || x.transform.rotate.x != y.transform.rotate.x ||
            x.transform.rotate.y != y.transform.rotate.y || x.transform.rotate.z != y.transform.rotate.z ||
            x.transform.rotate.w != y.transform.rotate.w) return false;
    }
    for (size_t i = 0; i < a.materials.size(); ++i) {
        const auto& x = a.materials[i]; const auto& y = b.materials[i];
        if (!Equal(x.color, y.color) || !Equal(x.emissiveColor, y.emissiveColor) || !Equal(x.uvTransform, y.uvTransform) ||
            x.enableLighting != y.enableLighting || x.emissiveIntensity != y.emissiveIntensity || x.shininess != y.shininess ||
            x.dissolveAmount != y.dissolveAmount || x.dissolveEdgeWidth != y.dissolveEdgeWidth ||
            x.dissolveNoiseScale != y.dissolveNoiseScale || x.textureKey != y.textureKey) return false;
    }
    return true;
}
uint64_t PoseBytes(const Object3d::ReplayPose& p) {
    // shared_ptr制御blockの余地も計上。allocator管理領域を含むprocess上限ではない。
    uint64_t n = sizeof(p) + 64;
    n = Add(n, Add(Add(p.modelKey.capacity(), p.environmentKey.capacity()), 2));
    n = Add(n, MultiplyBytes(p.joints.capacity(), sizeof(Object3d::ReplayJoint)));
    n = Add(n, MultiplyBytes(p.materials.capacity(), sizeof(Object3d::ReplayMaterial)));
    for (const auto& j : p.joints) n = Add(n, Add(j.name.capacity(), 1));
    for (const auto& m : p.materials) n = Add(n, Add(m.textureKey.capacity(), 1));
    return n;
}
}

bool SceneReplayHistorySystem::Start(SceneReplayIdentity identity, float hz, uint64_t mib) {
    if (status_.phase != SceneReplayHistoryPhase::Idle) return false;
    identity_ = identity; status_.owner = identity.owner;
    if (!identity.instance || !identity.generation || !identity.owner || !identity.deathSerial ||
        !std::isfinite(hz) || hz <= 0 || hz > 60 || !mib || mib > overflow / (1024 * 1024)) {
        Fail("DeathResponse:HistorySettings"); return false;
    }
    budget_ = mib * 1024 * 1024;
    status_.phase = SceneReplayHistoryPhase::Recording;
    return true;
}
void SceneReplayHistorySystem::Fail(const char* issue) {
    status_.phase = SceneReplayHistoryPhase::Failed;
    status_.diagnostic = issue;
    scratchReservation_ = 0;
    status_.countedBytes = CountBytes();
}
void SceneReplayHistorySystem::Clear() {
    frames_.clear(); identity_ = {}; status_ = {}; budget_ = scratchReservation_ = 0;
}
uint64_t SceneReplayHistorySystem::CountBytes() const {
    uint64_t total = Add(sizeof(*this), status_.diagnostic.capacity() + 1);
    for (auto f = frames_.begin(); f != frames_.end(); ++f) {
        total = Add(total, sizeof(SceneReplayFrame) + 2 * sizeof(void*));
        total = Add(total, MultiplyBytes(f->subjects.capacity(), sizeof(SceneReplaySubject)));
        total = Add(total, MultiplyBytes(f->originSubjects.capacity(), sizeof(SceneReplayOriginSubject)));
        for (size_t i = 0; i < f->subjects.size(); ++i) {
            const auto* pose = f->subjects[i].pose.get();
            bool seen = false;
            // subjectsはID順。同じID/incarnationだけが共有候補なので、巨大な一時hash表は不要。
            for (auto prev = frames_.begin(); prev != f && !seen; ++prev) {
                const auto found = std::lower_bound(prev->subjects.begin(), prev->subjects.end(), f->subjects[i].entityId,
                    [](const auto& subject, uint64_t id) { return subject.entityId < id; });
                seen = found != prev->subjects.end() && found->pose.get() == pose;
            }
            if (pose && !seen) total = Add(total, PoseBytes(*pose));
        }
    }
    return total;
}
bool SceneReplayHistorySystem::PruneFor(uint64_t additional) {
    while (Add(CountBytes(), additional) > budget_ && frames_.size() > 2) {
        // 始点と現在の末尾は固定。古い中間点から捨てるので結果は決定的。
        frames_.erase(std::next(frames_.begin())); ++status_.thinningCount;
    }
    status_.countedBytes = CountBytes(); status_.sampleCount = frames_.size();
    return Add(status_.countedBytes, additional) <= budget_;
}
bool SceneReplayHistorySystem::PrepareCapture(uint64_t scratch) {
    if (status_.phase != SceneReplayHistoryPhase::Recording) return false;
    // 初回も始点+最新+次scratchを収容できることを先に確認する。
    if (Add(sizeof(*this), MultiplyBytes(scratch, 3)) > budget_ || !PruneFor(scratch)) {
        Fail("DeathResponse:HistoryBudget"); return false;
    }
    scratchReservation_ = scratch;
    return true;
}
std::shared_ptr<const Object3d::ReplayPose> SceneReplayHistorySystem::SharePose(uint64_t entity, uint64_t incarnation,
    Object3d::ReplayPose pose) const {
    if (!frames_.empty()) {
        const auto& subjects = frames_.back().subjects;
        const auto found = std::lower_bound(subjects.begin(), subjects.end(), entity,
            [](const auto& subject, uint64_t id) { return subject.entityId < id; });
        if (found != subjects.end() && found->entityId == entity && found->incarnation == incarnation &&
            found->pose && Equal(*found->pose, pose)) return found->pose;
    }
    return std::make_shared<const Object3d::ReplayPose>(std::move(pose));
}
bool SceneReplayHistorySystem::Append(SceneReplayFrame frame) { return Commit(std::move(frame), false); }
bool SceneReplayHistorySystem::Seal(SceneReplayFrame frame) { return Commit(std::move(frame), true); }
bool SceneReplayHistorySystem::Commit(SceneReplayFrame frame, bool seal) {
    if (status_.phase != SceneReplayHistoryPhase::Recording || !scratchReservation_) return false;
    if (!(frame.identity == identity_) || !std::isfinite(frame.realSeconds) || frame.realSeconds < 0 ||
        !std::isfinite(frame.worldElapsedSeconds) || frame.worldElapsedSeconds < 0 || !std::isfinite(frame.gameViewEffectTime) ||
        (frame.waterTime && !std::isfinite(*frame.waterTime)) ||
        (frames_.empty() ? frame.realSeconds != 0 : (frame.realSeconds < frames_.back().realSeconds || frame.worldElapsedSeconds < frames_.back().worldElapsedSeconds))) {
        Fail("DeathResponse:HistoryIdentity"); return false;
    }
    if (!Finite(frame.ownerBasis.world) || !Finite(frame.ownerBasis.cameraWorld) || !Finite(frame.ownerBasis.cameraProjection)) {
        Fail("DeathResponse:HistoryPose"); return false;
    }
    std::sort(frame.subjects.begin(), frame.subjects.end(), [](const auto& a, const auto& b) { return a.entityId < b.entityId; });
    uint64_t previousId = 0;
    uint64_t actual = Add(sizeof(SceneReplayFrame) + 2 * sizeof(void*), MultiplyBytes(frame.subjects.capacity(), sizeof(SceneReplaySubject)));
    actual = Add(actual, MultiplyBytes(frame.originSubjects.capacity(), sizeof(SceneReplayOriginSubject)));
    for (const auto& s : frame.subjects) {
        if (!s.pose || !s.entityId || !s.incarnation || s.entityId == previousId) { Fail("DeathResponse:HistoryPose"); return false; }
        previousId = s.entityId;
        actual = Add(actual, PoseBytes(*s.pose));
    }
    if (actual > scratchReservation_) { Fail("DeathResponse:HistoryBudget"); return false; }
    try {
        // node確保成功までは既存末尾を破棄しない。同時刻は新しい末尾へ置換する。
        frames_.push_back(std::move(frame));
        if (frames_.size() > 1) {
            auto previous = std::prev(frames_.end(), 2);
            if (previous->realSeconds == frames_.back().realSeconds) {
                if (previous == frames_.begin()) {
                    frames_.back().originSubjects=std::move(previous->originSubjects);
                    frames_.back().ownerBasis=previous->ownerBasis;
                }
                frames_.erase(previous);
            }
        }
    } catch (const std::bad_alloc&) { Fail("DeathResponse:HistoryAllocation"); return false; }
    scratchReservation_ = 0;
    status_.sampleCount = frames_.size(); status_.realSeconds = frames_.back().realSeconds;
    status_.countedBytes = CountBytes();
    if (status_.countedBytes > budget_) { Fail("DeathResponse:HistoryBudget"); return false; }
    if (seal) status_.phase = SceneReplayHistoryPhase::Sealed;
    return true;
}
std::pair<const SceneReplayFrame*, const SceneReplayFrame*> SceneReplayHistorySystem::FindBracket(double t) const {
    if (status_.phase != SceneReplayHistoryPhase::Sealed || frames_.empty() || !std::isfinite(t)) return {};
    const auto* before = &frames_.front();
    for (const auto& f : frames_) { if (f.realSeconds >= t) return {before, &f}; before = &f; }
    return {before, before};
}

const std::list<SceneReplayFrame>* SceneReplayHistorySystem::SealedFrames() const {
    return status_.phase == SceneReplayHistoryPhase::Sealed && !frames_.empty() ? &frames_ : nullptr;
}
bool SceneReplayHistorySystem::FitsScratch(uint64_t bytes) const {
    return bytes != overflow && Add(CountBytes(), bytes) <= budget_;
}
namespace {
bool Compatible(const Object3d::ReplayPose& a, const Object3d::ReplayPose& b) {
    if (a.modelKey != b.modelKey || a.joints.size() != b.joints.size()) return false;
    for (size_t i = 0; i < a.joints.size(); ++i)
        if (a.joints[i].name != b.joints[i].name || a.joints[i].parent != b.joints[i].parent) return false;
    return true;
}
Vector3 Mix(const Vector3& a, const Vector3& b, float t) {
    return {static_cast<float>(double(a.x)+(double(b.x)-a.x)*t), static_cast<float>(double(a.y)+(double(b.y)-a.y)*t), static_cast<float>(double(a.z)+(double(b.z)-a.z)*t)};
}
bool StableTRS(const Matrix4x4& m, QuaternionTransform& out) {
    if (!DecomposeAffineMatrix(m, out.scale, out.rotate, out.translate) ||
        out.scale.x <= 0.000001f || out.scale.y <= 0.000001f || out.scale.z <= 0.000001f) return false;
    const auto rebuilt = MakeAffineMatrix(out.scale, out.rotate, out.translate);
    for (int r=0;r<4;++r) for(int c=0;c<4;++c)
        if (!std::isfinite(rebuilt.m[r][c]) || std::abs(rebuilt.m[r][c]-m.m[r][c]) >
            0.00001f * (std::max)(1.0f, std::abs(m.m[r][c]))) return false;
    return true;
}
}
bool SceneReplayHistorySystem::Evaluate(double seconds, SceneReplayFrame& out) const {
    const auto [left, right] = FindBracket(seconds);
    if (!left || !right) return false;
    // copyと補間poseの同時存在まで先に数える。確保後のcapacity差も再確認する。
    uint64_t scratch = Add(sizeof(SceneReplayFrame) * 2, MultiplyBytes(left->subjects.size() + right->subjects.size(), sizeof(SceneReplaySubject) * 2));
    for (const auto& subject : left->subjects) scratch = Add(scratch, MultiplyBytes(PoseBytes(*subject.pose), 3));
    for (const auto& subject : right->subjects) scratch = Add(scratch, PoseBytes(*subject.pose));
    scratch=Add(scratch,MultiplyBytes(frames_.front().originSubjects.capacity(),sizeof(SceneReplayOriginSubject)*2));
    scratch=Add(scratch,MultiplyBytes(out.subjects.capacity(),sizeof(SceneReplaySubject)));
    scratch=Add(scratch,MultiplyBytes(out.originSubjects.capacity(),sizeof(SceneReplayOriginSubject)));
    for(const auto& subject : out.subjects) if(subject.pose) scratch=Add(scratch,PoseBytes(*subject.pose));
    if (!FitsScratch(scratch)) return false;
    try {
        const bool atRight = seconds >= right->realSeconds;
        SceneReplayFrame result = atRight ? *right : *left;
        if (!atRight && right->realSeconds > left->realSeconds && seconds > left->realSeconds) {
            const float t = static_cast<float>((seconds-left->realSeconds)/(right->realSeconds-left->realSeconds));
            result.realSeconds = seconds;
            result.worldElapsedSeconds = left->worldElapsedSeconds+(right->worldElapsedSeconds-left->worldElapsedSeconds)*t;
            result.gameViewEffectTime = left->gameViewEffectTime+(right->gameViewEffectTime-left->gameViewEffectTime)*t;
            if (left->waterTime && right->waterTime) result.waterTime = *left->waterTime+(*right->waterTime-*left->waterTime)*t;
            for (auto& subject : result.subjects) {
                const auto found = std::lower_bound(right->subjects.begin(), right->subjects.end(), subject.entityId,
                    [](const auto& s, uint64_t id){ return s.entityId < id; });
                if (found == right->subjects.end() || found->entityId != subject.entityId ||
                    found->incarnation != subject.incarnation || !Compatible(*subject.pose, *found->pose)) continue;
                auto pose = std::make_shared<Object3d::ReplayPose>(*subject.pose);
                QuaternionTransform a{}, b{};
                if (StableTRS(pose->renderWorld,a) && StableTRS(found->pose->renderWorld,b))
                    pose->renderWorld = MakeAffineMatrix(Mix(a.scale,b.scale,t), Slerp(a.rotate,b.rotate,t), Mix(a.translate,b.translate,t));
                for (size_t j=0;j<pose->joints.size();++j) {
                    auto& value = pose->joints[j].transform; const auto& next = found->pose->joints[j].transform;
                    value.scale=Mix(value.scale,next.scale,t); value.translate=Mix(value.translate,next.translate,t);
                    value.rotate=Slerp(value.rotate,next.rotate,t);
                }
                if(!Finite(pose->renderWorld)) return false;
                for(const auto& joint : pose->joints) {
                    const auto& v=joint.transform;
                    if(!std::isfinite(v.scale.x) || !std::isfinite(v.scale.y) || !std::isfinite(v.scale.z) ||
                        !std::isfinite(v.translate.x) || !std::isfinite(v.translate.y) || !std::isfinite(v.translate.z) ||
                        !std::isfinite(v.rotate.x) || !std::isfinite(v.rotate.y) || !std::isfinite(v.rotate.z) || !std::isfinite(v.rotate.w)) return false;
                }
                subject.pose=std::move(pose);
            }
        }
        if(!std::isfinite(result.worldElapsedSeconds) || !std::isfinite(result.gameViewEffectTime) ||
            (result.waterTime && !std::isfinite(*result.waterTime))) return false;
        uint64_t actual = Add(sizeof(result), MultiplyBytes(result.subjects.capacity(), sizeof(SceneReplaySubject)));
        actual=Add(actual,MultiplyBytes(result.originSubjects.capacity(),sizeof(SceneReplayOriginSubject)));
        for (const auto& subject : result.subjects) actual=Add(actual, PoseBytes(*subject.pose));
        if (!FitsScratch(Add(actual,scratch))) return false;
        out = std::move(result);
        return true;
    } catch (const std::bad_alloc&) { return false; } catch (const std::length_error&) { return false; }
}
