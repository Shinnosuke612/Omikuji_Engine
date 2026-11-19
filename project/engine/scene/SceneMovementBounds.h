// 役割: プレイヤー移動範囲の保存値と、ワールドXZ上の形状計算を定義する。
#pragma once

#include "../math/Vector2.h"
#include "../math/Vector4.h"
#include <array>
#include <cstddef>
#include <string>

struct ColliderSweepShape;
struct SceneEntity;
class SceneDocument;

struct SceneMovementBoundsSettings {
	std::string shape = "Box";
	Vector2 center{}, halfExtents{25.0f, 25.0f};
	float radius = 25.0f;
	bool showBoundary = true;
	float revealDistance = 4.0f, boundaryY = 0.03f, lineWidth = 0.08f;
	Vector4 color{0.2f, 0.85f, 1.0f, 0.85f};
};

struct SceneMovementBoundsPlacement {
	Vector2 correction{};
	double clearance = 0.0;
	std::array<Vector2, 8> outwardNormals{};
	std::size_t normalCount = 0;
};

// 描画へ渡す値コピー。空のframeは描画不要。
struct SceneMovementBoundsFrame {
	SceneMovementBoundsSettings settings;
	float opacity = 0.0f;
};

namespace SceneMovementBounds {
	// Editor/保存読込/Runtimeで同じ数値条件を使う。nullptrは正常。
	const char* SettingsIssue(const SceneMovementBoundsSettings& settings);
	// Scene上の対象と必須Componentを照合する。実寸はCollider構築後にSolveで検証する。
	const char* TargetIssue(const SceneDocument& document, const SceneEntity& owner);
	// 保存された姿勢と移動Colliderで、有効化前に収容可能か確認する。
	const char* AuthoringIssue(const SceneDocument& document, const SceneEntity& owner,
		const SceneMovementBoundsSettings& settings);
	// 実ColliderのXZ投影を収める補正を返す。形状が収まらない場合はfalse。
	bool Solve(const SceneMovementBoundsSettings& settings, const ColliderSweepShape& shape,
		SceneMovementBoundsPlacement& out);
	// 全接触面に対して外向きでない、元の速度に最も近いXZ速度。
	Vector2 RestrictVelocity(const SceneMovementBoundsPlacement& placement, Vector2 velocity);
}
