// 役割: Scene遷移演出の保存値、切断幾何、変更不能snapshotと純粋な時刻評価を定義する。
#pragma once

#include "../math/Vector2.h"
#include "../math/Vector3.h"
#include "../math/Vector4.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace SceneTransitionAsset {
// V1入力容量。演出の推奨値や測定済み性能限界ではない。
inline constexpr std::size_t MaxJsonBytes = 2 * 1024 * 1024;
inline constexpr int MaxJsonDepth = 16;
inline constexpr std::size_t MaxCuts = 16, MaxFragments = 137;
inline constexpr std::size_t MaxVertices = 548, MaxTriangles = 274, MaxIndices = 822;
inline constexpr std::size_t MaxPoseKeysPerFragment = 256, MaxPoseKeys = 4096;
inline constexpr std::size_t MaxFlashKeys = 256, MaxAudioCues = 128;

enum class Phase { Cutting, Collapse };
enum class Easing { Linear, EaseIn, EaseOut, EaseInOut, SmoothStep };
struct Diagnostic { std::string field; std::string message; };
using Diagnostics = std::vector<Diagnostic>;

struct Cut {
    std::string id;
    Vector2 from{}, to{}; // 左上原点のUV。from→toの左側をIDの1bitとする。
    float startTime = 0, sweepDuration = 0, afterglowDuration = 0;
    Easing sweepEasing = Easing::Linear;
    Vector4 lineColor{}, glowColor{};
    float lineWidth = 0, glowWidth = 0, glowIntensity = 0;
};
struct PoseKey {
    float time = 0; // Collapse内の秒。
    Vector3 position{}; // 元重心からの移動量。高さ=1、X右/Y上/Z奥。
    Vector3 rotationEulerDegrees{}; // 連続度数。補間前に周回を折り返さない。
    Vector3 scale{1, 1, 1};
    Easing easingToNext = Easing::Linear;
};
struct FragmentMotion { std::uint16_t fragmentId = 0; std::vector<PoseKey> keys; };
struct FlashKey {
    Phase phase = Phase::Cutting;
    float time = 0;
    Vector4 color{};
    float intensity = 0;
    Easing easingToNext = Easing::Linear;
};
struct AudioCue {
    std::string id;
    Phase phase = Phase::Cutting;
    float time = 0;
    std::string clipPath; // 空は明示無音。存在確認/Voice生成はcallerの責務。
    float volume = 0, pitch = 0;
};
struct Asset {
    std::string displayName;
    float cutPhaseDuration = 0, collapseDuration = 0;
    std::vector<Cut> cuts;
    std::string geometryBinding;
    std::vector<FragmentMotion> fragments;
    std::vector<FlashKey> flashKeys;
    std::vector<AudioCue> audioCues;
};
struct GeometryVertex { Vector2 uv{}; std::uint16_t fragmentId = 0; };
struct GeometryFragment {
    std::uint16_t fragmentId = 0;
    std::vector<Vector2> polygon;
    Vector2 centroid{};
    float area = 0;
    std::size_t firstVertex = 0, firstIndex = 0, indexCount = 0;
};
struct CutSegment { Vector2 from{}, to{}; };
struct Geometry {
    std::string binding; // hashではなくcut順/id/座標bitの完全記述。
    std::vector<GeometryFragment> fragments; // fragmentId昇順。
    std::vector<GeometryVertex> vertices;
    std::vector<std::uint32_t> indices; // polygon先頭頂点からのfan。
    std::vector<CutSegment> cutSegments; // Asset.cutsと同順。
};

// TryCompileだけが構築し、const共有所有で公開する。SceneやEditor draftの寿命に依存しない。
class CompiledTransition {
public:
    const Asset& GetAsset() const { return asset_; }
    const Geometry& GetGeometry() const { return geometry_; }
    const std::vector<FragmentMotion>& GetMotions() const { return motions_; }
    const std::vector<AudioCue>& GetAudioCues() const { return audioCues_; }
private:
    CompiledTransition() = default;
    Asset asset_;
    Geometry geometry_;
    std::vector<FragmentMotion> motions_; // Geometry.fragmentsと同順。
    std::vector<AudioCue> audioCues_; // phase/time/元配列順。
    friend bool TryCompile(const Asset&, std::shared_ptr<const CompiledTransition>&, Diagnostics&);
};
struct FragmentPose {
    std::uint16_t fragmentId = 0;
    Vector3 position{}, rotationEulerDegrees{}, scale{1, 1, 1};
};
struct CutSample {
    CutSegment segment;
    float reveal = 0, glow = 0; // glowは残光減衰係数。glowIntensityと別。
    Vector4 lineColor{}, glowColor{};
    float lineWidth = 0, glowWidth = 0, glowIntensity = 0;
};
struct FlashSample { Vector4 color{}; float intensity = 0; };
struct FrameSample {
    Phase phase = Phase::Cutting;
    float localTime = 0;
    std::vector<FragmentPose> fragments;
    std::vector<CutSample> cuts;
    FlashSample flash;
};

// 各Try呼出しはdiagnosticsを今回の結果へ置換する。false時、出力/保存原本は変更しない。
// 再分割用。時間/styleを評価せず、motionの削除・移植も行わない。
bool TryBuildGeometry(const std::vector<Cut>& cuts, Geometry& geometry, Diagnostics& diagnostics);
// 全保存値とBinding/IDの一対一対応を検証し、所有するconst snapshotを公開する。
bool TryCompile(const Asset& asset, std::shared_ptr<const CompiledTransition>& compiled, Diagnostics& diagnostics);
// resources/以下のUTF-8論理path、suffix .transition.jsonのみ。Loadは音声を読まない。
bool TryLoad(const std::string& logicalPath, std::shared_ptr<const CompiledTransition>& compiled, Diagnostics& diagnostics);
// 保存先directoryは既存であること。検証後に同directoryの排他的tempをAtomic Replaceする。
// directoryの用意、dirty/競合管理はcallerが所有する。
bool TrySave(const std::string& logicalPath, const Asset& asset, Diagnostics& diagnostics);
// phase内の秒を評価。finiteな範囲外はclamp。IO/Audio発火/Scene操作/積分を行わない。
bool TryEvaluate(const CompiledTransition& compiled, Phase phase, float localTime, FrameSample& sample, Diagnostics& diagnostics);
} // namespace SceneTransitionAsset
