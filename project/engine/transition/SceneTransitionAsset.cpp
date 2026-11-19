// 役割: 遷移演出Assetの厳密なJSON IO、決定的な凸分割と副作用のない時間評価を実装する。
#include "SceneTransitionAsset.h"
#include "../math/Math.h"
#include "../utility/StringUtility.h"
#include "../utility/EditableResourcePath.h"
#include "../project/ProjectDescriptor.h"
#include "../../externals/nlohmann/json.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <locale>
#include <new>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <type_traits>

namespace SceneTransitionAsset {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
constexpr double DistanceTolerance = 1e-6, AreaTolerance = 1e-10, AreaSumTolerance = 1e-5;
struct Failure {
    Diagnostic diagnostic;
    GeometryBuildStatus geometryStatus = GeometryBuildStatus::InternalFailure;
};
[[noreturn]] void Fail(const std::string& field, const std::string& message) { throw Failure{{field, message}}; }
void Require(bool condition, const std::string& field, const std::string& message) {
    if (!condition) Fail(field, message);
}
template<class F> bool Guard(Diagnostics& diagnostics, F&& operation) {
    diagnostics.clear();
    try { operation(); return true; }
    catch (const Failure& error) { diagnostics.push_back(error.diagnostic); }
    catch (const std::exception& error) { diagnostics.push_back({"asset", error.what()}); }
    return false;
}
std::string At(const std::string& field, std::size_t index) { return field + "[" + std::to_string(index) + "]"; }
void Range(float value, double low, double high, const std::string& field, bool exclusiveLow = false) {
    Require(std::isfinite(value) && (exclusiveLow ? value > low : value >= low) && value <= high,
        field, "有限値かつ許可範囲内である必要があります");
}
void Utf8(const std::string& value, std::size_t limit, const std::string& field) {
    Require(value.size() <= limit, field, "UTF-8 byte上限を超えています");
    if (!value.empty()) Require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0) > 0, field, "UTF-8が不正です");
}
void Id(const std::string& value, const std::string& field) {
    Require(!value.empty() && value.size() <= 64, field, "idは1〜64byteです");
    for (unsigned char c : value) Require((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-', field, "idにはASCII英数字、_、-だけを使えます");
}
void CheckPhase(Phase phase, const std::string& field) {
    Require(phase == Phase::Cutting || phase == Phase::Collapse, field, "未知のphaseです");
}
void CheckEasing(Easing easing, const std::string& field) {
    switch (easing) {
    case Easing::Linear: case Easing::EaseIn: case Easing::EaseOut: case Easing::EaseInOut: case Easing::SmoothStep: return;
    default: Fail(field, "未知のeasingです");
    }
}
void Color(Vector4 value, const std::string& field) {
    Range(value.x, 0, 1, field + "[0]"); Range(value.y, 0, 1, field + "[1]");
    Range(value.z, 0, 1, field + "[2]"); Range(value.w, 0, 1, field + "[3]");
}
void VecRange(Vector3 value, double low, double high, const std::string& field) {
    Range(value.x, low, high, field + "[0]"); Range(value.y, low, high, field + "[1]"); Range(value.z, low, high, field + "[2]");
}
fs::path LogicalPath(const std::string& value, const std::string& field, bool assetPath) {
    Utf8(value, 4096, field);
    Require(!value.empty(), field, "pathが空です");
    for (unsigned char c : value) Require(c >= 32 && c != 127 && c != ':', field, "driveまたは制御文字を含むpathは使えません");
    const fs::path path = StringUtility::ToPath(value);
    Require(!path.has_root_path(), field, "絶対pathは使えません");
    for (const auto& part : path) {
        const auto& name = part.native();
        Require(part != L"..", field, "親directory移動は使えません");
        if (part == L".") continue;
        // Win32の末尾除去やdevice別名で、検証したpathと実際の保存先が食い違わないようにする。
        Require(!name.empty() && name.back() != L'.' && name.back() != L' ' && name.find_first_of(L"<>\"|?*") == std::wstring::npos,
            field, "Win32で別名または無効名になるpath成分は使えません");
        const auto stem = name.substr(0, name.find(L'.'));
        bool device = false;
        for (const wchar_t* reserved : {L"CON", L"PRN", L"AUX", L"NUL"})
            device |= CompareStringOrdinal(stem.c_str(), -1, reserved, -1, TRUE) == CSTR_EQUAL;
        if (stem.size() == 4 && stem[3] >= L'1' && stem[3] <= L'9') {
            const auto prefix = stem.substr(0, 3);
            device |= CompareStringOrdinal(prefix.c_str(), -1, L"COM", -1, TRUE) == CSTR_EQUAL ||
                CompareStringOrdinal(prefix.c_str(), -1, L"LPT", -1, TRUE) == CSTR_EQUAL;
        }
        Require(!device, field, "device名はresource pathに使えません");
    }
    const auto normalized = path.lexically_normal();
    Require(normalized.begin() != normalized.end() && *normalized.begin() == L"resources" &&
        normalized != fs::path(L"resources"), field, "resources/以下のpathが必要です");
    if (assetPath) {
        const auto name = StringUtility::ToUtf8(normalized.filename());
        Require(name.ends_with(".transition.json"), field, "suffixは.transition.jsonです");
    }
    return normalized;
}
bool Inside(const fs::path& root, const fs::path& target) {
    auto a = root.begin(), b = target.begin();
    for (; a != root.end(); ++a, ++b) {
        if (b == target.end()) return false;
        const auto& x = a->native(); const auto& y = b->native();
        if (CompareStringOrdinal(x.c_str(), static_cast<int>(x.size()), y.c_str(), static_cast<int>(y.size()), TRUE) != CSTR_EQUAL) return false;
    }
    return b != target.end();
}
fs::path ResolveAsset(const std::string& logicalPath) {
    const auto logical = LogicalPath(logicalPath, "path", true);
    const auto root = fs::weakly_canonical(EditableResourcePath::ResolveResource(fs::path(L"resources")));
    const auto target = fs::weakly_canonical(EditableResourcePath::ResolveResource(logical));
    Require(Inside(root, target), "path", "解決後のpathがresources境界外です");
    return target;
}

struct Point { double x, y; };
struct Plane { double x, y, c; };
struct Polygon { std::uint16_t id = 0; std::vector<Point> points; };
double Cross(Point a, Point b) { return a.x * b.y - a.y * b.x; }
Point Sub(Point a, Point b) { return {a.x - b.x, a.y - b.y}; }
double Distance(Plane line, Point p) {
    const double d = line.x * p.x + line.y * p.y + line.c;
    return std::abs(d) <= DistanceTolerance ? 0 : d;
}
bool Near(Point a, Point b) { return std::hypot(a.x - b.x, a.y - b.y) <= DistanceTolerance; }
void Clean(std::vector<Point>& points) {
    std::vector<Point> unique;
    for (Point p : points) if (unique.empty() || !Near(unique.back(), p)) unique.push_back(p);
    if (unique.size() > 1 && Near(unique.front(), unique.back())) unique.pop_back();
    bool changed = true;
    while (changed && unique.size() >= 3) {
        changed = false;
        for (std::size_t i = 0; i < unique.size(); ++i) {
            const Point a = unique[(i + unique.size() - 1) % unique.size()], b = unique[i], c = unique[(i + 1) % unique.size()];
            const Point ac = Sub(c, a), ab = Sub(b, a);
            const double length = std::hypot(ac.x, ac.y);
            const double projection = ab.x * ac.x + ab.y * ac.y;
            if (length > 0 && std::abs(Cross(ac, ab)) / length <= DistanceTolerance &&
                projection >= 0 && projection <= length * length) {
                unique.erase(unique.begin() + static_cast<std::ptrdiff_t>(i)); changed = true; break;
            }
        }
    }
    points = std::move(unique);
}
double Area(const std::vector<Point>& polygon) {
    double twice = 0;
    for (std::size_t i = 0; i < polygon.size(); ++i) twice += Cross(polygon[i], polygon[(i + 1) % polygon.size()]);
    return twice * 0.5;
}
std::vector<Point> Clip(const std::vector<Point>& polygon, Plane plane, bool left) {
    std::vector<Point> result;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Point a = polygon[i], b = polygon[(i + 1) % polygon.size()];
        const double da = Distance(plane, a), db = Distance(plane, b);
        const bool inA = left ? da >= 0 : da <= 0, inB = left ? db >= 0 : db <= 0;
        if (inA) result.push_back(a);
        if (inA != inB) {
            // 両half-planeで同じ式を使い、共有する切断境界の隙間を防ぐ。
            const double t = da / (da - db);
            result.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t});
        }
    }
    Clean(result);
    return result;
}
Vector2 ToVector(Point p) { return {static_cast<float>(p.x), static_cast<float>(p.y)}; }
std::string Binding(const std::vector<Cut>& cuts) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "geometry-v1";
    for (std::size_t i = 0; i < cuts.size(); ++i) {
        out << '|' << std::dec << i << ':' << cuts[i].id << ':';
        for (float value : {cuts[i].from.x, cuts[i].from.y, cuts[i].to.x, cuts[i].to.y}) {
            if (value == 0) value = 0; // -0と+0は同一幾何。
            out << std::hex << std::setw(8) << std::setfill('0') << std::bit_cast<std::uint32_t>(value) << ',';
        }
    }
    return out.str();
}
void RequireGeometry(bool condition, GeometryBuildStatus status,
    const std::string& field, const std::string& message) {
    if (!condition) throw Failure{{field, message}, status};
}

void GeometryId(const std::string& value, const std::string& field) {
    try {
        Id(value, field);
    } catch (Failure& error) {
        error.geometryStatus = GeometryBuildStatus::InvalidInput;
        throw;
    }
}

