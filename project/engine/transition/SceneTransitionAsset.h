// 役割: Scene遷移演出の保存値、切断幾何、変更不能snapshotと純粋な時刻評価を定義する。
#pragma once

#include "../math/Vector2.h"
#include "../math/Vector3.h"
#include "../math/Vector4.h"
#include <array>
#include <filesystem>
#include <string_view>
#include <variant>
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

// V1共通構築処理が完成させ、const共有所有で公開する。SceneやEditor draftの寿命に依存しない。
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
    friend struct CompiledTransitionV1Access;
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

// 幾何Build一回分の結果。CandidateRejectedは候補の幾何が不適合であることを示す。
enum class GeometryBuildStatus {
    Success,
    CandidateRejected,
    InvalidInput,
    CapacityExceeded,
    InternalFailure,
    AllocationFailure
};

// cutsの借用は呼出中のみ。呼出中はcutsを変更しない。時間/styleは評価しない。
// 成功時は値所有のgeometryへ置換し、diagnosticsは空。失敗時はgeometryを保持し、診断を一件返す。
// 診断の生成に失敗した場合や標準例外に該当しない例外は伝播する。
GeometryBuildStatus TryBuildGeometryDetailed(
    const std::vector<Cut>& cuts, Geometry& geometry, Diagnostics& diagnostics);

// 全保存値とBinding/IDの一対一対応を検証し、所有するconst snapshotを公開する。
bool TryCompile(const Asset& asset, std::shared_ptr<const CompiledTransition>& compiled, Diagnostics& diagnostics);
// resources/以下のUTF-8論理path、suffix .transition.jsonのみ。Loadは音声を読まない。
bool TryLoad(const std::string& logicalPath, std::shared_ptr<const CompiledTransition>& compiled, Diagnostics& diagnostics);
// 保存先directoryは既存であること。検証後に同directoryの排他的tempをAtomic Replaceする。
// directoryの用意、dirty/競合管理はcallerが所有する。
bool TrySave(const std::string& logicalPath, const Asset& asset, Diagnostics& diagnostics);
// phase内の秒を評価。finiteな範囲外はclamp。IO/Audio発火/Scene操作/積分を行わない。
bool TryEvaluate(const CompiledTransition& compiled, Phase phase, float localTime, FrameSample& sample, Diagnostics& diagnostics);
enum class LoadLinkedSeedMode { Fixed, PerRequest };
enum class LoadLinkedDirectionMode { Outward, Fixed };
enum class LoadLinkedTextMotion { Static, Pulse };
enum class LoadLinkedDefaultLanguage { Japanese, English };
struct LoadLinkedSeed {
    LoadLinkedSeedMode mode = LoadLinkedSeedMode::Fixed;
    std::uint32_t value = 0;
};
struct LoadLinkedCut { std::string id; Vector2 from{}, to{}; };
struct LoadLinkedAuthoredSource { std::vector<LoadLinkedCut> cuts; };
struct LoadLinkedRandomSource {
    std::uint32_t generationVersion = 0;
    LoadLinkedSeed seed;
    float angleStartDegrees = 0, angleSpanDegrees = 0;
    Vector2 pointMin{}, pointMax{};
    float minimumAngleSeparationDegrees = 0, minimumCutSeparation = 0;
};
using LoadLinkedCutSource = std::variant<LoadLinkedAuthoredSource, LoadLinkedRandomSource>;
struct LoadLinkedCutStyle {
    Easing sweepEasing = Easing::Linear;
    float afterglowDuration = 0;
    Vector4 lineColor{}, glowColor{};
    float lineWidth = 0, glowWidth = 0, glowIntensity = 0;
};
struct LoadLinkedCollapse {
    float duration = 0, launchSpeed = 0;
    LoadLinkedDirectionMode directionMode = LoadLinkedDirectionMode::Outward;
    Vector3 direction{};
    float fallAcceleration = 0;
    Vector3 angularVelocity{};
    float speedVariation = 0, directionVariationDegrees = 0;
    Vector3 rotationVariation{};
    float endFadeDuration = 0;
};
struct LoadLinkedText {
    bool enabled = false;
    std::string text;
    std::array<std::uint32_t, 2> referenceSize{}, layoutSize{};
    Vector2 position{}, pivot{};
    std::string fontFamily;
    float fontSize = 0;
    Vector4 color{};
    bool outlineEnabled = false;
    Vector4 outlineColor{};
    float outlineWidth = 0;
    LoadLinkedTextMotion motion = LoadLinkedTextMotion::Static;
    float pulsePeriod = 0, minimumOpacity = 0;
};
// 保存値だけを所有する。Runtime seedや生成Geometryを保存型へ含めない。
struct LoadLinkedAsset {
    std::string displayName;
    std::uint32_t minimumCuts = 0, maximumCuts = 0;
    float cutInterval = 0, cutSweepDuration = 0;
    LoadLinkedCutStyle cutStyle;
    LoadLinkedCutSource cutSource;
    LoadLinkedSeed motionSeed;
    LoadLinkedCollapse collapse;
    LoadLinkedText loadingText;
};
// 新規作成専用。未知languageはinvalid_argument、割当失敗は例外で伝える。
LoadLinkedAsset MakeDefaultLoadLinked(LoadLinkedDefaultLanguage language);
LoadLinkedRandomSource MakeDefaultLoadLinkedRandomSource();
// 不正draftも比較可能。NaNはbit一致、±0は同値。保存可能性の検査は別途必要。
bool EqualLoadLinkedSavedValues(const LoadLinkedAsset& a, const LoadLinkedAsset& b) noexcept;

