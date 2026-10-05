// 役割: SceneTransition Componentを評価し、遷移要求を生成する。
#pragma once

#include <string>

class SceneDocument;

// 状態を保持せず、入力条件を満たした遷移先Scene IDだけを返す。
class SceneTransitionSystem {
public:
	std::string Update(const SceneDocument& document) const;
};