Geometry Build(const std::vector<Cut>& cuts) {
    RequireGeometry(!cuts.empty() && cuts.size() <= MaxCuts,
        GeometryBuildStatus::InvalidInput, "cuts", "cut数は1〜16です");
    const std::vector<Point> rectangle{{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    std::vector<Polygon> polygons{{0, rectangle}};
    std::vector<Plane> planes;
    std::set<std::string> ids;
    Geometry geometry;
    for (std::size_t i = 0; i < cuts.size(); ++i) {
        const auto& cut = cuts[i]; const auto field = At("cuts", i);
        GeometryId(cut.id, field + ".id");
        RequireGeometry(ids.insert(cut.id).second,
            GeometryBuildStatus::InvalidInput, field + ".id", "idが重複しています");
        const Point a{cut.from.x, cut.from.y}, b{cut.to.x, cut.to.y};
        RequireGeometry(std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(b.x) && std::isfinite(b.y),
            GeometryBuildStatus::InvalidInput, field, "切断座標は有限値が必要です");
        const Point direction = Sub(b, a); const double length = std::hypot(direction.x, direction.y);
        RequireGeometry(length > 0,
            GeometryBuildStatus::CandidateRejected, field, "長さ0の切断線です");
        const Plane plane{-direction.y / length, direction.x / length,
            (direction.y * a.x - direction.x * a.y) / length};
        for (Plane previous : planes) {
            const bool same = std::hypot(plane.x - previous.x, plane.y - previous.y) <= DistanceTolerance && std::abs(plane.c - previous.c) <= DistanceTolerance;
            const bool reverse = std::hypot(plane.x + previous.x, plane.y + previous.y) <= DistanceTolerance && std::abs(plane.c + previous.c) <= DistanceTolerance;
            RequireGeometry(!same && !reverse,
                GeometryBuildStatus::CandidateRejected, field, "同一または反転した切断線が重複しています");
        }
        planes.push_back(plane);
        std::vector<Point> intersections;
        for (std::size_t edge = 0; edge < rectangle.size(); ++edge) {
            const Point p = rectangle[edge], q = rectangle[(edge + 1) % rectangle.size()];
            const double dp = Distance(plane, p), dq = Distance(plane, q);
            if (dp == 0) intersections.push_back(p);
            if ((dp < 0 && dq > 0) || (dp > 0 && dq < 0)) {
                const double t = dp / (dp - dq);
                intersections.push_back({p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t});
            }
        }
        std::sort(intersections.begin(), intersections.end(), [&](Point p, Point q) {
            return p.x * direction.x + p.y * direction.y < q.x * direction.x + q.y * direction.y;
        });
        RequireGeometry(intersections.size() >= 2 && !Near(intersections.front(), intersections.back()),
            GeometryBuildStatus::CandidateRejected, field, "画面を横切る区間がありません");
        geometry.cutSegments.push_back({ToVector(intersections.front()), ToVector(intersections.back())});
        std::vector<Polygon> next;
        std::size_t splits = 0;
        const auto bit = static_cast<std::uint16_t>(1u << i);
        for (const auto& polygon : polygons) {
            bool left = false, right = false;
            for (Point p : polygon.points) { const double d = Distance(plane, p); left |= d > 0; right |= d < 0; }
            RequireGeometry(left || right,
                GeometryBuildStatus::CandidateRejected, field, "許容差内で退化したpolygonです");
            if (left && right) {
                auto l = Clip(polygon.points, plane, true), r = Clip(polygon.points, plane, false);
                RequireGeometry(l.size() >= 3 && r.size() >= 3 && Area(l) > AreaTolerance && Area(r) > AreaTolerance,
                    GeometryBuildStatus::CandidateRejected, field, "切断で退化したpolygonが生成されます");
                next.push_back({static_cast<std::uint16_t>(polygon.id | bit), std::move(l)});
                next.push_back({polygon.id, std::move(r)}); ++splits;
            } else next.push_back({static_cast<std::uint16_t>(polygon.id | (left ? bit : 0)), polygon.points});
            RequireGeometry(next.size() <= MaxFragments,
                GeometryBuildStatus::CapacityExceeded, field, "断片数上限を超えています");
        }
        RequireGeometry(splits > 0,
            GeometryBuildStatus::CandidateRejected, field, "境界接触のみ、または断片を増やさないcutです");
        polygons = std::move(next);
    }
    std::sort(polygons.begin(), polygons.end(), [](const Polygon& a, const Polygon& b) { return a.id < b.id; });
    double totalArea = 0;
    for (const auto& polygon : polygons) {
        GeometryFragment fragment; fragment.fragmentId = polygon.id;
        // float出力後もtriangleの退化と向きを検証する。
        std::vector<Point> output;
        for (Point p : polygon.points) { const Vector2 v = ToVector(p); output.push_back({v.x, v.y}); fragment.polygon.push_back(v); }
        const double area = Area(output);
        const bool finiteArea = std::isfinite(area);
        RequireGeometry(finiteArea && area > AreaTolerance,
            finiteArea ? GeometryBuildStatus::CandidateRejected : GeometryBuildStatus::InternalFailure,
            "geometry", "出力polygonの面積が退化しています");
        Point center{0, 0};
        for (std::size_t i = 0; i < output.size(); ++i) {
            const Point p = output[i], q = output[(i + 1) % output.size()]; const double weight = Cross(p, q);
            center.x += (p.x + q.x) * weight; center.y += (p.y + q.y) * weight;
        }
        fragment.centroid = ToVector({center.x / (6 * area), center.y / (6 * area)});
        fragment.area = static_cast<float>(area); totalArea += area;
        fragment.firstVertex = geometry.vertices.size(); fragment.firstIndex = geometry.indices.size();
        RequireGeometry(geometry.vertices.size() + output.size() <= MaxVertices &&
            geometry.indices.size() + 3 * (output.size() - 2) <= MaxIndices,
            GeometryBuildStatus::CapacityExceeded, "geometry", "頂点/index容量を超えています");
        for (Vector2 uv : fragment.polygon) geometry.vertices.push_back({uv, polygon.id});
        for (std::size_t i = 1; i + 1 < output.size(); ++i) {
            RequireGeometry(Cross(Sub(output[i], output[0]), Sub(output[i + 1], output[0])) * 0.5 > AreaTolerance,
                GeometryBuildStatus::CandidateRejected, "geometry", "triangleの面積または向きが不正です");
            for (std::size_t local : {std::size_t{0}, i, i + 1}) {
                const auto index = fragment.firstVertex + local;
                RequireGeometry(index < geometry.vertices.size(),
                    GeometryBuildStatus::InternalFailure, "geometry", "indexが頂点範囲外です");
                geometry.indices.push_back(static_cast<std::uint32_t>(index));
            }
        }
        fragment.indexCount = geometry.indices.size() - fragment.firstIndex;
        geometry.fragments.push_back(std::move(fragment));
    }
    const bool validAreaSum = std::abs(totalArea - 1) <= AreaSumTolerance;
    RequireGeometry(validAreaSum && geometry.indices.size() / 3 <= MaxTriangles,
        validAreaSum ? GeometryBuildStatus::CapacityExceeded : GeometryBuildStatus::InternalFailure,
        "geometry", "面積合計またはtriangle容量が不正です");
    geometry.binding = Binding(cuts);
    return geometry;
}

float Duration(const Asset& asset, Phase phase) { return phase == Phase::Cutting ? asset.cutPhaseDuration : asset.collapseDuration; }
struct CutTimes { float sweepEnd, afterglowEnd; };
CutTimes ResolveCutTimes(const Cut& cut) {
    // 保存時刻のfloat精度へ各終端を一回だけ丸め、検証と描画sampleの境界を揃える。
    // double合計をそのまま比較すると、0.2f+0.8fを全体1秒の範囲外と誤判定する。
    const double sweepEnd = static_cast<double>(cut.startTime) + cut.sweepDuration;
    return {static_cast<float>(sweepEnd), static_cast<float>(sweepEnd + cut.afterglowDuration)};
}
void Validate(const Asset& asset, const Geometry& geometry) {
    // IOを通らないEditor draftも同じ契約で検証し、snapshot公開前にIDと全timelineを固定する。
    Utf8(asset.displayName, 1024, "displayName");
    Range(asset.cutPhaseDuration, 0, 3600, "cutPhaseDuration", true);
    Range(asset.collapseDuration, 0, 3600, "collapseDuration", true);
    Require(asset.geometryBinding == geometry.binding, "geometryBinding", "形状Bindingが一致しません。明示的な再分割が必要です");
    for (std::size_t i = 0; i < asset.cuts.size(); ++i) {
        const auto& cut = asset.cuts[i]; const auto field = At("cuts", i);
        Range(cut.startTime, 0, asset.cutPhaseDuration, field + ".startTime");
        Range(cut.sweepDuration, 0, asset.cutPhaseDuration, field + ".sweepDuration", true);
        Range(cut.afterglowDuration, 0, asset.cutPhaseDuration, field + ".afterglowDuration");
        Require(ResolveCutTimes(cut).afterglowEnd <= asset.cutPhaseDuration,
            field, "sweepと残光がCutting終端を超えています");
        CheckEasing(cut.sweepEasing, field + ".sweepEasing");
        Color(cut.lineColor, field + ".lineColor"); Color(cut.glowColor, field + ".glowColor");
        Range(cut.lineWidth, 0, 1, field + ".lineWidth", true); Range(cut.glowWidth, 0, 1, field + ".glowWidth", true);
        Range(cut.glowIntensity, 0, 32, field + ".glowIntensity");
        if (i == 0) Require(cut.startTime == 0 && cut.from.y == cut.to.y && cut.from.x < cut.to.x &&
            geometry.cutSegments[0].from.x == 0 && geometry.cutSegments[0].to.x == 1,
            field, "第一cutは時刻0、左から右へ画面を横切る水平線が必要です");
        else Require(cut.startTime >= asset.cuts[0].sweepDuration, field + ".startTime", "第一sweep終了前に後続cutは開始できません");
    }
    Require(asset.fragments.size() == geometry.fragments.size(), "fragments", "全生成断片に一件ずつmotionが必要です");
    std::set<std::uint16_t> fragmentIds;
    std::size_t totalKeys = 0;
    for (std::size_t i = 0; i < asset.fragments.size(); ++i) {
        const auto& motion = asset.fragments[i]; const auto field = At("fragments", i);
        Require(fragmentIds.insert(motion.fragmentId).second, field + ".fragmentId", "fragmentIdが重複しています");
        const auto found = std::find_if(geometry.fragments.begin(), geometry.fragments.end(), [&](const auto& fragment) { return fragment.fragmentId == motion.fragmentId; });
        Require(found != geometry.fragments.end(), field + ".fragmentId", "生成幾何にないfragmentIdです");
        Require(!motion.keys.empty() && motion.keys.size() <= MaxPoseKeysPerFragment, field + ".keys", "姿勢key数は1〜256です");
        totalKeys += motion.keys.size(); Require(totalKeys <= MaxPoseKeys, "fragments", "総姿勢key上限を超えています");
        for (std::size_t k = 0; k < motion.keys.size(); ++k) {
            const auto& key = motion.keys[k]; const auto keyField = At(field + ".keys", k);
            Range(key.time, 0, asset.collapseDuration, keyField + ".time");
            if (k > 0) Require(key.time > motion.keys[k - 1].time, keyField + ".time", "float化後の時刻も厳密昇順が必要です");
            VecRange(key.position, -10000, 10000, keyField + ".position");
            VecRange(key.rotationEulerDegrees, -360000, 360000, keyField + ".rotationEulerDegrees");
            VecRange(key.scale, 0, 1000, keyField + ".scale"); CheckEasing(key.easingToNext, keyField + ".easingToNext");
        }
        const auto& first = motion.keys.front();
        Require(first.time == 0 && first.position.x == 0 && first.position.y == 0 && first.position.z == 0 &&
            first.rotationEulerDegrees.x == 0 && first.rotationEulerDegrees.y == 0 && first.rotationEulerDegrees.z == 0 &&
            first.scale.x == 1 && first.scale.y == 1 && first.scale.z == 1, field + ".keys[0]", "初期keyは時刻0、移動0、回転0、拡縮1です");
    }
    Require(asset.flashKeys.size() <= MaxFlashKeys, "flashKeys", "Flash key上限を超えています");
    std::array<const FlashKey*, 2> firstFlash{}, lastFlash{};
    bool collapseSeen = false;
    for (std::size_t i = 0; i < asset.flashKeys.size(); ++i) {
        const auto& key = asset.flashKeys[i]; const auto field = At("flashKeys", i);
        CheckPhase(key.phase, field + ".phase");
        const std::size_t phase = key.phase == Phase::Cutting ? 0 : 1;
        Require(!(collapseSeen && phase == 0), field + ".phase", "Flash配列はCutting→Collapse順です"); collapseSeen |= phase == 1;
        Range(key.time, 0, Duration(asset, key.phase), field + ".time");
        Color(key.color, field + ".color"); Range(key.intensity, 0, 32, field + ".intensity"); CheckEasing(key.easingToNext, field + ".easingToNext");
        if (!firstFlash[phase]) firstFlash[phase] = &key;
        if (lastFlash[phase]) Require(key.time > lastFlash[phase]->time, field + ".time", "Flash時刻はphaseごとに厳密昇順です");
        lastFlash[phase] = &key;
    }
    for (std::size_t i = 0; i < 2; ++i) if (firstFlash[i]) Require(firstFlash[i]->time == 0 &&
        lastFlash[i]->time == Duration(asset, i == 0 ? Phase::Cutting : Phase::Collapse) && lastFlash[i]->intensity == 0,
        "flashKeys", "非空phaseには時刻0と終端key、終端intensity=0が必要です");
    Require(asset.audioCues.size() <= MaxAudioCues, "audioCues", "Audio Cue上限を超えています");
    std::set<std::string> audioIds;
    for (std::size_t i = 0; i < asset.audioCues.size(); ++i) {
        const auto& cue = asset.audioCues[i]; const auto field = At("audioCues", i);
        Id(cue.id, field + ".id"); Require(audioIds.insert(cue.id).second, field + ".id", "Audio Cue idが重複しています");
        CheckPhase(cue.phase, field + ".phase"); Range(cue.time, 0, Duration(asset, cue.phase), field + ".time");
        if (!cue.clipPath.empty()) LogicalPath(cue.clipPath, field + ".clipPath", false);
        Range(cue.volume, 0, 1, field + ".volume"); Range(cue.pitch, 0, 2, field + ".pitch", true);
    }
}

void Fields(const Json& object, std::initializer_list<const char*> names, const std::string& field) {
    Require(object.is_object(), field, "objectが必要です");
    for (const char* name : names) Require(object.contains(name), field + "." + name, "必須項目がありません");
    for (auto it = object.begin(); it != object.end(); ++it) Require(std::find_if(names.begin(), names.end(),
        [&](const char* name) { return it.key() == name; }) != names.end(), field + "." + it.key(), "未知項目です");
}
const Json& Array(const Json& value, std::size_t limit, const std::string& field) {
    Require(value.is_array() && value.size() <= limit, field, "配列型または件数上限に違反しています"); return value;
}
float Number(const Json& value, const std::string& field,
    double low = -(std::numeric_limits<float>::max)(), double high = (std::numeric_limits<float>::max)(), bool exclusiveLow = false) {
    Require(value.is_number(), field, "数値が必要です");
    const double number = value.get<double>();
    Require(std::isfinite(number) && (exclusiveLow ? number > low : number >= low) && number <= high,
        field, "数値が許可範囲外です");
    const auto converted = static_cast<float>(number);
    // 保存値はfloat。元doubleの範囲検証後、端点もfloatへ揃えて丸めだけによる拒否を防ぐ。
    Range(converted, static_cast<float>(low), static_cast<float>(high), field, exclusiveLow);
    return converted;
}
std::uint16_t Integer(const Json& value, const std::string& field) {
    Require(value.is_number_integer(), field, "整数が必要です");
    const double number = value.get<double>(); Require(number >= 0 && number <= 65535, field, "uint16範囲外です");
    return static_cast<std::uint16_t>(number);
}
std::string Text(const Json& value, std::size_t limit, const std::string& field) {
    Require(value.is_string(), field, "文字列が必要です"); auto text = value.get<std::string>(); Utf8(text, limit, field); return text;
}
template<std::size_t N> std::array<float, N> Components(const Json& value, const std::string& field,
    double low = -(std::numeric_limits<float>::max)(), double high = (std::numeric_limits<float>::max)()) {
    Array(value, N, field); Require(value.size() == N, field, "成分数が違います");
    std::array<float, N> result{}; for (std::size_t i = 0; i < N; ++i) result[i] = Number(value[i], At(field, i), low, high); return result;
}
Vector2 Read2(const Json& value, const std::string& field) { const auto v = Components<2>(value, field); return {v[0], v[1]}; }
Vector3 Read3(const Json& value, const std::string& field, double low, double high) { const auto v = Components<3>(value, field, low, high); return {v[0], v[1], v[2]}; }
Vector4 Read4(const Json& value, const std::string& field) { const auto v = Components<4>(value, field, 0, 1); return {v[0], v[1], v[2], v[3]}; }
Easing ReadEasing(const Json& value, const std::string& field) {
    const auto name = Text(value, 64, field);
    if (name == "Linear") return Easing::Linear; if (name == "EaseIn") return Easing::EaseIn;
    if (name == "EaseOut") return Easing::EaseOut; if (name == "EaseInOut") return Easing::EaseInOut;
    if (name == "SmoothStep") return Easing::SmoothStep; Fail(field, "未知のeasingです");
}
Phase ReadPhase(const Json& value, const std::string& field) {
    const auto name = Text(value, 64, field);
    if (name == "Cutting") return Phase::Cutting; if (name == "Collapse") return Phase::Collapse; Fail(field, "未知のphaseです");
}
Asset ReadAsset(const Json& root) {
    Fields(root, {"version", "effect", "displayName", "cutPhaseDuration", "collapseDuration", "cuts", "geometryBinding", "fragments", "flashKeys", "audioCues"}, "asset");
    Require(Integer(root.at("version"), "version") == 1, "version", "対応versionは1です");
    Require(Text(root.at("effect"), 64, "effect") == "SwordShatter", "effect", "対応effectはSwordShatterです");
    Asset asset;
    asset.displayName = Text(root.at("displayName"), 1024, "displayName");
    asset.cutPhaseDuration = Number(root.at("cutPhaseDuration"), "cutPhaseDuration", 0, 3600, true);
    asset.collapseDuration = Number(root.at("collapseDuration"), "collapseDuration", 0, 3600, true);
    const double cutDuration = root.at("cutPhaseDuration").get<double>();
    const double collapseDuration = root.at("collapseDuration").get<double>();
    const auto inputDuration = [&](Phase phase) { return phase == Phase::Cutting ? cutDuration : collapseDuration; };
    asset.geometryBinding = Text(root.at("geometryBinding"), MaxJsonBytes, "geometryBinding");
    const auto& cuts = Array(root.at("cuts"), MaxCuts, "cuts");
    for (std::size_t i = 0; i < cuts.size(); ++i) {
        const auto& j = cuts[i]; const auto f = At("cuts", i);
        Fields(j, {"id", "from", "to", "startTime", "sweepDuration", "sweepEasing", "afterglowDuration", "lineColor", "lineWidth", "glowColor", "glowWidth", "glowIntensity"}, f);
        Cut cut; cut.id = Text(j.at("id"), 64, f + ".id"); cut.from = Read2(j.at("from"), f + ".from"); cut.to = Read2(j.at("to"), f + ".to");
        cut.startTime = Number(j.at("startTime"), f + ".startTime", 0, cutDuration);
        cut.sweepDuration = Number(j.at("sweepDuration"), f + ".sweepDuration", 0, cutDuration, true);
        cut.sweepEasing = ReadEasing(j.at("sweepEasing"), f + ".sweepEasing");
        cut.afterglowDuration = Number(j.at("afterglowDuration"), f + ".afterglowDuration", 0, cutDuration);
        cut.lineColor = Read4(j.at("lineColor"), f + ".lineColor"); cut.lineWidth = Number(j.at("lineWidth"), f + ".lineWidth", 0, 1, true);
        cut.glowColor = Read4(j.at("glowColor"), f + ".glowColor"); cut.glowWidth = Number(j.at("glowWidth"), f + ".glowWidth", 0, 1, true);
        cut.glowIntensity = Number(j.at("glowIntensity"), f + ".glowIntensity", 0, 32); asset.cuts.push_back(std::move(cut));
    }
    const auto& fragments = Array(root.at("fragments"), MaxFragments, "fragments");
    std::size_t totalKeys = 0;
    for (std::size_t i = 0; i < fragments.size(); ++i) {
        const auto& j = fragments[i]; const auto f = At("fragments", i); Fields(j, {"fragmentId", "keys"}, f);
        FragmentMotion motion; motion.fragmentId = Integer(j.at("fragmentId"), f + ".fragmentId");
        const auto& keys = Array(j.at("keys"), MaxPoseKeysPerFragment, f + ".keys");
        totalKeys += keys.size(); Require(totalKeys <= MaxPoseKeys, f + ".keys", "総姿勢key上限を超えています");
        for (std::size_t k = 0; k < keys.size(); ++k) {
            const auto& v = keys[k]; const auto p = At(f + ".keys", k);
            Fields(v, {"time", "position", "rotationEulerDegrees", "scale", "easingToNext"}, p);
            motion.keys.push_back({Number(v.at("time"), p + ".time", 0, collapseDuration), Read3(v.at("position"), p + ".position", -10000, 10000),
                Read3(v.at("rotationEulerDegrees"), p + ".rotationEulerDegrees", -360000, 360000), Read3(v.at("scale"), p + ".scale", 0, 1000), ReadEasing(v.at("easingToNext"), p + ".easingToNext")});
        }
        asset.fragments.push_back(std::move(motion));
    }
    const auto& flashes = Array(root.at("flashKeys"), MaxFlashKeys, "flashKeys");
    for (std::size_t i = 0; i < flashes.size(); ++i) {
        const auto& j = flashes[i]; const auto f = At("flashKeys", i);
        Fields(j, {"phase", "time", "color", "intensity", "easingToNext"}, f);
        const auto phase = ReadPhase(j.at("phase"), f + ".phase");
        asset.flashKeys.push_back({phase, Number(j.at("time"), f + ".time", 0, inputDuration(phase)),
            Read4(j.at("color"), f + ".color"), Number(j.at("intensity"), f + ".intensity", 0, 32), ReadEasing(j.at("easingToNext"), f + ".easingToNext")});
    }
    const auto& audio = Array(root.at("audioCues"), MaxAudioCues, "audioCues");
    for (std::size_t i = 0; i < audio.size(); ++i) {
        const auto& j = audio[i]; const auto f = At("audioCues", i); Fields(j, {"id", "phase", "time", "clipPath", "volume", "pitch"}, f);
        const auto phase = ReadPhase(j.at("phase"), f + ".phase");
        asset.audioCues.push_back({Text(j.at("id"), 64, f + ".id"), phase,
            Number(j.at("time"), f + ".time", 0, inputDuration(phase)), Text(j.at("clipPath"), 4096, f + ".clipPath"),
            Number(j.at("volume"), f + ".volume", 0, 1), Number(j.at("pitch"), f + ".pitch", 0, 2, true)});
    }
    return asset;
}

const char* Name(Easing easing) {
    switch (easing) {
    case Easing::Linear: return "Linear"; case Easing::EaseIn: return "EaseIn";
    case Easing::EaseOut: return "EaseOut"; case Easing::EaseInOut: return "EaseInOut";
    case Easing::SmoothStep: return "SmoothStep"; default: Fail("easing", "未知のeasingです");
    }
}
const char* Name(Phase phase) { CheckPhase(phase, "phase"); return phase == Phase::Cutting ? "Cutting" : "Collapse"; }
Json Write2(Vector2 v) { return Json::array({v.x, v.y}); }
Json Write3(Vector3 v) { return Json::array({v.x, v.y, v.z}); }
Json Write4(Vector4 v) { return Json::array({v.x, v.y, v.z, v.w}); }
Json WriteAsset(const Asset& asset) {
    Json root{{"version", 1}, {"effect", "SwordShatter"}, {"displayName", asset.displayName},
        {"cutPhaseDuration", asset.cutPhaseDuration}, {"collapseDuration", asset.collapseDuration},
        {"geometryBinding", asset.geometryBinding}, {"cuts", Json::array()}, {"fragments", Json::array()},
        {"flashKeys", Json::array()}, {"audioCues", Json::array()}};
    for (const auto& cut : asset.cuts) root["cuts"].push_back({{"id", cut.id}, {"from", Write2(cut.from)}, {"to", Write2(cut.to)},
        {"startTime", cut.startTime}, {"sweepDuration", cut.sweepDuration}, {"sweepEasing", Name(cut.sweepEasing)},
        {"afterglowDuration", cut.afterglowDuration}, {"lineColor", Write4(cut.lineColor)}, {"lineWidth", cut.lineWidth},
        {"glowColor", Write4(cut.glowColor)}, {"glowWidth", cut.glowWidth}, {"glowIntensity", cut.glowIntensity}});
    for (const auto& motion : asset.fragments) {
        Json fragment{{"fragmentId", motion.fragmentId}, {"keys", Json::array()}};
        for (const auto& key : motion.keys) fragment["keys"].push_back({{"time", key.time}, {"position", Write3(key.position)},
            {"rotationEulerDegrees", Write3(key.rotationEulerDegrees)}, {"scale", Write3(key.scale)}, {"easingToNext", Name(key.easingToNext)}});
        root["fragments"].push_back(std::move(fragment));
    }
    for (const auto& key : asset.flashKeys) root["flashKeys"].push_back({{"phase", Name(key.phase)}, {"time", key.time},
        {"color", Write4(key.color)}, {"intensity", key.intensity}, {"easingToNext", Name(key.easingToNext)}});
    for (const auto& cue : asset.audioCues) root["audioCues"].push_back({{"id", cue.id}, {"phase", Name(cue.phase)},
        {"time", cue.time}, {"clipPath", cue.clipPath}, {"volume", cue.volume}, {"pitch", cue.pitch}});
    return root;
}
Asset ReadFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    Require(input.is_open(), "path", "Assetを開けません");
    std::string bytes(MaxJsonBytes + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    Require(!input.bad() && (input.eof() || !input.fail()), "path", "Assetの読込に失敗しました");
    const auto count = input.gcount();
    Require(count >= 0 && static_cast<std::size_t>(count) <= MaxJsonBytes, "asset", "JSON byte上限を超えています");
    bytes.resize(static_cast<std::size_t>(count)); Utf8(bytes, MaxJsonBytes, "asset");
    // parserはobjectをmap化して重複keyを失うため、受理前にobjectごとのkey集合を照合する。
    std::vector<std::set<std::string>> objectKeys;
    const auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
        Require(depth <= MaxJsonDepth, "asset", "JSON階層上限を超えています");
        if (event == Json::parse_event_t::object_start) objectKeys.emplace_back();
        else if (event == Json::parse_event_t::key) {
            const auto key = parsed.get<std::string>();
            Require(!objectKeys.empty() && objectKeys.back().insert(key).second, "asset." + key, "object keyが重複しています");
        } else if (event == Json::parse_event_t::object_end) {
            Require(!objectKeys.empty(), "asset", "object構造が不正です"); objectKeys.pop_back();
        }
        return true;
    };
    try { return ReadAsset(Json::parse(bytes, callback)); }
    catch (const Json::exception& error) { Fail("asset", error.what()); }
}
struct TemporaryFile {
    fs::path path;
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool owned = false;
    ~TemporaryFile() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        // CREATE_NEWに成功した自分のtempのみ。原本や他Sessionのtempには触れない。
        if (owned) DeleteFileW(path.c_str());
    }
};
void WriteFileAtomically(const fs::path& target, const std::string& bytes) {
    Require(fs::is_directory(target.parent_path()), "path", "保存先directoryがありません");
    static std::atomic<std::uint64_t> serial{0};
    TemporaryFile temporary;
    temporary.path = target;
    temporary.path += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(serial.fetch_add(1));
    temporary.handle = CreateFileW(temporary.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(temporary.handle != INVALID_HANDLE_VALUE, "path", "排他的temp作成に失敗しました (Win32 " + std::to_string(GetLastError()) + ")");
    temporary.owned = true;
    DWORD written = 0;
    Require(WriteFile(temporary.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size(),
        "path", "temp書込みに失敗しました");
    Require(FlushFileBuffers(temporary.handle) != FALSE, "path", "temp flushに失敗しました");
    const BOOL closed = CloseHandle(temporary.handle);
    if (closed) temporary.handle = INVALID_HANDLE_VALUE;
    Require(closed != FALSE, "path", "temp closeに失敗しました");
    const BOOL replaced = MoveFileExW(temporary.path.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    const DWORD replaceError = replaced ? ERROR_SUCCESS : GetLastError();
    Require(replaced != FALSE,
        "path", "Atomic Replaceに失敗しました (Win32 " + std::to_string(replaceError) + ")");
    temporary.owned = false;
}

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
    default: Fail("easing", "未知のeasingです");
    }
}
float Mix(float a, float b, float t) { return static_cast<float>(static_cast<double>(a) + (static_cast<double>(b) - a) * t); }
Vector3 Mix(Vector3 a, Vector3 b, float t) { return {Mix(a.x, b.x, t), Mix(a.y, b.y, t), Mix(a.z, b.z, t)}; }
Vector4 Mix(Vector4 a, Vector4 b, float t) { return {Mix(a.x, b.x, t), Mix(a.y, b.y, t), Mix(a.z, b.z, t), Mix(a.w, b.w, t)}; }
FragmentPose SamplePose(const FragmentMotion& motion, float time) {
    const auto next = std::upper_bound(motion.keys.begin(), motion.keys.end(), time,
        [](float t, const PoseKey& key) { return t < key.time; });
    const auto& a = next == motion.keys.begin() ? motion.keys.front() : *(next - 1);
    if (next == motion.keys.end() || next == motion.keys.begin()) return {motion.fragmentId, a.position, a.rotationEulerDegrees, a.scale};
    const auto& b = *next;
    const float t = Curve(a.easingToNext, (static_cast<double>(time) - a.time) / (static_cast<double>(b.time) - a.time));
    // Eulerの数値を直接補間することで、複数周回の制作意図を保持する。
    return {motion.fragmentId, Mix(a.position, b.position, t), Mix(a.rotationEulerDegrees, b.rotationEulerDegrees, t), Mix(a.scale, b.scale, t)};
}
FlashSample SampleFlash(const std::vector<FlashKey>& keys, Phase phase, float time) {
    const FlashKey* previous = nullptr;
    for (const auto& key : keys) {
        if (key.phase != phase) continue;
        if (key.time > time && previous) {
            const float t = Curve(previous->easingToNext, (static_cast<double>(time) - previous->time) / (static_cast<double>(key.time) - previous->time));
            return {Mix(previous->color, key.color, t), Mix(previous->intensity, key.intensity, t)};
        }
        previous = &key;
    }
    return previous ? FlashSample{previous->color, previous->intensity} : FlashSample{};
}
struct LinkedFailure { LoadLinkedDiagnostic diagnostic; };
using LinkedCode = LoadLinkedDiagnosticCode;
[[noreturn]] void LinkedFail(LinkedCode code, const std::string& field, const std::string& detail) {
    throw LinkedFailure{{code, field, detail}};
}
void LinkedRequire(bool condition, LinkedCode code, const std::string& field, const std::string& detail) {
    if (!condition) LinkedFail(code, field, detail);
}
template<class F> bool LinkedGuard(LoadLinkedDiagnostics& diagnostics, F&& operation) {
    diagnostics.clear();
    try { operation(); return true; }
    catch (const LinkedFailure& error) { diagnostics.push_back(error.diagnostic); }
    catch (const std::bad_alloc& error) { diagnostics.push_back({LinkedCode::ResourceFailure, "asset", error.what()}); }
    catch (const fs::filesystem_error& error) { diagnostics.push_back({LinkedCode::IoFailure, "path", error.what()}); }
    catch (const std::exception& error) { diagnostics.push_back({LinkedCode::InternalFailure, "asset", error.what()}); }
    return false;
}
void LinkedUtf8(std::string_view value, std::size_t limit, const std::string& field) {
    LinkedRequire(value.size() <= limit, LinkedCode::OutOfRange, field, "UTF-8 byte上限を超えています");
    if (!value.empty()) LinkedRequire(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0) > 0, LinkedCode::InvalidUtf8, field, "UTF-8が不正です");
}
void LinkedText(const std::string& value, std::size_t limit, const std::string& field, bool allowLf = false) {
    LinkedUtf8(value, limit, field);
    for (unsigned char c : value) LinkedRequire((c >= 32 && c != 127) || (allowLf && c == 10),
        LinkedCode::OutOfRange, field, "許可されない制御文字です");
}
void LinkedRange(double value, double low, double high, const std::string& field, bool exclusiveLow = false) {
    LinkedRequire(std::isfinite(value) && (exclusiveLow ? value > low : value >= low) && value <= high,
        LinkedCode::OutOfRange, field, "有限値かつ許可範囲内である必要があります");
}
void LinkedFields(const Json& value, std::initializer_list<const char*> names, const std::string& field) {
    LinkedRequire(value.is_object(), LinkedCode::InvalidType, field, "objectが必要です");
    for (const auto* name : names) LinkedRequire(value.contains(name), LinkedCode::MissingField,
        field == "asset" ? name : field + "." + name, "必須項目がありません");
    for (auto it = value.begin(); it != value.end(); ++it) LinkedRequire(
        std::find_if(names.begin(), names.end(), [&](const char* name) { return it.key() == name; }) != names.end(),
        LinkedCode::UnknownField, field == "asset" ? it.key() : field + "." + it.key(), "未知項目です");
}
const Json& LinkedArray(const Json& value, std::size_t count, const std::string& field) {
    LinkedRequire(value.is_array(), LinkedCode::InvalidType, field, "配列が必要です");
    LinkedRequire(value.size() == count, LinkedCode::OutOfRange, field, "成分数が違います");
    return value;
}
std::string LinkedString(const Json& value, const std::string& field) {
    LinkedRequire(value.is_string(), LinkedCode::InvalidType, field, "文字列が必要です");
    return value.get<std::string>();
}
std::string LinkedSavedText(const Json& value, const std::string& field, std::size_t limit,
    bool allowLf = false, bool requireNonempty = false) {
    auto text = LinkedString(value, field);
    LinkedText(text, limit, field, allowLf);
    LinkedRequire(!requireNonempty || !text.empty(), LinkedCode::OutOfRange, field, "空文字は使えません");
    return text;
}
bool LinkedBool(const Json& value, const std::string& field) {
    LinkedRequire(value.is_boolean(), LinkedCode::InvalidType, field, "booleanが必要です");
    return value.get<bool>();
}
std::uint32_t LinkedInteger(const Json& value, const std::string& field, std::uint32_t low, std::uint32_t high) {
    LinkedRequire(value.is_number_integer(), LinkedCode::InvalidType, field, "整数が必要です");
    // 負数やuint64の上位をuint32へ丸める前に検証する。
    LinkedRange(value.get<double>(), low, high, field);
    return value.get<std::uint32_t>();
}
float LinkedNumber(const Json& value, const std::string& field, double low, double high, bool exclusiveLow = false) {
    LinkedRequire(value.is_number(), LinkedCode::InvalidType, field, "数値が必要です");
    const double raw = value.get<double>();
    LinkedRange(raw, low, high, field, exclusiveLow);
    const float converted = static_cast<float>(raw);
    LinkedRange(converted, low, high, field, exclusiveLow);
    return converted;
}
Vector2 LinkedRead2(const Json& value, const std::string& field, double low, double high) {
    LinkedArray(value, 2, field);
    return {LinkedNumber(value[0], At(field, 0), low, high), LinkedNumber(value[1], At(field, 1), low, high)};
}
Vector3 LinkedRead3(const Json& value, const std::string& field, double low, double high) {
    LinkedArray(value, 3, field);
    return {LinkedNumber(value[0], At(field, 0), low, high), LinkedNumber(value[1], At(field, 1), low, high),
        LinkedNumber(value[2], At(field, 2), low, high)};
}
Vector4 LinkedRead4(const Json& value, const std::string& field) {
    LinkedArray(value, 4, field);
    return {LinkedNumber(value[0], At(field, 0), 0, 1), LinkedNumber(value[1], At(field, 1), 0, 1),
        LinkedNumber(value[2], At(field, 2), 0, 1), LinkedNumber(value[3], At(field, 3), 0, 1)};
}
template<class E> E LinkedEnum(const Json& value, const std::string& field,
    std::initializer_list<std::pair<const char*, E>> entries) {
    const auto name = LinkedString(value, field);
    for (const auto& entry : entries) if (name == entry.first) return entry.second;
    LinkedFail(LinkedCode::InvalidType, field, "未知の列挙値です");
}
LoadLinkedSeed ReadLinkedSeed(const Json& value, const std::string& field) {
    LinkedFields(value, {"mode", "value"}, field);
    return {LinkedEnum<LoadLinkedSeedMode>(value.at("mode"), field + ".mode",
        {{"Fixed", LoadLinkedSeedMode::Fixed}, {"PerRequest", LoadLinkedSeedMode::PerRequest}}),
        LinkedInteger(value.at("value"), field + ".value", 0, UINT32_MAX)};
}
void ValidateLinkedSeed(const LoadLinkedSeed& seed, const std::string& field) {
    LinkedRequire(seed.mode == LoadLinkedSeedMode::Fixed || seed.mode == LoadLinkedSeedMode::PerRequest,
        LinkedCode::InvalidType, field + ".mode", "未知のseed modeです");
}
Json WriteLinkedSeed(const LoadLinkedSeed& seed) {
    return {{"mode", seed.mode == LoadLinkedSeedMode::Fixed ? "Fixed" : "PerRequest"}, {"value", seed.value}};
}
// 数値一致では±0を区別しない。NaNだけbit表現の一致を用いる。
bool LinkedEqual(float a, float b) noexcept {
    return a == b || (std::isnan(a) && std::isnan(b) &&
        std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b));
}
bool LinkedEqual(Vector2 a, Vector2 b) noexcept { return LinkedEqual(a.x, b.x) && LinkedEqual(a.y, b.y); }
bool LinkedEqual(Vector3 a, Vector3 b) noexcept {
    return LinkedEqual(a.x, b.x) && LinkedEqual(a.y, b.y) && LinkedEqual(a.z, b.z);
}
bool LinkedEqual(Vector4 a, Vector4 b) noexcept {
    return LinkedEqual(a.x, b.x) && LinkedEqual(a.y, b.y) && LinkedEqual(a.z, b.z) && LinkedEqual(a.w, b.w);
}
template<class T> bool LinkedEqual(const T& a, const T& b) noexcept { return a == b; }
bool LinkedEqual(const LoadLinkedSeed& a, const LoadLinkedSeed& b) noexcept {
    return a.mode == b.mode && a.value == b.value;
}

