// 役割: ロード連動演出のphase入力を検査し、切断/散乱/文字の純CPU sampleを完成後に一括反映する。
#include "LoadLinkedEvaluation.h"
#include "../math/Math.h"
#include <algorithm>
#include <cmath>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

namespace SceneTransitionAsset {
namespace {
using EvaluationPhase = LoadLinkedEvaluationPhase;
using Code = LoadLinkedDiagnosticCode;
constexpr double Pi = 3.141592653589793238462643383279502884;
struct EvaluationFailure { LoadLinkedDiagnostic diagnostic; };
[[noreturn]] void Fail(Code code, const std::string& field, const char* detail) {
    throw EvaluationFailure{{code, field, detail}};
}
void Require(bool condition, Code code, const std::string& field, const char* detail) {
    if (!condition) Fail(code, field, detail);
}
std::string At(const char* field, std::size_t index) {
    return std::string(field) + "[" + std::to_string(index) + "]";
}
void Range(double value, double low, double high, const std::string& field) {
    Require(std::isfinite(value) && value >= low && value <= high,
        Code::OutOfRange, field, "有限値かつ許可範囲内である必要があります");
}
float OutputFloat(double value, double low, double high, const std::string& field) {
    Range(value, low, high, field);
    const float converted = static_cast<float>(value);
    Range(converted, low, high, field);
    return converted;
}
bool Waiting(EvaluationPhase phase) noexcept { return phase == EvaluationPhase::Waiting || phase == EvaluationPhase::Frozen; }
void ValidatePhase(EvaluationPhase phase) {
    switch (phase) {
    case EvaluationPhase::Cutting:
    case EvaluationPhase::Interval:
    case EvaluationPhase::Waiting:
    case EvaluationPhase::Frozen:
    case EvaluationPhase::Collapse:
        return;
    default: Fail(Code::InvalidType, "evaluation.phase", "未知の評価phaseです");
    }
}
void ValidateCounts(const LoadLinkedPresentationInput& input, std::size_t minimum, std::size_t maximum) {
    Require(minimum >= 1 && minimum <= maximum && maximum <= MaxCuts,
        Code::InternalFailure, "evaluation.settings", "生成物の本数設定が不正です");
    Require(input.completedCuts <= maximum, Code::OutOfRange, "evaluation.completedCuts", "最大本数を超えています");
    const auto count = input.completedCuts;
    switch (input.phase) {
    case EvaluationPhase::Cutting:
        Require(count < maximum, Code::OutOfRange, "evaluation.completedCuts", "進行中cutがありません");
        break;
    case EvaluationPhase::Interval:
        Require(count >= 1 && count < maximum, Code::OutOfRange, "evaluation.completedCuts", "Intervalの本数が不正です");
        break;
    case EvaluationPhase::Waiting:
        Require(count == maximum, Code::OutOfRange, "evaluation.completedCuts", "Waitingは最大本数の完了が必要です");
        break;
    case EvaluationPhase::Frozen:
    case EvaluationPhase::Collapse:
        Require(count >= minimum, Code::OutOfRange, "evaluation.completedCuts", "最低本数以上の確定本数が必要です");
        break;
    }
    Require(input.cutIndex == (input.phase == EvaluationPhase::Cutting ? count : 0),
        Code::OutOfRange, "evaluation.cutIndex", "phaseとcutIndexが一致しません");
    const bool frozen = input.phase == EvaluationPhase::Frozen || input.phase == EvaluationPhase::Collapse;
    Require(input.frozenCuts == (frozen ? count : 0),
        Code::OutOfRange, "evaluation.frozenCuts", "phaseと確定本数が一致しません");
}
void ValidateClocks(const LoadLinkedPresentationInput& input) {
    Require(std::isfinite(input.localTime), Code::OutOfRange, "evaluation.localTime", "局所時刻は有限値が必要です");
    if (input.phase != EvaluationPhase::Cutting && input.phase != EvaluationPhase::Collapse)
        Require(input.localTime == 0, Code::OutOfRange, "evaluation.localTime", "未使用の局所時刻は0が必要です");
    for (std::size_t i = 0; i < MaxCuts; ++i) {
        const auto field = At("evaluation.completedCutAges", i);
        Require(std::isfinite(input.completedCutAges[i]) && input.completedCutAges[i] >= 0,
            Code::OutOfRange, field, "ageは有限/非負が必要です");
        if (input.phase == EvaluationPhase::Collapse || i >= input.completedCuts)
            Require(input.completedCutAges[i] == 0, Code::OutOfRange, field, "未使用のageは0が必要です");
    }
    Require(std::isfinite(input.textAge) && input.textAge >= 0, Code::OutOfRange,
        "evaluation.textAge", "文字ageは有限/非負が必要です");
    Require(std::isfinite(input.waitAge) && input.waitAge >= 0, Code::OutOfRange,
        "evaluation.waitAge", "待機ageは有限/非負が必要です");
    if (!Waiting(input.phase))
        Require(input.waitAge == 0, Code::OutOfRange, "evaluation.waitAge", "未使用の待機ageは0が必要です");
    for (std::size_t i = 0; i < MaxCuts; ++i) {
        const auto field = At("evaluation.waitStartGlows", i);
        Range(input.waitStartGlows[i], 0, 1, field);
        if (!Waiting(input.phase) || i >= input.completedCuts)
            Require(input.waitStartGlows[i] == 0, Code::OutOfRange, field, "未使用の待機glowは0が必要です");
    }
}
const LoadLinkedPrefix* ValidateGeometry(const LoadLinkedGenerated& generated, const LoadLinkedPresentationInput& input) {
    const auto maximum = generated.Settings().maximumCuts;
    Require(generated.Cuts().size() == maximum && generated.CutSegments().size() == maximum,
        Code::InternalFailure, "evaluation.generated", "候補と描画線分のM件対応が不正です");
    if (input.phase != EvaluationPhase::Frozen && input.phase != EvaluationPhase::Collapse) return nullptr;
    const auto* prefix = generated.FindPrefix(input.frozenCuts);
    Require(prefix != nullptr && prefix->count == input.frozenCuts,
        Code::InternalFailure, "evaluation.prefix", "選択したNのprefixがありません");
    const auto& geometry = prefix->geometry;
    Require(!geometry.binding.empty() && !geometry.fragments.empty() && geometry.fragments.size() <= MaxFragments &&
        !geometry.vertices.empty() && geometry.vertices.size() <= MaxVertices &&
        !geometry.indices.empty() && geometry.indices.size() <= MaxIndices && geometry.indices.size() % 3 == 0 &&
        geometry.indices.size() / 3 <= MaxTriangles && geometry.cutSegments.size() == prefix->count &&
        prefix->motions.size() == geometry.fragments.size(),
        Code::InternalFailure, "evaluation.prefix", "選択prefixの容量または対応件数が不正です");
    for (std::size_t i = 0; i < geometry.fragments.size(); ++i) {
        Require((i == 0 || geometry.fragments[i - 1].fragmentId < geometry.fragments[i].fragmentId) &&
            geometry.fragments[i].fragmentId == prefix->motions[i].fragmentId, Code::InternalFailure,
            At("evaluation.motion", i) + ".fragmentId", "選択prefixの幾何とmotionのIDが一致しません");
    }
    return prefix;
}
// V1 Curveの計算順とfloat丸めを維持する。既存V1の関数・公開契約は変更しない。
float Curve(Easing easing, double ratio) {
    const float t = static_cast<float>(std::clamp(ratio, 0.0, 1.0));
    switch (easing) {
    case Easing::Linear: return t;
    case Easing::EaseIn: return t * t * t;
    case Easing::EaseOut: return Math::EaseOutCubic(t);
    case Easing::EaseInOut: {
        const float inverse = -2 * t + 2;
        return t < 0.5f ? 4 * t * t * t : 1 - inverse * inverse * inverse / 2;
    }
    case Easing::SmoothStep: return Math::SmoothStep(t);
    default: Fail(Code::InternalFailure, "evaluation.settings", "未知のsweep easingです");
    }
}
double CompletedGlow(const LoadLinkedAsset& settings, const LoadLinkedPresentationInput& input, std::size_t index) {
    if (Waiting(input.phase)) {
        const double weight = input.waitAge >= 0.2 ? 1 : input.waitAge / 0.2;
        const double remainder = std::fmod(input.waitAge, 1.2);
        const double pulse = 0.25 + 0.75 * (1 - std::cos(2 * Pi * remainder / 1.2)) / 2;
        return (1 - weight) * input.waitStartGlows[index] + weight * pulse;
    }
    const double afterglow = settings.cutStyle.afterglowDuration;
    Require(std::isfinite(afterglow) && afterglow >= 0, Code::InternalFailure,
        "evaluation.settings", "残光durationが不正です");
    // 大きい有限ageを除算する前に端点判定し、不要なoverflowを避ける。
    if (afterglow == 0 || input.completedCutAges[index] >= afterglow) return 0;
    return (std::max)(0.0, 1 - input.completedCutAges[index] / afterglow);
}
void EvaluateCuts(const LoadLinkedGenerated& generated, const LoadLinkedPresentationInput& input, LoadLinkedFrameSample& sample) {
    const auto& settings = generated.Settings();
    const auto& style = settings.cutStyle;
    const auto count = input.phase == EvaluationPhase::Collapse ? input.frozenCuts : settings.maximumCuts;
    sample.cuts.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        CutSample cut;
        cut.segment = generated.CutSegments()[i];
        cut.lineColor = style.lineColor;
        cut.glowColor = style.glowColor;
        cut.lineWidth = style.lineWidth;
        cut.glowWidth = style.glowWidth;
        cut.glowIntensity = style.glowIntensity;
        const auto field = At("evaluation.cuts", i);
        if (input.phase == EvaluationPhase::Collapse) {
            cut.reveal = 1;
        } else if (i < input.completedCuts) {
            cut.reveal = 1;
            cut.glow = OutputFloat(CompletedGlow(settings, input, i), 0, 1, field + ".glow");
        } else if (input.phase == EvaluationPhase::Cutting && i == input.cutIndex) {
            cut.reveal = OutputFloat(Curve(style.sweepEasing, sample.evaluatedLocalTime / settings.cutSweepDuration),
                0, 1, field + ".reveal");
            cut.glow = 1;
        }
        sample.cuts.push_back(cut);
    }
}
Vector3 PoseVector(const std::array<double, 3>& value, double limit, const std::string& field) {
    return {OutputFloat(value[0], -limit, limit, field), OutputFloat(value[1], -limit, limit, field),
        OutputFloat(value[2], -limit, limit, field)};
}
void EvaluateCollapse(const LoadLinkedPrefix& prefix, const LoadLinkedCollapse& settings, LoadLinkedFrameSample& sample) {
    const double time = sample.evaluatedLocalTime;
    sample.fragments.reserve(prefix.motions.size());
    for (std::size_t i = 0; i < prefix.motions.size(); ++i) {
        const auto& motion = prefix.motions[i];
        FragmentPose pose;
        pose.fragmentId = motion.fragmentId;
        if (time > 0) {
            std::array<double, 3> position{}, rotation{};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                position[axis] = motion.initialVelocity[axis] * time;
                rotation[axis] = motion.angularVelocityDegrees[axis] * time;
            }
            position[1] -= 0.5 * settings.fallAcceleration * time * time;
            const auto field = At("evaluation.fragments", i);
            pose.position = PoseVector(position, 10000, field + ".position");
            pose.rotationEulerDegrees = PoseVector(rotation, 360000, field + ".rotationEulerDegrees");
        }
        // t=0は既定の位置/回転0・scale1。alpha=0でも全姿勢の検査を先に完了する。
        sample.fragments.push_back(pose);
    }
    const double duration = settings.duration, fade = settings.endFadeDuration;
    double opacity = 1;
    if (fade > 0) {
        if (time >= duration) opacity = 0;
        else if (time > duration - fade) opacity = (duration - time) / fade;
    }
    sample.fragmentOpacity = OutputFloat(opacity, 0, 1, "evaluation.fragmentOpacity");
}
void EvaluateText(const LoadLinkedText& settings, const LoadLinkedPresentationInput& input, LoadLinkedFrameSample& sample) {
    if (input.phase == EvaluationPhase::Collapse || !settings.enabled) return;
    sample.textVisible = true;
    double opacity = 1;
    switch (settings.motion) {
    case LoadLinkedTextMotion::Static:
        break;
    case LoadLinkedTextMotion::Pulse:
        Require(std::isfinite(settings.pulsePeriod) && settings.pulsePeriod > 0,
            Code::InternalFailure, "evaluation.settings", "文字pulse周期が不正です");
        if (input.textAge != 0) {
            const double period = settings.pulsePeriod, minimum = settings.minimumOpacity;
            const double remainder = std::fmod(input.textAge, period);
            opacity = minimum + (1 - minimum) * (1 + std::cos(2 * Pi * remainder / period)) / 2;
        }
        break;
    default: Fail(Code::InternalFailure, "evaluation.settings", "未知の文字motionです");
    }
    sample.textOpacity = OutputFloat(opacity, 0, 1, "evaluation.textOpacity");
}
} // namespace

