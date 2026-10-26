// 役割: SceneTransition Componentを評価し、遷移要求を生成する。
#pragma once

#include <string>

class SceneDocument;

// 状態を保持せず、入力条件を満たした遷移先Scene IDだけを返す。
class SceneTransitionSystem {
public:
	// 入力遮断中はKey遷移だけを評価しない。旧callerの既定動作は維持する。
	std::string Update(const SceneDocument& document, bool acceptGameplayInput = true) const;
};