const std::initializer_list<const char*> LinkedStyleFields = {
    "sweepEasing", "afterglowDuration", "lineColor", "lineWidth", "glowColor", "glowWidth", "glowIntensity"
};
const std::initializer_list<const char*> LinkedCollapseFields = {
    "duration", "launchSpeed", "directionMode", "direction", "fallAcceleration", "angularVelocity",
    "speedVariation", "directionVariationDegrees", "rotationVariation", "endFadeDuration"
};
const std::initializer_list<const char*> LinkedTextFields = {
    "enabled", "text", "referenceSize", "position", "pivot", "layoutSize", "fontFamily", "fontSize", "color",
    "outlineEnabled", "outlineColor", "outlineWidth", "motion", "pulsePeriod", "minimumOpacity"
};
const std::initializer_list<const char*> LinkedRandomFields = {
    "kind", "generationVersion", "seed", "angleStartDegrees", "angleSpanDegrees", "pointMin", "pointMax",
    "minimumAngleSeparationDegrees", "minimumCutSeparation"
};
const std::initializer_list<const char*> LinkedAssetFields = {
    "version", "effect", "playback", "displayName", "minimumCuts", "maximumCuts", "cutInterval",
    "cutSweepDuration", "cutStyle", "cutSource", "motionSeed", "collapse", "loadingText"
};

