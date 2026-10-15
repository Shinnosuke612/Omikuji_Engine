// 役割: 遷移演出Assetの厳密なJSON IO、決定的な凸分割と副作用のない時間評価を実装する。
#include "SceneTransitionAsset.h"
#include "../math/Math.h"
#include "../utility/StringUtility.h"
#include "../utility/EditableResourcePath.h"
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
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace SceneTransitionAsset {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
constexpr double DistanceTolerance = 1e-6, AreaTolerance = 1e-10, AreaSumTolerance = 1e-5;
struct Failure { Diagnostic diagnostic; };
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
Geometry Build(const std::vector<Cut>& cuts) {
    Require(!cuts.empty() && cuts.size() <= MaxCuts, "cuts", "cut数は1〜16です");
    const std::vector<Point> rectangle{{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    std::vector<Polygon> polygons{{0, rectangle}};
    std::vector<Plane> planes;
    std::set<std::string> ids;
    Geometry geometry;
    for (std::size_t i = 0; i < cuts.size(); ++i) {
        const auto& cut = cuts[i]; const auto field = At("cuts", i);
        Id(cut.id, field + ".id"); Require(ids.insert(cut.id).second, field + ".id", "idが重複しています");
        const Point a{cut.from.x, cut.from.y}, b{cut.to.x, cut.to.y};
        Require(std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(b.x) && std::isfinite(b.y), field, "切断座標は有限値が必要です");
        const Point direction = Sub(b, a); const double length = std::hypot(direction.x, direction.y);
        Require(length > 0, field, "長さ0の切断線です");
        const Plane plane{-direction.y / length, direction.x / length,
            (direction.y * a.x - direction.x * a.y) / length};
        for (Plane previous : planes) {
            const bool same = std::hypot(plane.x - previous.x, plane.y - previous.y) <= DistanceTolerance && std::abs(plane.c - previous.c) <= DistanceTolerance;
            const bool reverse = std::hypot(plane.x + previous.x, plane.y + previous.y) <= DistanceTolerance && std::abs(plane.c + previous.c) <= DistanceTolerance;
            Require(!same && !reverse, field, "同一または反転した切断線が重複しています");
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
        Require(intersections.size() >= 2 && !Near(intersections.front(), intersections.back()), field, "画面を横切る区間がありません");
        geometry.cutSegments.push_back({ToVector(intersections.front()), ToVector(intersections.back())});
        std::vector<Polygon> next;
        std::size_t splits = 0;
        const auto bit = static_cast<std::uint16_t>(1u << i);
        for (const auto& polygon : polygons) {
            bool left = false, right = false;
            for (Point p : polygon.points) { const double d = Distance(plane, p); left |= d > 0; right |= d < 0; }
            Require(left || right, field, "許容差内で退化したpolygonです");
            if (left && right) {
                auto l = Clip(polygon.points, plane, true), r = Clip(polygon.points, plane, false);
                Require(l.size() >= 3 && r.size() >= 3 && Area(l) > AreaTolerance && Area(r) > AreaTolerance,
                    field, "切断で退化したpolygonが生成されます");
                next.push_back({static_cast<std::uint16_t>(polygon.id | bit), std::move(l)});
                next.push_back({polygon.id, std::move(r)}); ++splits;
            } else next.push_back({static_cast<std::uint16_t>(polygon.id | (left ? bit : 0)), polygon.points});
            Require(next.size() <= MaxFragments, field, "断片数上限を超えています");
        }
        Require(splits > 0, field, "境界接触のみ、または断片を増やさないcutです");
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
        Require(std::isfinite(area) && area > AreaTolerance, "geometry", "出力polygonの面積が退化しています");
        Point center{0, 0};
        for (std::size_t i = 0; i < output.size(); ++i) {
            const Point p = output[i], q = output[(i + 1) % output.size()]; const double weight = Cross(p, q);
            center.x += (p.x + q.x) * weight; center.y += (p.y + q.y) * weight;
        }
        fragment.centroid = ToVector({center.x / (6 * area), center.y / (6 * area)});
        fragment.area = static_cast<float>(area); totalArea += area;
        fragment.firstVertex = geometry.vertices.size(); fragment.firstIndex = geometry.indices.size();
        Require(geometry.vertices.size() + output.size() <= MaxVertices &&
            geometry.indices.size() + 3 * (output.size() - 2) <= MaxIndices, "geometry", "頂点/index容量を超えています");
        for (Vector2 uv : fragment.polygon) geometry.vertices.push_back({uv, polygon.id});
        for (std::size_t i = 1; i + 1 < output.size(); ++i) {
            Require(Cross(Sub(output[i], output[0]), Sub(output[i + 1], output[0])) * 0.5 > AreaTolerance,
                "geometry", "triangleの面積または向きが不正です");
            for (std::size_t local : {std::size_t{0}, i, i + 1}) {
                const auto index = fragment.firstVertex + local;
                Require(index < geometry.vertices.size(), "geometry", "indexが頂点範囲外です");
                geometry.indices.push_back(static_cast<std::uint32_t>(index));
            }
        }
        fragment.indexCount = geometry.indices.size() - fragment.firstIndex;
        geometry.fragments.push_back(std::move(fragment));
    }
    Require(std::abs(totalArea - 1) <= AreaSumTolerance && geometry.indices.size() / 3 <= MaxTriangles,
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
} // namespace

bool TryBuildGeometry(const std::vector<Cut>& cuts, Geometry& geometry, Diagnostics& diagnostics) {
    return Guard(diagnostics, [&] { auto result = Build(cuts); geometry = std::move(result); });
}
bool TryCompile(const Asset& asset, std::shared_ptr<const CompiledTransition>& compiled, Diagnostics& diagnostics) {
    return Guard(diagnostics, [&] {
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
        compiled = std::move(result);
    });
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
} // namespace SceneTransitionAsset
