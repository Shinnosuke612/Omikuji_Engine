// 役割: 凍結した乱数系列で切断候補と散乱係数を生成し、全成功時だけCPU値を一括公開する。
#include "LoadLinkedGeneration.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace SceneTransitionAsset {
namespace {
constexpr double Pi = 3.141592653589793238462643383279502884;
constexpr std::uint32_t AttemptsPerCut = 256;
using State = LoadLinkedGenerationState;
using FailureKind = LoadLinkedGenerationFailureKind;
using Rejection = LoadLinkedCandidateRejection;
using D3 = std::array<double, 3>;

// 診断文字列の確保より先にStepを終端できる、割当不要の内部失敗情報。
struct GenerationIssue { FailureKind kind; const char* field; const char* detail; };
void Check(bool condition, const char* field, const char* detail,
    FailureKind kind = FailureKind::Internal) {
    if (!condition) throw GenerationIssue{kind, field, detail};
}
std::uint64_t Next64(std::uint64_t& state) noexcept {
    state += UINT64_C(0x9E3779B97F4A7C15);
    auto value = state;
    value = (value ^ (value >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31);
}
double U01(std::uint64_t& state) noexcept {
    return static_cast<double>(Next64(state) >> 40) / 16777216;
}
double Norm(const D3& v) noexcept { return std::hypot(v[0], v[1], v[2]); }
D3 Normalize(const D3& value) {
    const double length = Norm(value);
    Check(std::isfinite(length) && length > 0, "generation.motion.direction", "方向の正規化に失敗しました", FailureKind::Motion);
    return {value[0] / length, value[1] / length, value[2] / length};
}
D3 Cross(const D3& a, const D3& b) noexcept {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
bool SameSegment(const CutSegment& a, const CutSegment& b) noexcept {
    return a.from.x == b.from.x && a.from.y == b.from.y && a.to.x == b.to.x && a.to.y == b.to.y;
}
struct CandidateLine { double nx = 0, ny = 0, rho = 0; };
CandidateLine LineFromCut(const Cut& cut, double ratio) {
    const double x = ratio * (static_cast<double>(cut.from.x) - 0.5);
    const double y = static_cast<double>(cut.from.y) - 0.5;
    const double dx = ratio * (static_cast<double>(cut.to.x) - cut.from.x);
    const double dy = static_cast<double>(cut.to.y) - cut.from.y;
    const double length = std::hypot(dx, dy);
    return {-dy / length, dx / length, (-dy / length) * x + (dx / length) * y};
}
bool ClipAxis(double point, double direction, double low, double high, double& minimum, double& maximum) noexcept {
    if (direction == 0) return point >= low && point <= high;
    const double a = (low - point) / direction, b = (high - point) / direction;
    minimum = (std::max)(minimum, (std::min)(a, b));
    maximum = (std::min)(maximum, (std::max)(a, b));
    return minimum < maximum;
}
std::optional<Rejection> RandomCandidate(const LoadLinkedRandomSource& settings, double ratio,
    double angleSample, double xSample, double ySample, const std::vector<CandidateLine>& accepted,
    Cut& cut, CandidateLine& line) {
    const double angle = std::fmod(static_cast<double>(settings.angleStartDegrees) +
        settings.angleSpanDegrees * angleSample, 360.0);
    double dx = 0, dy = 0;
    if (angle == 0) dx = 1;
    else if (angle == 90) dy = 1;
    else if (angle == 180) dx = -1;
    else if (angle == 270) dy = -1;
    else { dx = std::cos(angle * Pi / 180); dy = std::sin(angle * Pi / 180); }
    const double u = settings.pointMin.x + (static_cast<double>(settings.pointMax.x) - settings.pointMin.x) * xSample;
    const double v = settings.pointMin.y + (static_cast<double>(settings.pointMax.y) - settings.pointMin.y) * ySample;
    const double x = ratio * (u - 0.5), y = v - 0.5;
    double minimum = -std::numeric_limits<double>::infinity(), maximum = std::numeric_limits<double>::infinity();
    if (!ClipAxis(x, dx, -ratio / 2, ratio / 2, minimum, maximum) ||
        !ClipAxis(y, dy, -0.5, 0.5, minimum, maximum) ||
        !std::isfinite(minimum) || !std::isfinite(maximum) || minimum >= maximum) return Rejection::Clip;
    cut.from = {static_cast<float>((x + minimum * dx) / ratio + 0.5), static_cast<float>(y + minimum * dy + 0.5)};
    cut.to = {static_cast<float>((x + maximum * dx) / ratio + 0.5), static_cast<float>(y + maximum * dy + 0.5)};
    const auto valid = [](Vector2 point) {
        return std::isfinite(point.x) && std::isfinite(point.y) && point.x >= 0 && point.x <= 1 && point.y >= 0 && point.y <= 1;
    };
    if (!valid(cut.from) || !valid(cut.to) ||
        (cut.from.x == cut.to.x && cut.from.y == cut.to.y)) return Rejection::FloatCoordinates;
    const double length = std::hypot(ratio * (static_cast<double>(cut.to.x) - cut.from.x),
        static_cast<double>(cut.to.y) - cut.from.y);
    if (length <= 1e-6) return Rejection::TooShort;
    // 近接判定は、Buildへ渡すfloat端点から復元した同じ線を使う。
    line = LineFromCut(cut, ratio);
    for (const auto& previous : accepted) {
        const double dot = line.nx * previous.nx + line.ny * previous.ny;
        const double sign = dot < 0 ? -1 : 1;
        const double delta = std::acos(std::clamp(std::abs(dot), 0.0, 1.0)) * 180 / Pi;
        if (settings.minimumAngleSeparationDegrees > 0 && delta < settings.minimumAngleSeparationDegrees)
            return Rejection::AngleSeparation;
        const double distance = std::abs(line.rho - sign * previous.rho);
        if (settings.minimumCutSeparation > 0 && delta <= 5 && distance < settings.minimumCutSeparation)
            return Rejection::CutSeparation;
    }
    return std::nullopt;
}
void HashByte(std::uint64_t& hash, unsigned char byte) noexcept {
    hash = (hash ^ byte) * UINT64_C(1099511628211);
}
void HashLittleEndian(std::uint64_t& hash, std::uint32_t value, unsigned bytes) noexcept {
    for (unsigned i = 0; i < bytes; ++i) HashByte(hash, static_cast<unsigned char>(value >> (8 * i)));
}
std::uint64_t MotionState(std::uint32_t seed, const LoadLinkedPrefix& prefix, std::uint16_t fragmentId) {
    Check(prefix.geometry.binding.size() <= (std::numeric_limits<std::uint32_t>::max)(),
        "generation.motion.binding", "binding長をuint32で表現できません");
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned char byte : std::string_view("MOTION-V1")) HashByte(hash, byte);
    HashLittleEndian(hash, seed, 4);
    HashLittleEndian(hash, static_cast<std::uint32_t>(prefix.count), 4);
    HashLittleEndian(hash, static_cast<std::uint32_t>(prefix.geometry.binding.size()), 4);
    for (unsigned char byte : prefix.geometry.binding) HashByte(hash, byte);
    HashLittleEndian(hash, fragmentId, 2);
    return hash;
}
void ValidateMotion(const LoadLinkedFragmentMotion& motion, const LoadLinkedCollapse& settings) {
    const double duration = settings.duration;
    const double distance = Norm(motion.initialVelocity) * duration +
        0.5 * settings.fallAcceleration * duration * duration;
    Check(std::isfinite(distance) && distance <= 10000, "generation.motion.position",
        "生成係数の軌道上限を超えています", FailureKind::Motion);
    for (std::size_t i = 0; i < 3; ++i) {
        Check(std::isfinite(motion.initialVelocity[i]) && std::isfinite(motion.angularVelocityDegrees[i]) &&
            std::abs(motion.angularVelocityDegrees[i]) * duration <= 360000,
            "generation.motion.coefficients", "係数が非有限または角度上限を超えています", FailureKind::Motion);
    }
}
LoadLinkedFragmentMotion MakeMotion(const LoadLinkedPrefix& prefix, const GeometryFragment& fragment,
    const LoadLinkedCollapse& settings, std::uint32_t seed, double ratio) {
    auto state = MotionState(seed, prefix, fragment.fragmentId);
    // 設定のばらつきが0でも、凍結した6出力を同じ順に消費する。
    const double speedSample = U01(state), coneSample = U01(state), azimuthSample = U01(state);
    const D3 rotationSamples{U01(state), U01(state), U01(state)};
    D3 base;
    const D3 savedDirection{settings.direction.x, settings.direction.y, settings.direction.z};
    if (settings.directionMode == LoadLinkedDirectionMode::Outward) {
        base = {ratio * (static_cast<double>(fragment.centroid.x) - 0.5), 0.5 - fragment.centroid.y, 0};
        base = Norm(base) > 1e-6 ? Normalize(base) : Normalize(savedDirection);
    } else {
        Check(settings.directionMode == LoadLinkedDirectionMode::Fixed, "generation.motion.directionMode", "未知の方向方式です");
        base = Normalize(savedDirection);
    }
    D3 direction = base;
    if (settings.directionVariationDegrees != 0) {
        D3 helper{};
        std::size_t axis = 0;
        if (std::abs(base[1]) < std::abs(base[axis])) axis = 1;
        if (std::abs(base[2]) < std::abs(base[axis])) axis = 2;
        helper[axis] = 1; // 同値はX→Y→Zの順で保持する。
        const D3 e1 = Normalize(Cross(helper, base)), e2 = Cross(base, e1);
        const double beta = settings.directionVariationDegrees * Pi / 180;
        const double cosine = 1 - coneSample * (1 - std::cos(beta));
        const double sine = std::sqrt((std::max)(0.0, 1 - cosine * cosine));
        const double phi = 2 * Pi * azimuthSample;
        for (std::size_t i = 0; i < 3; ++i)
            direction[i] = cosine * base[i] + sine * (std::cos(phi) * e1[i] + std::sin(phi) * e2[i]);
        direction = Normalize(direction);
    }
    const double speed = settings.launchSpeed * (1 + settings.speedVariation * (2 * speedSample - 1));
    const D3 angular{settings.angularVelocity.x, settings.angularVelocity.y, settings.angularVelocity.z};
    const D3 variation{settings.rotationVariation.x, settings.rotationVariation.y, settings.rotationVariation.z};
    LoadLinkedFragmentMotion motion;
    motion.fragmentId = fragment.fragmentId;
    for (std::size_t i = 0; i < 3; ++i) {
        motion.initialVelocity[i] = speed * direction[i];
        motion.angularVelocityDegrees[i] = angular[i] + variation[i] * (2 * rotationSamples[i] - 1);
    }
    ValidateMotion(motion, settings);
    return motion;
}
} // namespace

struct LoadLinkedGenerated::Data {
    std::uint64_t requestId = 0, settingsRevision = 0;
    LoadLinkedAsset settings;
    std::uint32_t captureWidth = 0, captureHeight = 0, cutSeed = 0, motionSeed = 0;
    std::vector<LoadLinkedResolvedCut> cuts;
    std::vector<CutSegment> segments;
    std::vector<LoadLinkedPrefix> prefixes;
};

struct LoadLinkedGeneration::Impl {
    // 候補・線分・GeometryはDataだけに格納し、storeは処理位置と完了証跡だけを持つ。
    struct PrefixStore {
        std::size_t minimum = 0, maximum = 0, verifiedPrefixes = 0;
        bool cutsFinished = false;
        void Initialize(LoadLinkedGenerated::Data& data) {
            minimum = data.settings.minimumCuts;
            maximum = data.settings.maximumCuts;
            data.cuts.reserve(maximum);
            data.segments.reserve(maximum);
            data.prefixes.reserve(maximum - minimum + 1);
        }
        void Accept(LoadLinkedGenerated::Data& data, const Cut& cut, Geometry&& geometry) {
            const auto count = data.cuts.size() + 1;
            Check(!cutsFinished && count <= maximum && geometry.cutSegments.size() == count,
                "generation.prefix.accept", "受理prefixの件数が一致しません");
            for (std::size_t i = 0; i + 1 < count; ++i)
                Check(SameSegment(data.segments[i], geometry.cutSegments[i]),
                    "generation.prefix.segments", "受理済み線分が一致しません");
            const auto segment = geometry.cutSegments.back();
            data.cuts.push_back({cut.id, cut.from, cut.to});
            data.segments.push_back(segment);
            if (count >= minimum) data.prefixes.push_back({count, std::move(geometry), {}});
        }
        void CheckGeometry(const LoadLinkedPrefix& prefix) const {
            Check(!prefix.geometry.fragments.empty() && prefix.geometry.cutSegments.size() == prefix.count,
                "generation.prefix.geometry", "prefixの幾何が空または線分数が不正です");
            for (std::size_t i = 1; i < prefix.geometry.fragments.size(); ++i)
                Check(prefix.geometry.fragments[i - 1].fragmentId < prefix.geometry.fragments[i].fragmentId,
                    "generation.prefix.ids", "fragmentIdが厳密昇順ではありません");
        }
        std::size_t FinishCuts(LoadLinkedGenerated::Data& data) {
            Check(data.cuts.size() == maximum && data.segments.size() == maximum &&
                data.prefixes.size() == maximum - minimum + 1,
                "generation.prefix.count", "生成済みprefixの件数が不正です");
            std::size_t total = 0;
            for (std::size_t i = 0; i < data.prefixes.size(); ++i) {
                auto& prefix = data.prefixes[i];
                Check(prefix.count == minimum + i, "generation.prefix.order", "prefixのNが連続していません");
                CheckGeometry(prefix);
                prefix.motions.reserve(prefix.geometry.fragments.size());
                total += prefix.geometry.fragments.size();
            }
            cutsFinished = true;
            Check(total <= MaxCuts * MaxFragments, "generation.motion.capacity", "motion容量が上限を超えています");
            return total;
        }
        void AppendMotion(LoadLinkedGenerated::Data& data, std::size_t prefixIndex, LoadLinkedFragmentMotion motion) {
            Check(cutsFinished && prefixIndex == verifiedPrefixes && prefixIndex < data.prefixes.size(),
                "generation.motion.order", "motionのprefix処理順が不正です");
            auto& prefix = data.prefixes[prefixIndex];
            const auto index = prefix.motions.size();
            Check(index < prefix.geometry.fragments.size() && prefix.geometry.fragments[index].fragmentId == motion.fragmentId,
                "generation.motion.id", "motionと幾何のIDが一致しません");
            ValidateMotion(motion, data.settings.collapse);
            prefix.motions.push_back(std::move(motion));
            if (prefix.motions.size() == prefix.geometry.fragments.size()) {
                for (std::size_t i = 0; i < prefix.motions.size(); ++i)
                    Check(prefix.motions[i].fragmentId == prefix.geometry.fragments[i].fragmentId,
                        "generation.motion.ids", "prefix完了時のIDが一致しません");
                ++verifiedPrefixes;
            }
        }
        void CheckPublish(const LoadLinkedGenerated::Data& data, const LoadLinkedGenerationStatus& status) const {
            Check(cutsFinished && verifiedPrefixes == data.prefixes.size() &&
                data.requestId == status.requestId && data.settingsRevision == status.settingsRevision &&
                data.captureWidth == status.captureWidth && data.captureHeight == status.captureHeight &&
                data.cutSeed == status.cutSeed && data.motionSeed == status.motionSeed &&
                data.cuts.size() == maximum && data.segments.size() == maximum &&
                data.prefixes.size() == maximum - minimum + 1 && status.acceptedCuts == maximum &&
                status.completedMotions == status.totalMotions,
                "generation.publish", "完成物のmetadataまたは完了数が一致しません");
        }
    };
    struct Workspace {
        std::unique_ptr<LoadLinkedGenerated::Data> data;
        PrefixStore store;
        std::vector<Cut> buildCuts;
        std::vector<CandidateLine> lines;
        std::uint64_t randomState = 0;
        double ratio = 1;
    };
    LoadLinkedGenerationStatus status;
    LoadLinkedGenerationFailure failure;
    std::unique_ptr<Workspace> workspace;
    std::shared_ptr<const LoadLinkedGenerated> result;