LoadLinkedCutStyle ReadLinkedCutStyle(const Json& j) {
    LinkedFields(j, LinkedStyleFields, "cutStyle");
    LoadLinkedCutStyle value;
    value.sweepEasing = LinkedEnum<Easing>(j.at("sweepEasing"), "cutStyle.sweepEasing",
        {{"Linear", Easing::Linear}, {"EaseIn", Easing::EaseIn}, {"EaseOut", Easing::EaseOut}, {"EaseInOut", Easing::EaseInOut}, {"SmoothStep", Easing::SmoothStep}});
    value.afterglowDuration = LinkedNumber(j.at("afterglowDuration"), "cutStyle.afterglowDuration", 0, 3600);
    value.lineColor = LinkedRead4(j.at("lineColor"), "cutStyle.lineColor");
    value.lineWidth = LinkedNumber(j.at("lineWidth"), "cutStyle.lineWidth", 0, 1, true);
    value.glowColor = LinkedRead4(j.at("glowColor"), "cutStyle.glowColor");
    value.glowWidth = LinkedNumber(j.at("glowWidth"), "cutStyle.glowWidth", 0, 1, true);
    value.glowIntensity = LinkedNumber(j.at("glowIntensity"), "cutStyle.glowIntensity", 0, 32);
    return value;
}
Json WriteLinkedCutStyle(const LoadLinkedCutStyle& value) {
    return {
        {"sweepEasing", Name(value.sweepEasing)},
        {"afterglowDuration", value.afterglowDuration},
        {"lineColor", Write4(value.lineColor)},
        {"lineWidth", value.lineWidth},
        {"glowColor", Write4(value.glowColor)},
        {"glowWidth", value.glowWidth},
        {"glowIntensity", value.glowIntensity}
    };
}
LoadLinkedCollapse ReadLinkedCollapse(const Json& j) {
    LinkedFields(j, LinkedCollapseFields, "collapse");
    LoadLinkedCollapse value;
    value.duration = LinkedNumber(j.at("duration"), "collapse.duration", 0, 3600, true);
    value.launchSpeed = LinkedNumber(j.at("launchSpeed"), "collapse.launchSpeed", 0, 10000);
    value.directionMode = LinkedEnum<LoadLinkedDirectionMode>(j.at("directionMode"), "collapse.directionMode",
        {{"Outward", LoadLinkedDirectionMode::Outward}, {"Fixed", LoadLinkedDirectionMode::Fixed}});
    value.direction = LinkedRead3(j.at("direction"), "collapse.direction", -1, 1);
    value.fallAcceleration = LinkedNumber(j.at("fallAcceleration"), "collapse.fallAcceleration", 0, 10000);
    value.angularVelocity = LinkedRead3(j.at("angularVelocity"), "collapse.angularVelocity", -360000, 360000);
    value.speedVariation = LinkedNumber(j.at("speedVariation"), "collapse.speedVariation", 0, 1);
    value.directionVariationDegrees = LinkedNumber(j.at("directionVariationDegrees"), "collapse.directionVariationDegrees", 0, 180);
    value.rotationVariation = LinkedRead3(j.at("rotationVariation"), "collapse.rotationVariation", 0, 360000);
    value.endFadeDuration = LinkedNumber(j.at("endFadeDuration"), "collapse.endFadeDuration", 0, 3600);
    return value;
}
Json WriteLinkedCollapse(const LoadLinkedCollapse& value) {
    return {
        {"duration", value.duration},
        {"launchSpeed", value.launchSpeed},
        {"directionMode", value.directionMode == LoadLinkedDirectionMode::Outward ? "Outward" : "Fixed"},
        {"direction", Write3(value.direction)},
        {"fallAcceleration", value.fallAcceleration},
        {"angularVelocity", Write3(value.angularVelocity)},
        {"speedVariation", value.speedVariation},
        {"directionVariationDegrees", value.directionVariationDegrees},
        {"rotationVariation", Write3(value.rotationVariation)},
        {"endFadeDuration", value.endFadeDuration}
    };
}
LoadLinkedText ReadLinkedText(const Json& j) {
    LinkedFields(j, LinkedTextFields, "loadingText");
    LoadLinkedText value;
    value.enabled = LinkedBool(j.at("enabled"), "loadingText.enabled");
    value.text = LinkedSavedText(j.at("text"), "loadingText.text", 4096, true);
    LinkedArray(j.at("referenceSize"), 2, "loadingText.referenceSize");
    value.referenceSize[0] = LinkedInteger(j.at("referenceSize")[0], "loadingText.referenceSize[0]", 1, 4096);
    value.referenceSize[1] = LinkedInteger(j.at("referenceSize")[1], "loadingText.referenceSize[1]", 1, 4096);
    value.position = LinkedRead2(j.at("position"), "loadingText.position", 0, 4096);
    value.pivot = LinkedRead2(j.at("pivot"), "loadingText.pivot", 0, 1);
    LinkedArray(j.at("layoutSize"), 2, "loadingText.layoutSize");
    value.layoutSize[0] = LinkedInteger(j.at("layoutSize")[0], "loadingText.layoutSize[0]", 1, 4096);
    value.layoutSize[1] = LinkedInteger(j.at("layoutSize")[1], "loadingText.layoutSize[1]", 1, 4096);
    value.fontFamily = LinkedSavedText(j.at("fontFamily"), "loadingText.fontFamily", 256);
    value.fontSize = LinkedNumber(j.at("fontSize"), "loadingText.fontSize", 1, 512);
    value.color = LinkedRead4(j.at("color"), "loadingText.color");
    value.outlineEnabled = LinkedBool(j.at("outlineEnabled"), "loadingText.outlineEnabled");
    value.outlineColor = LinkedRead4(j.at("outlineColor"), "loadingText.outlineColor");
    value.outlineWidth = LinkedNumber(j.at("outlineWidth"), "loadingText.outlineWidth", 0, 32);
    value.motion = LinkedEnum<LoadLinkedTextMotion>(j.at("motion"), "loadingText.motion",
        {{"Static", LoadLinkedTextMotion::Static}, {"Pulse", LoadLinkedTextMotion::Pulse}});
    value.pulsePeriod = LinkedNumber(j.at("pulsePeriod"), "loadingText.pulsePeriod", 0, 3600, true);
    value.minimumOpacity = LinkedNumber(j.at("minimumOpacity"), "loadingText.minimumOpacity", 0, 1);
    return value;
}
Json WriteLinkedText(const LoadLinkedText& value) {
    return {
        {"enabled", value.enabled},
        {"text", value.text},
        {"referenceSize", value.referenceSize},
        {"position", Write2(value.position)},
        {"pivot", Write2(value.pivot)},
        {"layoutSize", value.layoutSize},
        {"fontFamily", value.fontFamily},
        {"fontSize", value.fontSize},
        {"color", Write4(value.color)},
        {"outlineEnabled", value.outlineEnabled},
        {"outlineColor", Write4(value.outlineColor)},
        {"outlineWidth", value.outlineWidth},
        {"motion", value.motion == LoadLinkedTextMotion::Static ? "Static" : "Pulse"},
        {"pulsePeriod", value.pulsePeriod},
        {"minimumOpacity", value.minimumOpacity}
    };
}
LoadLinkedRandomSource ReadLinkedRandomSource(const Json& j) {
    LinkedFields(j, LinkedRandomFields, "cutSource");
    LoadLinkedRandomSource value;
    value.generationVersion = LinkedInteger(j.at("generationVersion"), "cutSource.generationVersion", 1, 1);
    value.seed = ReadLinkedSeed(j.at("seed"), "cutSource.seed");
    value.angleStartDegrees = LinkedNumber(j.at("angleStartDegrees"), "cutSource.angleStartDegrees", 0, 360);
    LinkedRequire(j.at("angleStartDegrees").get<double>() < 360 && value.angleStartDegrees < 360,
        LinkedCode::OutOfRange, "cutSource.angleStartDegrees", "360度未満が必要です");
    value.angleSpanDegrees = LinkedNumber(j.at("angleSpanDegrees"), "cutSource.angleSpanDegrees", 0, 360);
    value.pointMin = LinkedRead2(j.at("pointMin"), "cutSource.pointMin", 0, 1);
    value.pointMax = LinkedRead2(j.at("pointMax"), "cutSource.pointMax", 0, 1);
    value.minimumAngleSeparationDegrees = LinkedNumber(j.at("minimumAngleSeparationDegrees"), "cutSource.minimumAngleSeparationDegrees", 0, 90);
    value.minimumCutSeparation = LinkedNumber(j.at("minimumCutSeparation"), "cutSource.minimumCutSeparation", 0, 1);
    return value;
}
Json WriteLinkedRandomSource(const LoadLinkedRandomSource& value) {
    return {
        {"kind", "Random"},
        {"generationVersion", value.generationVersion},
        {"seed", WriteLinkedSeed(value.seed)},
        {"angleStartDegrees", value.angleStartDegrees},
        {"angleSpanDegrees", value.angleSpanDegrees},
        {"pointMin", Write2(value.pointMin)},
        {"pointMax", Write2(value.pointMax)},
        {"minimumAngleSeparationDegrees", value.minimumAngleSeparationDegrees},
        {"minimumCutSeparation", value.minimumCutSeparation}
    };
}
void ValidateLinkedScalars(const LoadLinkedAsset& asset) {
    LinkedText(asset.displayName, 1024, "displayName");
    LinkedRequire(!asset.displayName.empty(), LinkedCode::OutOfRange, "displayName", "表示名が空です");
    LinkedRange(asset.minimumCuts, 1, 16, "minimumCuts");
    LinkedRange(asset.maximumCuts, 1, 16, "maximumCuts");
    LinkedRange(asset.cutInterval, 0, 3600, "cutInterval");
    LinkedRange(asset.cutSweepDuration, 0, 3600, "cutSweepDuration", true);
    LinkedRequire(asset.cutStyle.sweepEasing == Easing::Linear ||
        asset.cutStyle.sweepEasing == Easing::EaseIn ||
        asset.cutStyle.sweepEasing == Easing::EaseOut ||
        asset.cutStyle.sweepEasing == Easing::EaseInOut ||
        asset.cutStyle.sweepEasing == Easing::SmoothStep,
        LinkedCode::InvalidType, "cutStyle.sweepEasing", "未知の列挙値です");
    LinkedRange(asset.cutStyle.afterglowDuration, 0, 3600, "cutStyle.afterglowDuration");
    LinkedRange(asset.cutStyle.lineColor.x, 0, 1, "cutStyle.lineColor[0]");
    LinkedRange(asset.cutStyle.lineColor.y, 0, 1, "cutStyle.lineColor[1]");
    LinkedRange(asset.cutStyle.lineColor.z, 0, 1, "cutStyle.lineColor[2]");
    LinkedRange(asset.cutStyle.lineColor.w, 0, 1, "cutStyle.lineColor[3]");
    LinkedRange(asset.cutStyle.lineWidth, 0, 1, "cutStyle.lineWidth", true);
    LinkedRange(asset.cutStyle.glowColor.x, 0, 1, "cutStyle.glowColor[0]");
    LinkedRange(asset.cutStyle.glowColor.y, 0, 1, "cutStyle.glowColor[1]");
    LinkedRange(asset.cutStyle.glowColor.z, 0, 1, "cutStyle.glowColor[2]");
    LinkedRange(asset.cutStyle.glowColor.w, 0, 1, "cutStyle.glowColor[3]");
    LinkedRange(asset.cutStyle.glowWidth, 0, 1, "cutStyle.glowWidth", true);
    LinkedRange(asset.cutStyle.glowIntensity, 0, 32, "cutStyle.glowIntensity");
    LinkedRequire(!asset.cutSource.valueless_by_exception(), LinkedCode::InvalidType, "cutSource", "方式が不正です");
    if (const auto* authored = std::get_if<LoadLinkedAuthoredSource>(&asset.cutSource)) {
        LinkedRange(authored->cuts.size(), 1, 16, "cutSource.cuts");
        std::set<std::string> ids;
        for (std::size_t i = 0; i < authored->cuts.size(); ++i) {
            const auto& cut = authored->cuts[i];
            const auto field = At("cutSource.cuts", i);
            LinkedUtf8(cut.id, 64, field + ".id");
            LinkedRequire(!cut.id.empty() && cut.id.size() <= 64, LinkedCode::OutOfRange, field + ".id", "idは1〜64byteです");
            for (unsigned char c : cut.id) LinkedRequire((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-', LinkedCode::InvalidType, field + ".id", "idはASCII英数字/_/-です");
            LinkedRequire(ids.insert(cut.id).second, LinkedCode::InvalidType, field + ".id", "idが重複しています");
            LinkedRange(cut.from.x, 0, 1, field + ".from[0]");
            LinkedRange(cut.from.y, 0, 1, field + ".from[1]");
            LinkedRange(cut.to.x, 0, 1, field + ".to[0]");
            LinkedRange(cut.to.y, 0, 1, field + ".to[1]");
        }
    } else {
        const auto& source = std::get<LoadLinkedRandomSource>(asset.cutSource);
        LinkedRange(source.generationVersion, 1, 1, "cutSource.generationVersion");
        ValidateLinkedSeed(source.seed, "cutSource.seed");
        LinkedRange(source.angleStartDegrees, 0, 360, "cutSource.angleStartDegrees");
        LinkedRequire(source.angleStartDegrees < 360, LinkedCode::OutOfRange, "cutSource.angleStartDegrees", "360度未満が必要です");
        LinkedRange(source.angleSpanDegrees, 0, 360, "cutSource.angleSpanDegrees");
        LinkedRange(source.pointMin.x, 0, 1, "cutSource.pointMin[0]");
        LinkedRange(source.pointMin.y, 0, 1, "cutSource.pointMin[1]");
        LinkedRange(source.pointMax.x, 0, 1, "cutSource.pointMax[0]");
        LinkedRange(source.pointMax.y, 0, 1, "cutSource.pointMax[1]");
        LinkedRange(source.minimumAngleSeparationDegrees, 0, 90, "cutSource.minimumAngleSeparationDegrees");
        LinkedRange(source.minimumCutSeparation, 0, 1, "cutSource.minimumCutSeparation");
    }
    ValidateLinkedSeed(asset.motionSeed, "motionSeed");
    LinkedRange(asset.collapse.duration, 0, 3600, "collapse.duration", true);
    LinkedRange(asset.collapse.launchSpeed, 0, 10000, "collapse.launchSpeed");
    LinkedRequire(asset.collapse.directionMode == LoadLinkedDirectionMode::Outward ||
        asset.collapse.directionMode == LoadLinkedDirectionMode::Fixed,
        LinkedCode::InvalidType, "collapse.directionMode", "未知の列挙値です");
    LinkedRange(asset.collapse.direction.x, -1, 1, "collapse.direction[0]");
    LinkedRange(asset.collapse.direction.y, -1, 1, "collapse.direction[1]");
    LinkedRange(asset.collapse.direction.z, -1, 1, "collapse.direction[2]");
    LinkedRange(asset.collapse.fallAcceleration, 0, 10000, "collapse.fallAcceleration");
    LinkedRange(asset.collapse.angularVelocity.x, -360000, 360000, "collapse.angularVelocity[0]");
    LinkedRange(asset.collapse.angularVelocity.y, -360000, 360000, "collapse.angularVelocity[1]");
    LinkedRange(asset.collapse.angularVelocity.z, -360000, 360000, "collapse.angularVelocity[2]");
    LinkedRange(asset.collapse.speedVariation, 0, 1, "collapse.speedVariation");
    LinkedRange(asset.collapse.directionVariationDegrees, 0, 180, "collapse.directionVariationDegrees");
    LinkedRange(asset.collapse.rotationVariation.x, 0, 360000, "collapse.rotationVariation[0]");
    LinkedRange(asset.collapse.rotationVariation.y, 0, 360000, "collapse.rotationVariation[1]");
    LinkedRange(asset.collapse.rotationVariation.z, 0, 360000, "collapse.rotationVariation[2]");
    LinkedRange(asset.collapse.endFadeDuration, 0, 3600, "collapse.endFadeDuration");
    LinkedText(asset.loadingText.text, 4096, "loadingText.text", true);
    for (auto value : asset.loadingText.referenceSize) LinkedRange(value, 1, 4096, "loadingText.referenceSize");
    LinkedRange(asset.loadingText.position.x, 0, 4096, "loadingText.position[0]");
    LinkedRange(asset.loadingText.position.y, 0, 4096, "loadingText.position[1]");
    LinkedRange(asset.loadingText.pivot.x, 0, 1, "loadingText.pivot[0]");
    LinkedRange(asset.loadingText.pivot.y, 0, 1, "loadingText.pivot[1]");
    for (auto value : asset.loadingText.layoutSize) LinkedRange(value, 1, 4096, "loadingText.layoutSize");
    LinkedText(asset.loadingText.fontFamily, 256, "loadingText.fontFamily");
    LinkedRange(asset.loadingText.fontSize, 1, 512, "loadingText.fontSize");
    LinkedRange(asset.loadingText.color.x, 0, 1, "loadingText.color[0]");
    LinkedRange(asset.loadingText.color.y, 0, 1, "loadingText.color[1]");
    LinkedRange(asset.loadingText.color.z, 0, 1, "loadingText.color[2]");
    LinkedRange(asset.loadingText.color.w, 0, 1, "loadingText.color[3]");
    LinkedRange(asset.loadingText.outlineColor.x, 0, 1, "loadingText.outlineColor[0]");
    LinkedRange(asset.loadingText.outlineColor.y, 0, 1, "loadingText.outlineColor[1]");
    LinkedRange(asset.loadingText.outlineColor.z, 0, 1, "loadingText.outlineColor[2]");
    LinkedRange(asset.loadingText.outlineColor.w, 0, 1, "loadingText.outlineColor[3]");
    LinkedRange(asset.loadingText.outlineWidth, 0, 32, "loadingText.outlineWidth");
    LinkedRequire(asset.loadingText.motion == LoadLinkedTextMotion::Static ||
        asset.loadingText.motion == LoadLinkedTextMotion::Pulse,
        LinkedCode::InvalidType, "loadingText.motion", "未知の列挙値です");
    LinkedRange(asset.loadingText.pulsePeriod, 0, 3600, "loadingText.pulsePeriod", true);
    LinkedRange(asset.loadingText.minimumOpacity, 0, 1, "loadingText.minimumOpacity");
}

// doubleの保存入力とfloatへ確定した値に、同じ相関条件を適用する。
void LinkedCollapseRelations(double duration, double speed, double variation, double acceleration,
    const std::array<double, 3>& direction, const std::array<double, 3>& angular,
    const std::array<double, 3>& rotation, double fade) {
    const double norm = std::hypot(direction[0], direction[1], direction[2]);
    LinkedRequire(norm > 1e-6, LinkedCode::OutOfRange, "collapse.direction", "方向ノルムは1e-6超が必要です");
    const double distance = speed * (1 + variation) * duration + 0.5 * acceleration * duration * duration;
    LinkedRange(distance, 0, 10000, "collapse");
    for (std::size_t i = 0; i < 3; ++i)
        LinkedRange((std::abs(angular[i]) + rotation[i]) * duration, 0, 360000, At("collapse.angularVelocity", i));
    LinkedRange(fade, 0, duration, "collapse.endFadeDuration");
}
void LinkedTextRelations(const std::array<std::uint32_t, 2>& reference, const std::array<std::uint32_t, 2>& layout,
    const std::array<double, 2>& position, const std::array<double, 2>& pivot,
    bool enabled, double alpha, bool outline, double width) {
    if (enabled) LinkedRequire(alpha > 0, LinkedCode::OutOfRange, "loadingText.color[3]", "有効な文字には正のalphaが必要です");
    if (outline) LinkedRequire(width > 0, LinkedCode::OutOfRange, "loadingText.outlineWidth", "有効な輪郭には正の幅が必要です");
    const double padding = (outline ? std::ceil(width) : 0) + 2;
    for (std::size_t i = 0; i < 2; ++i) {
        LinkedRange(position[i], 0, reference[i], At("loadingText.position", i));
        LinkedRange(layout[i] + 2 * padding, 0, 4096, At("loadingText.layoutSize", i));
        const double origin = position[i] - pivot[i] * layout[i];
        LinkedRequire(origin >= 0 && origin + layout[i] <= reference[i], LinkedCode::OutOfRange,
            At("loadingText.layoutSize", i), "文字枠が基準画面内に収まりません");
    }
}
// ASCII space/LFと全角spaceを除く文字があるか、Unicode正規化せず確認する。
bool LinkedHasVisibleText(const std::string& text) {
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == ' ' || text[i] == '\n') { ++i; continue; }
        if (text.compare(i, 3, "\xE3\x80\x80") == 0) { i += 3; continue; }
        return true;
    }
    return false;
}
void ValidateLinkedRelations(const LoadLinkedAsset& asset) {
    LinkedRequire(asset.minimumCuts <= asset.maximumCuts, LinkedCode::OutOfRange, "maximumCuts", "minimum以上が必要です");
    if (const auto* source = std::get_if<LoadLinkedAuthoredSource>(&asset.cutSource)) {
        LinkedRequire(source->cuts.size() >= asset.maximumCuts, LinkedCode::OutOfRange, "cutSource.cuts", "maximumCuts以上が必要です");
    } else {
        const auto& random = std::get<LoadLinkedRandomSource>(asset.cutSource);
        LinkedRequire(random.pointMin.x <= random.pointMax.x && random.pointMin.y <= random.pointMax.y,
            LinkedCode::OutOfRange, "cutSource.pointMax", "各軸でmin以上が必要です");
    }
    const auto& c = asset.collapse;
    LinkedCollapseRelations(c.duration, c.launchSpeed, c.speedVariation, c.fallAcceleration,
        {c.direction.x, c.direction.y, c.direction.z}, {c.angularVelocity.x, c.angularVelocity.y, c.angularVelocity.z},
        {c.rotationVariation.x, c.rotationVariation.y, c.rotationVariation.z}, c.endFadeDuration);
    const auto& t = asset.loadingText;
    LinkedRequire(!t.enabled || LinkedHasVisibleText(t.text), LinkedCode::OutOfRange,
        "loadingText.text", "有効な文面が必要です");
    LinkedTextRelations(t.referenceSize, t.layoutSize, {t.position.x, t.position.y}, {t.pivot.x, t.pivot.y},
        t.enabled, t.color.w, t.outlineEnabled, t.outlineWidth);
}
void ValidateLinkedGeometry(const LoadLinkedAsset& asset) {
    const auto* source = std::get_if<LoadLinkedAuthoredSource>(&asset.cutSource);
    if (!source) return;
    std::vector<Cut> prefix;
    prefix.reserve(source->cuts.size());
    for (const auto& saved : source->cuts) {
        Cut cut;
        cut.id = saved.id;
        cut.from = saved.from;
        cut.to = saved.to;
        prefix.push_back(std::move(cut));
        Geometry geometry;
        Diagnostics diagnostics;
        const auto status = TryBuildGeometryDetailed(prefix, geometry, diagnostics);
        if (status == GeometryBuildStatus::Success) continue;
        LinkedCode code = LinkedCode::InternalFailure;
        switch (status) {
        case GeometryBuildStatus::CandidateRejected: code = LinkedCode::InvalidGeometry; break;
        case GeometryBuildStatus::InvalidInput: code = LinkedCode::InvalidType; break;
        case GeometryBuildStatus::CapacityExceeded: code = LinkedCode::OutOfRange; break;
        case GeometryBuildStatus::AllocationFailure: code = LinkedCode::ResourceFailure; break;
        default: break;
        }
        if (diagnostics.empty()) LinkedFail(LinkedCode::InternalFailure, "cutSource.cuts", "幾何失敗の診断がありません");
        // 幾何のcuts fieldを保存schemaのcutSource.cutsへ対応付ける。
        const auto& diagnostic = diagnostics.front();
        const auto field = diagnostic.field.starts_with("cuts") ? "cutSource." + diagnostic.field : diagnostic.field;
        LinkedFail(code, field, diagnostic.message);
    }
}
template<std::size_t N> std::array<double, N> LinkedRawComponents(const Json& value) {
    std::array<double, N> result{};
    for (std::size_t i = 0; i < N; ++i) result[i] = value[i].get<double>();
    return result;
}