enum class LoadLinkedDiagnosticCode {
    InvalidType, OutOfRange, UnknownField, MissingField, DuplicateField,
    InvalidUtf8, InvalidPath, UnsupportedVersion, InvalidGeometry,
    AttemptLimit, ResourceFailure, IoFailure, ExternalChange,
    Cancelled, InternalFailure
};
struct LoadLinkedDiagnostic {
    LoadLinkedDiagnosticCode code = LoadLinkedDiagnosticCode::InternalFailure;
    std::string field, detail;
};
using LoadLinkedDiagnostics = std::vector<LoadLinkedDiagnostic>;
using DecodedTransitionAsset = std::variant<
    std::shared_ptr<const CompiledTransition>, std::shared_ptr<const LoadLinkedAsset>>;
// 成功は診断空、失敗は最初の一件。診断割当失敗・非標準例外は伝播し得る。
// ValidateはIO/乱数を使わない。Decodeは同一bytesから構築し失敗時outを保持する。
bool TryValidateLoadLinked(const LoadLinkedAsset&, LoadLinkedDiagnostics&);
bool TryDecodeTransitionBytes(std::string_view bytes, DecodedTransitionAsset& out, LoadLinkedDiagnostics&);
// Runtime用。mainで捕捉したabsolute resources rootだけから一回読取してV1/V2を判別する。
// false/例外時out保持。診断割当失敗・非標準例外は伝播し得る。Editor context/保存baselineは不要。
bool TryReadTransitionFile(const std::filesystem::path& capturedResourcesRoot,
    const std::string& logicalPath, DecodedTransitionAsset& out, LoadLinkedDiagnostics&);

enum class EditorAssetProjectKind { Descriptor, Legacy };
class EditorAssetFileContext;
using EditorAssetContextPtr = std::shared_ptr<const EditorAssetFileContext>;
// 捕捉rootを検証して構築。false/例外時out保持、診断割当失敗・非標準例外は伝播し得る。
bool TryCreateEditorAssetFileContext(const std::filesystem::path& sourceRoot,
    std::uint64_t hostEpoch, EditorAssetContextPtr& out, LoadLinkedDiagnostics&);
// 捕捉rootとidentityを値所有する。hostはIO前にactive pointer/epochとPlay状態を検査する。
// 再接続時は新contextと増加epochを発行し、旧contextへ現在rootを再解決しない。
class EditorAssetFileContext final {
public:
    EditorAssetProjectKind Kind() const noexcept { return kind_; }
    std::uint64_t HostEpoch() const noexcept { return hostEpoch_; }
    const std::string& ProjectId() const noexcept { return projectId_; }
    const std::filesystem::path& DescriptorPath() const noexcept { return descriptorPath_; }
    const std::filesystem::path& SourceRoot() const noexcept { return sourceRoot_; }
    const std::filesystem::path& ResourcesRoot() const noexcept { return resourcesRoot_; }
    EditorAssetFileContext(const EditorAssetFileContext&) = delete;
    EditorAssetFileContext& operator=(const EditorAssetFileContext&) = delete;
private:
    EditorAssetFileContext() = default;
    EditorAssetProjectKind kind_ = EditorAssetProjectKind::Legacy;
    std::uint64_t hostEpoch_ = 0;
    std::string projectId_;
    std::filesystem::path descriptorPath_, sourceRoot_, resourcesRoot_;
    friend bool TryCreateEditorAssetFileContext(const std::filesystem::path&,
        std::uint64_t, EditorAssetContextPtr&, LoadLinkedDiagnostics&);
};