    // 診断割当が失敗しても、workspaceを解放して終端した状態を保持する。
    void Fail(FailureKind kind) noexcept {
        failure.kind = kind;
        failure.cutIndex = status.acceptedCuts;
        if (workspace && workspace->data && status.state == State::GeneratingMotion) {
            const auto index = workspace->store.verifiedPrefixes;
            if (index < workspace->data->prefixes.size()) {
                const auto& prefix = workspace->data->prefixes[index];
                failure.prefixCount = prefix.count;
                const auto fragment = prefix.motions.size();
                if (fragment < prefix.geometry.fragments.size()) {
                    failure.fragmentId = prefix.geometry.fragments[fragment].fragmentId;
                    failure.hasFragment = true;
                }
            }
        }
        status.state = State::Failed;
        workspace.reset();
        result.reset();
    }
    void Reject(Rejection reason) {
        ++failure.rejections[static_cast<std::size_t>(reason)];
        if (status.attemptsForCurrentCut == AttemptsPerCut) {
            Fail(FailureKind::TrialLimit);
            failure.diagnostics.push_back({"generation.cuts.attempts", "現在cutの256候補がすべて不適合でした"});
        }
    }
    void BuildCandidate(Cut cut, const CandidateLine* line, bool random) {
        auto& work = *workspace;
        work.buildCuts.push_back(std::move(cut));
        Geometry geometry;
        Diagnostics diagnostics;
        const auto geometryStatus = TryBuildGeometryDetailed(work.buildCuts, geometry, diagnostics);
        if (geometryStatus != GeometryBuildStatus::Success) {
            work.buildCuts.pop_back();
            failure.geometryStatus = geometryStatus;
            switch (geometryStatus) {
            case GeometryBuildStatus::CandidateRejected:
            case GeometryBuildStatus::InvalidInput:
            case GeometryBuildStatus::CapacityExceeded:
            case GeometryBuildStatus::InternalFailure:
            case GeometryBuildStatus::AllocationFailure:
                break;
            default:
                throw GenerationIssue{FailureKind::Internal, "generation.geometry.status", "未知の幾何Build結果です"};
            }
            if (random && geometryStatus == GeometryBuildStatus::CandidateRejected) {
                failure.diagnostics = std::move(diagnostics);
                Reject(Rejection::Geometry);
            } else {
                Fail(FailureKind::Geometry);
                failure.prefixCount = status.acceptedCuts + 1;
                failure.diagnostics = std::move(diagnostics);
            }
            return;
        }
        work.store.Accept(*work.data, work.buildCuts.back(), std::move(geometry));
        if (line) work.lines.push_back(*line);
        status.acceptedCuts = work.data->cuts.size();
        status.attemptsForCurrentCut = 0;
        failure = {};
        if (status.acceptedCuts == work.data->settings.maximumCuts) {
            status.totalMotions = work.store.FinishCuts(*work.data);
            status.state = State::GeneratingMotion; // motionは次のStepから作る。
        }
    }
    void StepCuts() {
        auto& work = *workspace;
        if (const auto* authored = std::get_if<LoadLinkedAuthoredSource>(&work.data->settings.cutSource)) {
            const auto& saved = authored->cuts[status.acceptedCuts];
            Cut cut;
            cut.id = saved.id;
            cut.from = saved.from;
            cut.to = saved.to;
            BuildCandidate(std::move(cut), nullptr, false);
            return;
        }
        const auto* source = std::get_if<LoadLinkedRandomSource>(&work.data->settings.cutSource);
        Check(source != nullptr && source->generationVersion == 1, "generation.cuts.source", "生成方式が不正です");
        ++status.attemptsForCurrentCut;
        ++status.totalAttempts;
        const double angle = U01(work.randomState), x = U01(work.randomState), y = U01(work.randomState);
        Cut cut;
        CandidateLine line;
        if (const auto rejection = RandomCandidate(*source, work.ratio, angle, x, y, work.lines, cut, line)) {
            Reject(*rejection);
            return;
        }
        cut.id = "cut_" + std::to_string(status.acceptedCuts);
        BuildCandidate(std::move(cut), &line, true);
    }
    void StepMotion() {
        auto& work = *workspace;
        const auto prefixIndex = work.store.verifiedPrefixes;
        Check(prefixIndex < work.data->prefixes.size(), "generation.motion.prefix", "motion対象のprefixがありません");
        const auto& prefix = work.data->prefixes[prefixIndex];
        const auto index = prefix.motions.size();
        Check(index < prefix.geometry.fragments.size(), "generation.motion.fragment", "motion対象のfragmentがありません");
        auto motion = MakeMotion(prefix, prefix.geometry.fragments[index], work.data->settings.collapse,
            work.data->motionSeed, work.ratio);
        work.store.AppendMotion(*work.data, prefixIndex, std::move(motion));
        ++status.completedMotions;
        if (status.completedMotions == status.totalMotions) {
            work.store.CheckPublish(*work.data, status);
            // 本体とshared control blockをCompletedより先に確保する。失敗なら公開しない。
            std::shared_ptr<const LoadLinkedGenerated> complete(new LoadLinkedGenerated(std::move(work.data)));
            result = std::move(complete);
            workspace.reset();
            status.state = State::Completed;
        }
    }
};

LoadLinkedGenerated::LoadLinkedGenerated(std::unique_ptr<Data> data) noexcept : data_(std::move(data)) {}
LoadLinkedGenerated::~LoadLinkedGenerated() = default;
std::uint64_t LoadLinkedGenerated::RequestId() const noexcept { return data_->requestId; }
std::uint64_t LoadLinkedGenerated::SettingsRevision() const noexcept { return data_->settingsRevision; }
const LoadLinkedAsset& LoadLinkedGenerated::Settings() const noexcept { return data_->settings; }
std::uint32_t LoadLinkedGenerated::CaptureWidth() const noexcept { return data_->captureWidth; }
std::uint32_t LoadLinkedGenerated::CaptureHeight() const noexcept { return data_->captureHeight; }
std::uint32_t LoadLinkedGenerated::CutSeed() const noexcept { return data_->cutSeed; }
std::uint32_t LoadLinkedGenerated::MotionSeed() const noexcept { return data_->motionSeed; }
const std::vector<LoadLinkedResolvedCut>& LoadLinkedGenerated::Cuts() const noexcept { return data_->cuts; }
const std::vector<CutSegment>& LoadLinkedGenerated::CutSegments() const noexcept { return data_->segments; }
const LoadLinkedPrefix* LoadLinkedGenerated::FindPrefix(std::size_t count) const noexcept {
    if (count < data_->settings.minimumCuts || count > data_->settings.maximumCuts) return nullptr;
    return &data_->prefixes[count - data_->settings.minimumCuts];
}
LoadLinkedGeneration::LoadLinkedGeneration() : impl_(std::make_unique<Impl>()) {}
LoadLinkedGeneration::~LoadLinkedGeneration() = default;
bool LoadLinkedGeneration::TryBegin(const LoadLinkedGenerationInput& input, LoadLinkedGenerationFailure& failure) {
    static_assert(std::is_nothrow_move_assignable_v<LoadLinkedGenerationFailure>);
    try {
        const auto& previous = impl_->status;
        Check(previous.state == State::Idle || previous.state == State::Failed || previous.state == State::Cancelled ||
            (previous.state == State::Completed && previous.resultTaken),
            "generation.begin", "生成中または未取得の完成結果があります", FailureKind::InvalidInput);
        Check(input.requestId != 0 && input.requestId != previous.requestId,
            "generation.input.requestId", "要求IDは非0で直前のIDと異なる必要があります", FailureKind::InvalidInput);
        Check(input.settings != nullptr, "generation.input.settings", "設定がありません", FailureKind::InvalidInput);
        Check(input.captureWidth >= 1 && input.captureWidth <= 4096 && input.captureHeight >= 1 && input.captureHeight <= 4096,
            "generation.input.captureSize", "Captureの各寸法は1〜4096です", FailureKind::InvalidInput);
        // mutable aliasから独立するコピーを、既存workspaceへ触れる前に検証する。
        auto data = std::make_unique<LoadLinkedGenerated::Data>();
        data->settings = *input.settings;
        LoadLinkedDiagnostics validation;
        if (!TryValidateLoadLinked(data->settings, validation)) {
            failure = {};
            failure.kind = FailureKind::InvalidInput;
            failure.validationDiagnostics = std::move(validation);
            return false;
        }
        std::uint32_t cutSeed = 0;
        if (const auto* random = std::get_if<LoadLinkedRandomSource>(&data->settings.cutSource)) {
            Check(random->seed.mode == LoadLinkedSeedMode::PerRequest || input.cutSeed == random->seed.value,
                "generation.input.cutSeed", "Fixed cutSeedが保存値と一致しません", FailureKind::InvalidInput);
            cutSeed = input.cutSeed;
        }
        Check(data->settings.motionSeed.mode == LoadLinkedSeedMode::PerRequest ||
            input.motionSeed == data->settings.motionSeed.value,
            "generation.input.motionSeed", "Fixed motionSeedが保存値と一致しません", FailureKind::InvalidInput);
        data->requestId = input.requestId;
        data->settingsRevision = input.settingsRevision;
        data->captureWidth = input.captureWidth;
        data->captureHeight = input.captureHeight;
        data->cutSeed = cutSeed;
        data->motionSeed = input.motionSeed;
        auto workspace = std::make_unique<Impl::Workspace>();
        workspace->data = std::move(data);
        workspace->store.Initialize(*workspace->data);
        workspace->buildCuts.reserve(workspace->data->settings.maximumCuts);
        workspace->lines.reserve(workspace->data->settings.maximumCuts);
        workspace->ratio = static_cast<double>(input.captureWidth) / input.captureHeight;
        LoadLinkedGenerationStatus status;
        status.state = State::GeneratingCuts;
        status.requestId = input.requestId;
        status.settingsRevision = input.settingsRevision;
        status.captureWidth = input.captureWidth;
        status.captureHeight = input.captureHeight;
        status.cutSeed = cutSeed;
        status.motionSeed = input.motionSeed;
        if (const auto* random = std::get_if<LoadLinkedRandomSource>(&workspace->data->settings.cutSource)) {
            status.randomGenerationVersion = random->generationVersion;
            workspace->randomState = static_cast<std::uint64_t>(cutSeed) ^ UINT64_C(0x4355540000000001);
        }
        // ここ以降は割当不要。成功時だけ要求と計数を一括交換する。
        impl_->workspace = std::move(workspace);
        impl_->result.reset();
        impl_->status = status;
        impl_->failure = {};
        failure = {};
        return true;
    } catch (const GenerationIssue& error) {
        failure = {};
        failure.kind = error.kind;
        failure.diagnostics.push_back({error.field, error.detail});
    } catch (const std::bad_alloc& error) {
        failure = {};
        failure.kind = FailureKind::Allocation;
        failure.diagnostics.push_back({"generation.begin", error.what()});
    } catch (const std::exception& error) {
        failure = {};
        failure.kind = FailureKind::Internal;
        failure.diagnostics.push_back({"generation.begin", error.what()});
    }
    return false;
}
LoadLinkedGenerationState LoadLinkedGeneration::Step() {
    if (impl_->status.state != State::GeneratingCuts && impl_->status.state != State::GeneratingMotion)
        return impl_->status.state;
    try {
        if (impl_->status.state == State::GeneratingCuts) impl_->StepCuts();
        else impl_->StepMotion();
    } catch (const GenerationIssue& error) {
        if (impl_->status.state == State::Failed) throw;
        impl_->Fail(error.kind);
        impl_->failure.diagnostics.push_back({error.field, error.detail});
    } catch (const std::bad_alloc& error) {
        if (impl_->status.state == State::Failed) throw;
        impl_->Fail(FailureKind::Allocation);
        impl_->failure.diagnostics.push_back({"generation.step", error.what()});
    } catch (const std::exception& error) {
        if (impl_->status.state == State::Failed) throw;
        impl_->Fail(FailureKind::Internal);
        impl_->failure.diagnostics.push_back({"generation.step", error.what()});
    } catch (...) {
        if (impl_->status.state != State::Failed) impl_->Fail(FailureKind::Internal);
        throw;
    }
    return impl_->status.state;
}
void LoadLinkedGeneration::Cancel() noexcept {
    const auto state = impl_->status.state;
    if (state != State::GeneratingCuts && state != State::GeneratingMotion &&
        !(state == State::Completed && !impl_->status.resultTaken)) return;
    impl_->workspace.reset();
    impl_->result.reset();
    impl_->failure = {};
    impl_->status.state = State::Cancelled;
}
LoadLinkedGenerationStatus LoadLinkedGeneration::GetStatus() const noexcept { return impl_->status; }
const LoadLinkedGenerationFailure& LoadLinkedGeneration::GetFailure() const noexcept { return impl_->failure; }
bool LoadLinkedGeneration::TryTakeResult(std::uint64_t expectedRequestId,
    std::shared_ptr<const LoadLinkedGenerated>& out) noexcept {
    if (impl_->status.state != State::Completed || impl_->status.resultTaken ||
        impl_->status.requestId != expectedRequestId) return false;
    out = std::move(impl_->result);
    impl_->status.resultTaken = true;
    return true;
}
} // namespace SceneTransitionAsset