void ValidateLinkedShape(const Json& root) {
    LinkedFields(root, LinkedAssetFields, "asset");
    LinkedFields(root.at("cutStyle"), LinkedStyleFields, "cutStyle");
    const auto& source = root.at("cutSource");
    LinkedRequire(source.is_object(), LinkedCode::InvalidType, "cutSource", "objectが必要です");
    LinkedRequire(source.contains("kind"), LinkedCode::MissingField, "cutSource.kind", "方式がありません");
    const auto kind = LinkedString(source.at("kind"), "cutSource.kind");
    if (kind == "Authored") {
        LinkedFields(source, {"kind", "cuts"}, "cutSource");
        const auto& cuts = source.at("cuts");
        LinkedRequire(cuts.is_array(), LinkedCode::InvalidType, "cutSource.cuts", "配列が必要です");
        LinkedRange(cuts.size(), 1, 16, "cutSource.cuts");
        for (std::size_t i = 0; i < cuts.size(); ++i) {
            const auto field = At("cutSource.cuts", i);
            LinkedFields(cuts[i], {"id", "from", "to"}, field);
            LinkedArray(cuts[i].at("from"), 2, field + ".from");
            LinkedArray(cuts[i].at("to"), 2, field + ".to");
        }
    } else if (kind == "Random") {
        LinkedFields(source, LinkedRandomFields, "cutSource");
        LinkedFields(source.at("seed"), {"mode", "value"}, "cutSource.seed");
        LinkedArray(source.at("pointMin"), 2, "cutSource.pointMin");
        LinkedArray(source.at("pointMax"), 2, "cutSource.pointMax");
    } else LinkedFail(LinkedCode::InvalidType, "cutSource.kind", "未知の方式です");
    LinkedFields(root.at("motionSeed"), {"mode", "value"}, "motionSeed");
    LinkedFields(root.at("collapse"), LinkedCollapseFields, "collapse");
    LinkedFields(root.at("loadingText"), LinkedTextFields, "loadingText");
    for (const auto* name : {"lineColor", "glowColor"})
        LinkedArray(root.at("cutStyle").at(name), 4, std::string("cutStyle.") + name);
    for (const auto* name : {"direction", "angularVelocity", "rotationVariation"})
        LinkedArray(root.at("collapse").at(name), 3, std::string("collapse.") + name);
    for (const auto* name : {"referenceSize", "position", "pivot", "layoutSize"})
        LinkedArray(root.at("loadingText").at(name), 2, std::string("loadingText.") + name);
    for (const auto* name : {"color", "outlineColor"})
        LinkedArray(root.at("loadingText").at(name), 4, std::string("loadingText.") + name);
}