class LoadLinkedDiskSnapshot;
using LoadLinkedDiskSnapshotPtr = std::shared_ptr<const LoadLinkedDiskSnapshot>;
struct LoadLinkedMustNotExist {};
struct LoadLinkedMatchSnapshot { LoadLinkedDiskSnapshotPtr snapshot; };
using LoadLinkedWriteExpectation = std::variant<LoadLinkedMustNotExist, LoadLinkedMatchSnapshot>;
enum class LoadLinkedWriteStatus { NotWritten, Written, Indeterminate };
enum class LoadLinkedIoStage {
    None, Validate, Resolve, Read, Compare, Serialize, Allocate,
    CreateTemporary, WriteTemporary, FlushTemporary, CloseTemporary, Rename
};
struct LoadLinkedWriteResult {
    LoadLinkedWriteStatus status = LoadLinkedWriteStatus::NotWritten;
    LoadLinkedDiskSnapshotPtr snapshot; // Writtenだけ非null。
    LoadLinkedDiagnostics diagnostics; // 補助診断。割当失敗時は空になり得るのでscalarも参照する。
    LoadLinkedDiagnosticCode failureCode = LoadLinkedDiagnosticCode::InternalFailure; // Written時は参照しない。
    LoadLinkedIoStage failureStage = LoadLinkedIoStage::None;
    std::uint32_t systemError = 0; // その場で取得したWin32 error。0は該当なし。
};
// false/例外時out保持。診断割当失敗・非標準例外は伝播し得る。
bool TryReadLoadLinkedFile(const EditorAssetContextPtr&, const std::string& logicalPath,
    LoadLinkedDiskSnapshotPtr& out, LoadLinkedDiagnostics&);
// 全例外を捕捉しscalar診断を返す。renameの成功を確証できなければIndeterminate。
// 単独Writer前提。成功後は事前確保snapshotをmoveする。自動再試行はcallerも行わない。
LoadLinkedWriteResult WriteLoadLinkedFile(const EditorAssetContextPtr&, const std::string& logicalPath,
    const LoadLinkedAsset&, const LoadLinkedWriteExpectation&) noexcept;
// 値と正確なbytesを同じ読取/保存から所有する。getter参照はsnapshotの寿命内だけ有効。
class LoadLinkedDiskSnapshot final {
public:
    const std::shared_ptr<const LoadLinkedAsset>& Value() const noexcept { return value_; }
    const std::string& Bytes() const noexcept { return bytes_; }
    const std::string& LogicalPath() const noexcept { return logicalPath_; }
    const std::filesystem::path& TargetPath() const noexcept { return targetPath_; }
    const EditorAssetContextPtr& Context() const noexcept { return context_; }
    LoadLinkedDiskSnapshot(const LoadLinkedDiskSnapshot&) = delete;
    LoadLinkedDiskSnapshot& operator=(const LoadLinkedDiskSnapshot&) = delete;
private:
    LoadLinkedDiskSnapshot() = default;
    std::shared_ptr<const LoadLinkedAsset> value_;
    std::string bytes_, logicalPath_;
    std::filesystem::path targetPath_;
    EditorAssetContextPtr context_;
    friend bool TryReadLoadLinkedFile(const EditorAssetContextPtr&, const std::string&,
        LoadLinkedDiskSnapshotPtr&, LoadLinkedDiagnostics&);
    friend LoadLinkedWriteResult WriteLoadLinkedFile(const EditorAssetContextPtr&, const std::string&,
        const LoadLinkedAsset&, const LoadLinkedWriteExpectation&) noexcept;
};

} // namespace SceneTransitionAsset