bool TryEvaluateLoadLinked(const std::shared_ptr<const LoadLinkedGenerated>& generated,
    const LoadLinkedPresentationInput& input, LoadLinkedFrameSample& outSample, LoadLinkedDiagnostics& diagnostics) {
    // generatedがoutSample.generatedへの参照でも、commit前後のborrowの寿命を保つ。
    const auto owner = generated;
    diagnostics.clear();
    try {
        Require(owner != nullptr, Code::InvalidType, "evaluation.generated", "生成物がありません");
        Require(input.expectedRequestId != 0, Code::InvalidType, "evaluation.expectedRequestId", "要求IDは非0が必要です");
        Require(input.expectedRequestId == owner->RequestId(), Code::ExternalChange,
            "evaluation.expectedRequestId", "生成要求が一致しません");
        Require(input.expectedSettingsRevision == owner->SettingsRevision(), Code::ExternalChange,
            "evaluation.expectedSettingsRevision", "設定revisionが一致しません");
        Require(input.presentationGeneration != 0, Code::InvalidType,
            "evaluation.presentationGeneration", "表示generationは非0が必要です");
        ValidatePhase(input.phase);
        const auto& settings = owner->Settings();
        ValidateCounts(input, settings.minimumCuts, settings.maximumCuts);
        ValidateClocks(input);
        const auto* prefix = ValidateGeometry(*owner, input);

        LoadLinkedFrameSample sample;
        sample.generated = owner;
        sample.presentationGeneration = input.presentationGeneration;
        sample.sampleRevision = input.sampleRevision;
        sample.phase = input.phase;
        sample.completedCuts = input.completedCuts;
        sample.prefixCount = prefix ? prefix->count : 0;
        if (input.phase == EvaluationPhase::Cutting) {
            Require(std::isfinite(settings.cutSweepDuration) && settings.cutSweepDuration > 0,
                Code::InternalFailure, "evaluation.settings", "sweep durationが不正です");
            sample.evaluatedLocalTime = std::clamp(input.localTime, 0.0, static_cast<double>(settings.cutSweepDuration));
        } else if (input.phase == EvaluationPhase::Collapse) {
            Require(std::isfinite(settings.collapse.duration) && settings.collapse.duration > 0 &&
                std::isfinite(settings.collapse.endFadeDuration) && settings.collapse.endFadeDuration >= 0 &&
                settings.collapse.endFadeDuration <= settings.collapse.duration,
                Code::InternalFailure, "evaluation.settings", "散乱duration/fadeが不正です");
            sample.evaluatedLocalTime = std::clamp(input.localTime, 0.0, static_cast<double>(settings.collapse.duration));
            sample.drawMode = LoadLinkedDrawMode::Fragments;
        }
        EvaluateCuts(*owner, input, sample);
        if (input.phase == EvaluationPhase::Collapse) EvaluateCollapse(*prefix, settings.collapse, sample);
        EvaluateText(settings.loadingText, input, sample);
        // 標準allocatorのvector/shared_ptrを含め、完成sampleの反映に割当を持ち込まない。
        static_assert(std::is_nothrow_swappable_v<LoadLinkedFrameSample>);
        using std::swap;
        swap(outSample, sample);
        return true;
    } catch (const EvaluationFailure& error) {
        diagnostics.push_back(error.diagnostic);
    } catch (const std::bad_alloc& error) {
        diagnostics.push_back({Code::ResourceFailure, "evaluation", error.what()});
    } catch (const std::exception& error) {
        diagnostics.push_back({Code::InternalFailure, "evaluation", error.what()});
    }
    return false;
}
} // namespace SceneTransitionAsset