LoadLinkedAsset ReadLinkedAsset(const Json& root) {
    ValidateLinkedShape(root);
    LinkedRequire(LinkedString(root.at("effect"), "effect") == "SwordShatter", LinkedCode::InvalidType, "effect", "未知のeffectです");
    LinkedRequire(LinkedString(root.at("playback"), "playback") == "LoadLinked", LinkedCode::InvalidType, "playback", "未知のplaybackです");
    LoadLinkedAsset asset;
    asset.displayName = LinkedSavedText(root.at("displayName"), "displayName", 1024, false, true);
    asset.minimumCuts = LinkedInteger(root.at("minimumCuts"), "minimumCuts", 1, 16);
    asset.maximumCuts = LinkedInteger(root.at("maximumCuts"), "maximumCuts", 1, 16);
    asset.cutInterval = LinkedNumber(root.at("cutInterval"), "cutInterval", 0, 3600);
    asset.cutSweepDuration = LinkedNumber(root.at("cutSweepDuration"), "cutSweepDuration", 0, 3600, true);
    asset.cutStyle = ReadLinkedCutStyle(root.at("cutStyle"));
    const auto& source = root.at("cutSource");
    LinkedRequire(source.is_object(), LinkedCode::InvalidType, "cutSource", "objectが必要です");
    LinkedRequire(source.contains("kind"), LinkedCode::MissingField, "cutSource.kind", "方式がありません");
    const auto kind = LinkedString(source.at("kind"), "cutSource.kind");
    if (kind == "Authored") {
        LinkedFields(source, {"kind", "cuts"}, "cutSource");
        const auto& cuts = source.at("cuts");
        LinkedRequire(cuts.is_array(), LinkedCode::InvalidType, "cutSource.cuts", "配列が必要です");
        LinkedRange(cuts.size(), 1, 16, "cutSource.cuts");
        LoadLinkedAuthoredSource authored;
        std::set<std::string> ids;
        for (std::size_t i = 0; i < cuts.size(); ++i) {
            const auto field = At("cutSource.cuts", i);
            const auto& cut = cuts[i];
            LinkedFields(cut, {"id", "from", "to"}, field);
            auto id = LinkedSavedText(cut.at("id"), field + ".id", 64, false, true);
            try { Id(id, field + ".id"); }
            catch (const Failure& error) { LinkedFail(LinkedCode::InvalidType, error.diagnostic.field, error.diagnostic.message); }
            LinkedRequire(ids.insert(id).second, LinkedCode::InvalidType, field + ".id", "idが重複しています");
            authored.cuts.push_back({std::move(id), LinkedRead2(cut.at("from"), field + ".from", 0, 1),
                LinkedRead2(cut.at("to"), field + ".to", 0, 1)});
        }
        asset.cutSource = std::move(authored);
    } else if (kind == "Random") {
        asset.cutSource = ReadLinkedRandomSource(source);
    } else LinkedFail(LinkedCode::InvalidType, "cutSource.kind", "未知の方式です");
    asset.motionSeed = ReadLinkedSeed(root.at("motionSeed"), "motionSeed");
    asset.collapse = ReadLinkedCollapse(root.at("collapse"));
    asset.loadingText = ReadLinkedText(root.at("loadingText"));
    ValidateLinkedScalars(asset);
    // 元doubleの相関を確認してから、float保存値の相関と幾何を検証する。
    LinkedRequire(asset.minimumCuts <= asset.maximumCuts, LinkedCode::OutOfRange, "maximumCuts", "minimum以上が必要です");
    if (kind == "Authored") LinkedRequire(std::get<LoadLinkedAuthoredSource>(asset.cutSource).cuts.size() >= asset.maximumCuts,
        LinkedCode::OutOfRange, "cutSource.cuts", "maximumCuts以上が必要です");
    if (kind == "Random") {
        const auto low = LinkedRawComponents<2>(source.at("pointMin"));
        const auto high = LinkedRawComponents<2>(source.at("pointMax"));
        LinkedRequire(low[0] <= high[0] && low[1] <= high[1], LinkedCode::OutOfRange,
            "cutSource.pointMax", "各軸でmin以上が必要です");
    }
    const auto& c = root.at("collapse");
    LinkedCollapseRelations(c.at("duration").get<double>(), c.at("launchSpeed").get<double>(),
        c.at("speedVariation").get<double>(), c.at("fallAcceleration").get<double>(),
        LinkedRawComponents<3>(c.at("direction")), LinkedRawComponents<3>(c.at("angularVelocity")),
        LinkedRawComponents<3>(c.at("rotationVariation")), c.at("endFadeDuration").get<double>());
    const auto& t = root.at("loadingText");
    LinkedRequire(!asset.loadingText.enabled || LinkedHasVisibleText(asset.loadingText.text), LinkedCode::OutOfRange,
        "loadingText.text", "有効な文面が必要です");
    LinkedTextRelations(asset.loadingText.referenceSize, asset.loadingText.layoutSize,
        LinkedRawComponents<2>(t.at("position")), LinkedRawComponents<2>(t.at("pivot")),
        asset.loadingText.enabled, t.at("color")[3].get<double>(), asset.loadingText.outlineEnabled,
        t.at("outlineWidth").get<double>());
    ValidateLinkedRelations(asset);
    ValidateLinkedGeometry(asset);
    return asset;
}
Json WriteLinkedAsset(const LoadLinkedAsset& asset) {
    Json source;
    if (const auto* authored = std::get_if<LoadLinkedAuthoredSource>(&asset.cutSource)) {
        source = {{"kind", "Authored"}, {"cuts", Json::array()}};
        for (const auto& cut : authored->cuts) source["cuts"].push_back(
            {{"id", cut.id}, {"from", Write2(cut.from)}, {"to", Write2(cut.to)}});
    } else source = WriteLinkedRandomSource(std::get<LoadLinkedRandomSource>(asset.cutSource));
    return {{"version", 2}, {"effect", "SwordShatter"}, {"playback", "LoadLinked"},
        {"displayName", asset.displayName}, {"minimumCuts", asset.minimumCuts}, {"maximumCuts", asset.maximumCuts},
        {"cutInterval", asset.cutInterval}, {"cutSweepDuration", asset.cutSweepDuration},
        {"cutStyle", WriteLinkedCutStyle(asset.cutStyle)}, {"cutSource", std::move(source)},
        {"motionSeed", WriteLinkedSeed(asset.motionSeed)}, {"collapse", WriteLinkedCollapse(asset.collapse)},
        {"loadingText", WriteLinkedText(asset.loadingText)}};
}
Json ParseLinkedBytes(std::string_view bytes) {
    LinkedUtf8(bytes, MaxJsonBytes, "asset");
    // JSON文法にない生NULを、lexerが入力終端として扱って後続bytesを見落とす前に拒否する。
    LinkedRequire(bytes.find('\0') == std::string_view::npos, LinkedCode::InvalidType, "asset", "JSONに生NULを含められません");
    std::vector<std::set<std::string>> keys;
    const auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
        LinkedRequire(depth <= MaxJsonDepth, LinkedCode::OutOfRange, "asset", "JSON階層上限を超えています");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::key) {
            const auto key = parsed.get<std::string>();
            LinkedRequire(!keys.empty() && keys.back().insert(key).second, LinkedCode::DuplicateField, "asset." + key, "keyが重複しています");
        } else if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    try { return Json::parse(bytes.begin(), bytes.end(), callback); }
    catch (const Json::out_of_range& error) { LinkedFail(LinkedCode::OutOfRange, "asset", error.what()); }
    catch (const Json::exception& error) { LinkedFail(LinkedCode::InvalidType, "asset", error.what()); }
}

bool LinkedSamePath(const fs::path& a, const fs::path& b) {
    auto leftPath = a;
    auto rightPath = b;
    leftPath.make_preferred();
    rightPath.make_preferred();
    auto left = leftPath.begin(), right = rightPath.begin();
    for (; left != leftPath.end() && right != rightPath.end(); ++left, ++right) {
        if (CompareStringOrdinal(left->c_str(), -1, right->c_str(), -1, TRUE) != CSTR_EQUAL) return false;
    }
    return left == leftPath.end() && right == rightPath.end();
}
struct LinkedProjectInfo {
    EditorAssetProjectKind kind = EditorAssetProjectKind::Legacy;
    fs::path source, resources, descriptor;
    std::string projectId;
};
LinkedProjectInfo InspectLinkedProject(const fs::path& source) {
    LinkedRequire(!source.empty(), LinkedCode::InvalidPath, "context", "source rootが空です");
    LinkedRequire(fs::is_directory(source), LinkedCode::InvalidPath, "context", "source directoryがありません");
    LinkedProjectInfo info;
    info.source = fs::canonical(source);
    LinkedRequire(fs::is_directory(info.source / L"resources"), LinkedCode::InvalidPath, "context", "resources directoryがありません");
    info.resources = fs::canonical(info.source / L"resources");
    const auto descriptor = info.source.parent_path() / L"game.project.json";
    std::error_code error;
    const auto status = fs::symlink_status(descriptor, error);
    // 存在しない場合だけLegacy判定へ進む。検査不能や壊れたdescriptorは拒否する。
    LinkedRequire(!error || error == std::errc::no_such_file_or_directory,
        LinkedCode::IoFailure, "context", "descriptorの存在を確認できません");
    if (fs::exists(status)) {
        ProjectDescriptor project;
        std::string detail;
        if (!project.Load(descriptor, detail)) LinkedFail(LinkedCode::InvalidPath, "context", detail);
        LinkedRequire(LinkedSamePath(fs::canonical(project.GetProjectRoot() / L"project"), info.source),
            LinkedCode::InvalidPath, "context", "descriptorとsource rootが一致しません");
        info.kind = EditorAssetProjectKind::Descriptor;
        info.projectId = project.GetProjectId();
        info.descriptor = fs::canonical(descriptor);
    } else {
        LinkedRequire(fs::exists(info.source / L"CG2_2025_04_14.vcxproj"), LinkedCode::InvalidPath,
            "context", "Legacy source markerがありません");
    }
    return info;
}
void VerifyLinkedContext(const EditorAssetContextPtr& context) {
    LinkedRequire(context != nullptr, LinkedCode::InvalidPath, "context", "contextがありません");
    const auto info = InspectLinkedProject(context->SourceRoot());
    LinkedRequire(info.kind == context->Kind() && info.projectId == context->ProjectId() &&
        LinkedSamePath(info.source, context->SourceRoot()) && LinkedSamePath(info.resources, context->ResourcesRoot()) &&
        LinkedSamePath(info.descriptor, context->DescriptorPath()), LinkedCode::ExternalChange, "context", "Projectの識別情報またはrootが変更されています");
}
struct LinkedResolvedPath { std::string logical; fs::path target; };
LinkedResolvedPath ResolveLinkedTarget(const EditorAssetContextPtr& context, const std::string& path) {
    fs::path logical;
    try { logical = LogicalPath(path, "path", true); }
    catch (const Failure& error) { LinkedFail(LinkedCode::InvalidPath, error.diagnostic.field, error.diagnostic.message); }
    auto it = logical.begin();
    ++it; // LogicalPathが確認したresources成分を外し、捕捉rootへ結合する。
    fs::path relative;
    for (; it != logical.end(); ++it) relative /= *it;
    auto target = fs::weakly_canonical(context->ResourcesRoot() / relative);
    LinkedRequire(Inside(context->ResourcesRoot(), target), LinkedCode::InvalidPath, "path", "解決後のpathがresources境界外です");
    LinkedRequire(fs::is_directory(target.parent_path()), LinkedCode::InvalidPath, "path", "保存先directoryがありません");
    return {StringUtility::ToUtf8(logical), std::move(target)};
}
std::string ReadLinkedBytes(const fs::path& target) {
    std::ifstream input(target, std::ios::binary);
    LinkedRequire(input.is_open(), LinkedCode::IoFailure, "path", "Assetを開けません");
    std::string bytes(MaxJsonBytes + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    LinkedRequire(!input.bad() && (input.eof() || !input.fail()), LinkedCode::IoFailure, "path", "読込に失敗しました");
    const auto count = input.gcount();
    LinkedRequire(count >= 0 && static_cast<std::size_t>(count) <= MaxJsonBytes, LinkedCode::OutOfRange, "asset", "JSON byte上限を超えています");
    bytes.resize(static_cast<std::size_t>(count));
    return bytes;
}
void CompareLinkedBaseline(const EditorAssetContextPtr& context, const LinkedResolvedPath& resolved,
    const LoadLinkedWriteExpectation& expectation, LoadLinkedIoStage& stage) {
    stage = LoadLinkedIoStage::Resolve;
    VerifyLinkedContext(context);
    const auto now = ResolveLinkedTarget(context, resolved.logical);
    LinkedRequire(LinkedSamePath(now.target, resolved.target), LinkedCode::ExternalChange, "path", "解決先が変更されています");
    stage = LoadLinkedIoStage::Compare;
    if (const auto* match = std::get_if<LoadLinkedMatchSnapshot>(&expectation)) {
        const auto& snapshot = match->snapshot;
        LinkedRequire(snapshot && snapshot->Context() == context && LinkedSamePath(snapshot->TargetPath(), resolved.target),
            LinkedCode::ExternalChange, "path", "保存基準のcontextまたはtargetが一致しません");
        stage = LoadLinkedIoStage::Read;
        const auto bytes = ReadLinkedBytes(resolved.target);
        stage = LoadLinkedIoStage::Compare;
        LinkedRequire(bytes == snapshot->Bytes(), LinkedCode::ExternalChange, "path", "元ファイルのbytesが変更されています");
    } else {
        LinkedRequire(!fs::exists(resolved.target), LinkedCode::ExternalChange, "path", "同名のAssetが存在します");
    }
}
// Writerのエラー経路では診断の割当も失敗し得る。scalarは常に先に確定する。
void SetLinkedWriteDiagnostic(LoadLinkedWriteResult& result, LinkedCode code,
    const char* field, const char* detail) noexcept {
    result.failureCode = code;
    try { result.diagnostics.push_back({code, field, detail}); }
    catch (...) { result.diagnostics.clear(); }
}

} // namespace

// V1公開Guardと新Decodeの内側で、例外の型を保った共通構築を行う。
struct CompiledTransitionV1Access {
    static std::shared_ptr<const CompiledTransition> Compile(const Asset& asset) {
        auto geometry = Build(asset.cuts); Validate(asset, geometry);
        std::shared_ptr<CompiledTransition> result(new CompiledTransition);
        result->asset_ = asset; result->geometry_ = std::move(geometry);
        for (const auto& fragment : result->geometry_.fragments) {
            const auto motion = std::find_if(asset.fragments.begin(), asset.fragments.end(), [&](const auto& item) { return item.fragmentId == fragment.fragmentId; });
            result->motions_.push_back(*motion);
        }
        result->audioCues_ = asset.audioCues;
        std::stable_sort(result->audioCues_.begin(), result->audioCues_.end(), [](const AudioCue& a, const AudioCue& b) {
            if (a.phase != b.phase) return a.phase == Phase::Cutting;
            return a.time < b.time;
        });
        return result;
    }
};

GeometryBuildStatus TryBuildGeometryDetailed(
    const std::vector<Cut>& cuts, Geometry& geometry, Diagnostics& diagnostics) {
    diagnostics.clear();
    try {
        auto result = Build(cuts);
        // 全検証を通過した完成値を、割当を伴わないmoveで一度だけ反映する。
        static_assert(noexcept(geometry = std::move(result)));
        geometry = std::move(result);
        return GeometryBuildStatus::Success;
    } catch (const Failure& error) {
        diagnostics.push_back(error.diagnostic);
        switch (error.geometryStatus) {
        case GeometryBuildStatus::CandidateRejected:
        case GeometryBuildStatus::InvalidInput:
        case GeometryBuildStatus::CapacityExceeded:
        case GeometryBuildStatus::InternalFailure:
        case GeometryBuildStatus::AllocationFailure:
            return error.geometryStatus;
        case GeometryBuildStatus::Success:
        default:
            return GeometryBuildStatus::InternalFailure;
        }
    } catch (const std::bad_alloc& error) {
        diagnostics.push_back({"asset", error.what()});
        return GeometryBuildStatus::AllocationFailure;
    } catch (const std::exception& error) {
        diagnostics.push_back({"asset", error.what()});
        return GeometryBuildStatus::InternalFailure;
    }
}

bool TryBuildGeometry(const std::vector<Cut>& cuts, Geometry& geometry, Diagnostics& diagnostics) {
    return Guard(diagnostics, [&] { auto result = Build(cuts); geometry = std::move(result); });
}
bool TryCompile(const Asset& asset, std::shared_ptr<const CompiledTransition>& compiled, Diagnostics& diagnostics) {
    return Guard(diagnostics, [&] { compiled = CompiledTransitionV1Access::Compile(asset); });
}
bool TryLoad(const std::string& logicalPath, std::shared_ptr<const CompiledTransition>& compiled, Diagnostics& diagnostics) {
    return Guard(diagnostics, [&] {
        const auto asset = ReadFile(ResolveAsset(logicalPath));
        std::shared_ptr<const CompiledTransition> result;
        Diagnostics compileDiagnostics;
        if (!TryCompile(asset, result, compileDiagnostics)) throw Failure{compileDiagnostics.front()};
        compiled = std::move(result);
    });
}
bool TrySave(const std::string& logicalPath, const Asset& asset, Diagnostics& diagnostics) {
    return Guard(diagnostics, [&] {
        std::shared_ptr<const CompiledTransition> compiled; Diagnostics compileDiagnostics;
        if (!TryCompile(asset, compiled, compileDiagnostics)) throw Failure{compileDiagnostics.front()};
        const auto target = ResolveAsset(logicalPath);
        const auto bytes = WriteAsset(compiled->GetAsset()).dump(2);
        Require(bytes.size() <= MaxJsonBytes, "asset", "保存JSON byte上限を超えています");
        WriteFileAtomically(target, bytes);
    });
}
bool TryEvaluate(const CompiledTransition& compiled, Phase phase, float localTime, FrameSample& sample, Diagnostics& diagnostics) {
    return Guard(diagnostics, [&] {
        CheckPhase(phase, "phase"); Require(std::isfinite(localTime), "localTime", "時刻は有限値が必要です");
        const auto& asset = compiled.GetAsset(); const auto& geometry = compiled.GetGeometry();
        FrameSample result; result.phase = phase; result.localTime = std::clamp(localTime, 0.0f, Duration(asset, phase));
        for (const auto& motion : compiled.GetMotions()) {
            if (phase == Phase::Cutting) result.fragments.push_back({motion.fragmentId, {}, {}, {1, 1, 1}});
            else result.fragments.push_back(SamplePose(motion, result.localTime));
        }
        for (std::size_t i = 0; i < asset.cuts.size(); ++i) {
            const auto& cut = asset.cuts[i];
            CutSample value{geometry.cutSegments[i], 0, 0, cut.lineColor, cut.glowColor, cut.lineWidth, cut.glowWidth, cut.glowIntensity};
            if (phase == Phase::Collapse) value.reveal = 1;
            else {
                const CutTimes times = ResolveCutTimes(cut);
                if (result.localTime >= cut.startTime) {
                    // 到達を先に分岐し、float化で幅0になった短区間でも0除算せず終端値を返す。
                    if (result.localTime < times.sweepEnd) {
                        value.reveal = Curve(cut.sweepEasing,
                            (static_cast<double>(result.localTime) - cut.startTime) /
                            (static_cast<double>(times.sweepEnd) - cut.startTime));
                        value.glow = 1;
                    } else {
                        value.reveal = 1;
                        if (result.localTime < times.afterglowEnd) {
                            value.glow = static_cast<float>(1 -
                                (static_cast<double>(result.localTime) - times.sweepEnd) /
                                (static_cast<double>(times.afterglowEnd) - times.sweepEnd));
                        }
                    }
                }
            }
            result.cuts.push_back(value);
        }
        result.flash = SampleFlash(asset.flashKeys, phase, result.localTime);
        sample = std::move(result);
    });
}
bool EqualLoadLinkedSavedValues(const LoadLinkedAsset& a, const LoadLinkedAsset& b) noexcept {
    if (!(LinkedEqual(a.displayName, b.displayName)
        && LinkedEqual(a.minimumCuts, b.minimumCuts)
        && LinkedEqual(a.maximumCuts, b.maximumCuts)
        && LinkedEqual(a.cutInterval, b.cutInterval)
        && LinkedEqual(a.cutSweepDuration, b.cutSweepDuration)
        && LinkedEqual(a.motionSeed, b.motionSeed)
        && LinkedEqual(a.cutStyle.sweepEasing, b.cutStyle.sweepEasing)
        && LinkedEqual(a.cutStyle.afterglowDuration, b.cutStyle.afterglowDuration)
        && LinkedEqual(a.cutStyle.lineColor, b.cutStyle.lineColor)
        && LinkedEqual(a.cutStyle.lineWidth, b.cutStyle.lineWidth)
        && LinkedEqual(a.cutStyle.glowColor, b.cutStyle.glowColor)
        && LinkedEqual(a.cutStyle.glowWidth, b.cutStyle.glowWidth)
        && LinkedEqual(a.cutStyle.glowIntensity, b.cutStyle.glowIntensity)
        && LinkedEqual(a.collapse.duration, b.collapse.duration)
        && LinkedEqual(a.collapse.launchSpeed, b.collapse.launchSpeed)
        && LinkedEqual(a.collapse.directionMode, b.collapse.directionMode)
        && LinkedEqual(a.collapse.direction, b.collapse.direction)
        && LinkedEqual(a.collapse.fallAcceleration, b.collapse.fallAcceleration)
        && LinkedEqual(a.collapse.angularVelocity, b.collapse.angularVelocity)
        && LinkedEqual(a.collapse.speedVariation, b.collapse.speedVariation)
        && LinkedEqual(a.collapse.directionVariationDegrees, b.collapse.directionVariationDegrees)
        && LinkedEqual(a.collapse.rotationVariation, b.collapse.rotationVariation)
        && LinkedEqual(a.collapse.endFadeDuration, b.collapse.endFadeDuration)
        && LinkedEqual(a.loadingText.enabled, b.loadingText.enabled)
        && LinkedEqual(a.loadingText.text, b.loadingText.text)
        && LinkedEqual(a.loadingText.referenceSize, b.loadingText.referenceSize)
        && LinkedEqual(a.loadingText.position, b.loadingText.position)
        && LinkedEqual(a.loadingText.pivot, b.loadingText.pivot)
        && LinkedEqual(a.loadingText.layoutSize, b.loadingText.layoutSize)
        && LinkedEqual(a.loadingText.fontFamily, b.loadingText.fontFamily)
        && LinkedEqual(a.loadingText.fontSize, b.loadingText.fontSize)
        && LinkedEqual(a.loadingText.color, b.loadingText.color)
        && LinkedEqual(a.loadingText.outlineEnabled, b.loadingText.outlineEnabled)
        && LinkedEqual(a.loadingText.outlineColor, b.loadingText.outlineColor)
        && LinkedEqual(a.loadingText.outlineWidth, b.loadingText.outlineWidth)
        && LinkedEqual(a.loadingText.motion, b.loadingText.motion)
        && LinkedEqual(a.loadingText.pulsePeriod, b.loadingText.pulsePeriod)
        && LinkedEqual(a.loadingText.minimumOpacity, b.loadingText.minimumOpacity))) return false;
    if (a.cutSource.index() != b.cutSource.index()) return false;
    if (a.cutSource.valueless_by_exception()) return true;
    if (const auto* x = std::get_if<LoadLinkedAuthoredSource>(&a.cutSource)) {
        const auto& y = std::get<LoadLinkedAuthoredSource>(b.cutSource);
        if (x->cuts.size() != y.cuts.size()) return false;
        for (std::size_t i = 0; i < x->cuts.size(); ++i) {
            if (x->cuts[i].id != y.cuts[i].id || !LinkedEqual(x->cuts[i].from, y.cuts[i].from) ||
                !LinkedEqual(x->cuts[i].to, y.cuts[i].to)) return false;
        }
        return true;
    }
    const auto& x = std::get<LoadLinkedRandomSource>(a.cutSource);
    const auto& y = std::get<LoadLinkedRandomSource>(b.cutSource);
    return LinkedEqual(x.generationVersion, y.generationVersion)
        && LinkedEqual(x.seed, y.seed)
        && LinkedEqual(x.angleStartDegrees, y.angleStartDegrees)
        && LinkedEqual(x.angleSpanDegrees, y.angleSpanDegrees)
        && LinkedEqual(x.pointMin, y.pointMin)
        && LinkedEqual(x.pointMax, y.pointMax)
        && LinkedEqual(x.minimumAngleSeparationDegrees, y.minimumAngleSeparationDegrees)
        && LinkedEqual(x.minimumCutSeparation, y.minimumCutSeparation);
}

LoadLinkedRandomSource MakeDefaultLoadLinkedRandomSource() {
    LoadLinkedRandomSource source;
    source.generationVersion = 1;
    source.seed = {LoadLinkedSeedMode::Fixed, 1};
    source.angleSpanDegrees = 360;
    source.pointMin = {0.15f, 0.15f};
    source.pointMax = {0.85f, 0.85f};
    return source;
}
LoadLinkedAsset MakeDefaultLoadLinked(LoadLinkedDefaultLanguage language) {
    if (language != LoadLinkedDefaultLanguage::Japanese && language != LoadLinkedDefaultLanguage::English)
        throw std::invalid_argument("Unknown load-linked default language");
    LoadLinkedAsset asset;
    asset.displayName = language == LoadLinkedDefaultLanguage::Japanese ? "ロード連動の斬り込み" : "Load-linked sword shatter";
    asset.minimumCuts = 2;
    asset.maximumCuts = 2;
    asset.cutInterval = 0.15f;
    asset.cutSweepDuration = 0.25f;
    asset.cutStyle.afterglowDuration = 0.8f;
    asset.cutStyle.lineColor = {0.04f, 0.04f, 0.04f, 1};
    asset.cutStyle.lineWidth = 0.003f;
    asset.cutStyle.glowColor = {0.25f, 0.65f, 1, 1};
    asset.cutStyle.glowWidth = 0.025f;
    asset.cutStyle.glowIntensity = 3;
    asset.cutSource = LoadLinkedAuthoredSource{{{"h", {0, 0.5f}, {1, 0.5f}}, {"d", {0.2f, 0.1f}, {0.8f, 0.9f}}}};
    asset.motionSeed = {LoadLinkedSeedMode::Fixed, 1};
    asset.collapse.duration = 2;
    asset.collapse.launchSpeed = 0.8f;
    asset.collapse.direction = {0, 0, 1};
    asset.collapse.fallAcceleration = 0.6f;
    asset.collapse.angularVelocity = {0, 0, 45};
    asset.collapse.endFadeDuration = 0.3f;
    auto& text = asset.loadingText;
    text.enabled = true;
    text.text = language == LoadLinkedDefaultLanguage::Japanese ? "読み込み中…" : "Loading…";
    text.referenceSize = {1280, 720};
    text.position = {640, 648};
    text.pivot = {0.5f, 0.5f};
    text.layoutSize = {480, 64};
    text.fontFamily = "Yu Gothic UI";
    text.fontSize = 32;
    text.color = {1, 1, 1, 1};
    text.outlineEnabled = true;
    text.outlineColor = {0, 0, 0, 1};
    text.outlineWidth = 2;
    text.motion = LoadLinkedTextMotion::Pulse;
    text.pulsePeriod = 1.2f;
    text.minimumOpacity = 0.35f;
    return asset;
}
bool TryValidateLoadLinked(const LoadLinkedAsset& asset, LoadLinkedDiagnostics& diagnostics) {
    return LinkedGuard(diagnostics, [&] {
        ValidateLinkedScalars(asset);
        ValidateLinkedRelations(asset);
        ValidateLinkedGeometry(asset);
    });
}
bool TryDecodeTransitionBytes(std::string_view bytes, DecodedTransitionAsset& out, LoadLinkedDiagnostics& diagnostics) {
    return LinkedGuard(diagnostics, [&] {
        const auto root = ParseLinkedBytes(bytes);
        LinkedRequire(root.is_object(), LinkedCode::InvalidType, "asset", "objectが必要です");
        LinkedRequire(root.contains("version"), LinkedCode::MissingField, "version", "versionがありません");
        const auto& version = root.at("version");
        LinkedRequire(version.is_number_integer(), LinkedCode::InvalidType, "version", "整数versionが必要です");
        DecodedTransitionAsset result;
        if (version == 1) {
            try {
                Asset asset;
                try { asset = ReadAsset(root); }
                catch (const Json::exception& error) { LinkedFail(LinkedCode::InvalidType, "asset", error.what()); }
                result = CompiledTransitionV1Access::Compile(asset);
            } catch (const Failure& error) {
                LinkedFail(LinkedCode::InvalidType, error.diagnostic.field, error.diagnostic.message);
            }
        } else if (version == 2) {
            result = std::make_shared<const LoadLinkedAsset>(ReadLinkedAsset(root));
        } else LinkedFail(LinkedCode::UnsupportedVersion, "version", "未対応の整数versionです");
        static_assert(std::is_nothrow_move_assignable_v<DecodedTransitionAsset>);
        out = std::move(result);
    });
}
bool TryReadTransitionFile(const fs::path& capturedResourcesRoot, const std::string& logicalPath,
    DecodedTransitionAsset& out, LoadLinkedDiagnostics& diagnostics) {
    return LinkedGuard(diagnostics, [&] {
        LinkedRequire(capturedResourcesRoot.is_absolute() && fs::is_directory(capturedResourcesRoot),
            LinkedCode::InvalidPath, "path", "捕捉resources rootは実在する絶対directoryが必要です");
        const auto root = fs::canonical(capturedResourcesRoot);
        fs::path logical;
        try { logical = LogicalPath(logicalPath, "path", true); }
        catch (const Failure& error) { LinkedFail(LinkedCode::InvalidPath, error.diagnostic.field, error.diagnostic.message); }
        fs::path relative;
        auto part = logical.begin();
        for (++part; part != logical.end(); ++part) relative /= *part;
        const auto target = fs::weakly_canonical(root / relative);
        LinkedRequire(Inside(root, target), LinkedCode::InvalidPath, "path", "解決後のpathがresources境界外です");
        const auto bytes = ReadLinkedBytes(target);
        DecodedTransitionAsset candidate;
        LoadLinkedDiagnostics decodeDiagnostics;
        if (!TryDecodeTransitionBytes(bytes, candidate, decodeDiagnostics)) throw LinkedFailure{decodeDiagnostics.front()};
        static_assert(std::is_nothrow_move_assignable_v<DecodedTransitionAsset>);
        out = std::move(candidate);
    });
}
bool TryCreateEditorAssetFileContext(const fs::path& sourceRoot, std::uint64_t hostEpoch,
    EditorAssetContextPtr& out, LoadLinkedDiagnostics& diagnostics) {
    return LinkedGuard(diagnostics, [&] {
        LinkedRequire(hostEpoch != 0, LinkedCode::OutOfRange, "context.hostEpoch", "epochは0以外が必要です");
        const auto info = InspectLinkedProject(sourceRoot);
        std::shared_ptr<EditorAssetFileContext> result(new EditorAssetFileContext);
        result->kind_ = info.kind;
        result->hostEpoch_ = hostEpoch;
        result->projectId_ = info.projectId;
        result->descriptorPath_ = info.descriptor;
        result->sourceRoot_ = info.source;
        result->resourcesRoot_ = info.resources;
        out = std::move(result);
    });
}
bool TryReadLoadLinkedFile(const EditorAssetContextPtr& context, const std::string& logicalPath,
    LoadLinkedDiskSnapshotPtr& out, LoadLinkedDiagnostics& diagnostics) {
    return LinkedGuard(diagnostics, [&] {
        VerifyLinkedContext(context);
        auto resolved = ResolveLinkedTarget(context, logicalPath);
        auto bytes = ReadLinkedBytes(resolved.target);
        DecodedTransitionAsset decoded;
        LoadLinkedDiagnostics decodeDiagnostics;
        if (!TryDecodeTransitionBytes(bytes, decoded, decodeDiagnostics)) throw LinkedFailure{decodeDiagnostics.front()};
        const auto* value = std::get_if<std::shared_ptr<const LoadLinkedAsset>>(&decoded);
        LinkedRequire(value != nullptr, LinkedCode::UnsupportedVersion, "version", "V2専用の編集入口です");
        std::shared_ptr<LoadLinkedDiskSnapshot> result(new LoadLinkedDiskSnapshot);
        result->value_ = *value;
        result->bytes_ = std::move(bytes);
        result->logicalPath_ = std::move(resolved.logical);
        result->targetPath_ = std::move(resolved.target);
        result->context_ = context;
        out = std::move(result);
    });
}
LoadLinkedWriteResult WriteLoadLinkedFile(const EditorAssetContextPtr& context, const std::string& logicalPath,
    const LoadLinkedAsset& asset, const LoadLinkedWriteExpectation& expectation) noexcept {
    static_assert(std::is_nothrow_default_constructible_v<LoadLinkedWriteResult>);
    static_assert(std::is_nothrow_move_constructible_v<LoadLinkedWriteResult>);
    static_assert(std::is_nothrow_move_assignable_v<LoadLinkedWriteResult>);
    static_assert(std::is_nothrow_move_assignable_v<LoadLinkedDiskSnapshotPtr>);
    static_assert(std::is_nothrow_move_constructible_v<LoadLinkedDiagnostics>);
    static_assert(std::is_nothrow_move_assignable_v<LoadLinkedDiagnostics>);
    LoadLinkedWriteResult result;
    result.failureStage = LoadLinkedIoStage::Validate;
    try {
        LinkedRequire(!expectation.valueless_by_exception(), LinkedCode::InvalidType, "expectation", "保存期待値がありません");
        ValidateLinkedScalars(asset);
        ValidateLinkedRelations(asset);
        ValidateLinkedGeometry(asset);
        result.failureStage = LoadLinkedIoStage::Resolve;
        VerifyLinkedContext(context);
        auto resolved = ResolveLinkedTarget(context, logicalPath);
        if (const auto* match = std::get_if<LoadLinkedMatchSnapshot>(&expectation))
            LinkedRequire(match->snapshot && match->snapshot->Context() == context &&
                LinkedSamePath(match->snapshot->TargetPath(), resolved.target), LinkedCode::ExternalChange, "path", "保存基準が一致しません");
        result.failureStage = LoadLinkedIoStage::Serialize;
        auto bytes = WriteLinkedAsset(asset).dump(2);
        DecodedTransitionAsset decoded;
        LoadLinkedDiagnostics decodeDiagnostics;
        if (!TryDecodeTransitionBytes(bytes, decoded, decodeDiagnostics)) throw LinkedFailure{decodeDiagnostics.front()};
        result.failureStage = LoadLinkedIoStage::Allocate;
        std::shared_ptr<LoadLinkedDiskSnapshot> snapshot(new LoadLinkedDiskSnapshot);
        snapshot->value_ = std::get<std::shared_ptr<const LoadLinkedAsset>>(std::move(decoded));
        snapshot->bytes_ = std::move(bytes);
        snapshot->logicalPath_ = resolved.logical;
        snapshot->targetPath_ = resolved.target;
        snapshot->context_ = context;
        result.failureStage = LoadLinkedIoStage::Compare;
        CompareLinkedBaseline(context, resolved, expectation, result.failureStage);
        result.failureStage = LoadLinkedIoStage::CreateTemporary;
        static std::atomic<std::uint64_t> serial{0};
        TemporaryFile temporary;
        temporary.path = resolved.target;
        temporary.path += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(serial.fetch_add(1));
        temporary.handle = CreateFileW(temporary.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (temporary.handle == INVALID_HANDLE_VALUE) {
            result.systemError = GetLastError();
            LinkedFail(LinkedCode::IoFailure, "path", "排他的temp作成に失敗しました");
        }
        temporary.owned = true;
        result.failureStage = LoadLinkedIoStage::WriteTemporary;
        DWORD written = 0;
        const BOOL wrote = WriteFile(temporary.handle, snapshot->bytes_.data(), static_cast<DWORD>(snapshot->bytes_.size()), &written, nullptr);
        if (!wrote || written != snapshot->bytes_.size()) {
            result.systemError = wrote ? ERROR_WRITE_FAULT : GetLastError();
            LinkedFail(LinkedCode::IoFailure, "path", "temp書込みに失敗しました");
        }
        result.failureStage = LoadLinkedIoStage::FlushTemporary;
        if (!FlushFileBuffers(temporary.handle)) {
            result.systemError = GetLastError();
            LinkedFail(LinkedCode::IoFailure, "path", "temp flushに失敗しました");
        }
        result.failureStage = LoadLinkedIoStage::CloseTemporary;
        if (!CloseHandle(temporary.handle)) {
            result.systemError = GetLastError();
            LinkedFail(LinkedCode::IoFailure, "path", "temp closeに失敗しました");
        }
        temporary.handle = INVALID_HANDLE_VALUE;
        result.failureStage = LoadLinkedIoStage::Compare;
        CompareLinkedBaseline(context, resolved, expectation, result.failureStage);
        const DWORD flags = MOVEFILE_WRITE_THROUGH |
            (std::holds_alternative<LoadLinkedMatchSnapshot>(expectation) ? MOVEFILE_REPLACE_EXISTING : 0);
        // ここから先は保存結果が不確定になり得る。全返却情報は既に構築済み。
        result.failureStage = LoadLinkedIoStage::Rename;
        result.status = LoadLinkedWriteStatus::Indeterminate;
        if (!MoveFileExW(temporary.path.c_str(), resolved.target.c_str(), flags)) {
            result.systemError = GetLastError();
            SetLinkedWriteDiagnostic(result, LinkedCode::IoFailure, "path", "renameの成功を確認できません");
            return result;
        }
        temporary.owned = false;
        static_assert(noexcept(result.snapshot = std::move(snapshot)));
        result.snapshot = std::move(snapshot);
        result.status = LoadLinkedWriteStatus::Written;
        result.failureStage = LoadLinkedIoStage::None;
        result.systemError = 0;
        return result;
    } catch (const LinkedFailure& error) {
        SetLinkedWriteDiagnostic(result, error.diagnostic.code, error.diagnostic.field.c_str(), error.diagnostic.detail.c_str());
    } catch (const std::bad_alloc&) {
        SetLinkedWriteDiagnostic(result, LinkedCode::ResourceFailure, "asset", "メモリを確保できません");
    } catch (const fs::filesystem_error& error) {
        SetLinkedWriteDiagnostic(result, LinkedCode::IoFailure, "path", error.what());
    } catch (const std::exception& error) {
        SetLinkedWriteDiagnostic(result, LinkedCode::InternalFailure, "asset", error.what());
    } catch (...) {
        SetLinkedWriteDiagnostic(result, LinkedCode::InternalFailure, "asset", "非標準例外が発生しました");
    }
    return result;
}

} // namespace SceneTransitionAsset
