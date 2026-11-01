// 役割: Playerの攻撃・被弾設定を値コピーで解決・表示・検証するEditor専用の入口。
// DocumentやSessionを所有せず、要求にEntity/Componentへのポインタを保持しない。
#pragma once
#include "EditorLocalization.h"
#include "../scene/SceneDocument.h"
#include <cstdint>
#include <string>
#include <vector>

namespace PlayerCombatEditor {
enum class IssueCode {
	None, Stale, Locked, Playing, Missing, Ambiguous, Competing,
	Legacy, UnknownAction, Disabled, ReceiverElsewhere, ReceiverMissing,
	BodyMissing, FrozenAxes, HealthMissing, InvalidTime, InvalidDistance,
	InvalidReaction, InvalidState, InvalidTarget, InvalidChoice, AddFailed
};
struct Issue { IssueCode code = IssueCode::None; std::string detail; };

struct DocumentToken {
	bool prefab = false;
	std::string key;
	uint64_t epoch = 0, generation = 0, revision = 0;
};
struct Subject {
	uint64_t actorId = 0, entityId = 0, componentId = 0;
	std::string componentType, attackName;
};
enum class Operation { None, EditAttack, EditReaction, AddReaction, Inspect, OpenPrefab, ReturnToActor };
enum Field : uint64_t {
	Windup = 1ull << 0, ActiveTime = 1ull << 1, Recovery = 1ull << 2,
	Forward = 1ull << 3, Side = 1ull << 4, Easing = 1ull << 5,
	Facing = 1ull << 6, FacingTarget = 1ull << 7, Angle = 1ull << 8,
	Enabled = 1ull << 9, Multiplier = 1ull << 10, Duration = 1ull << 11,
	Trigger = 1ull << 12, MinimumPoise = 1ull << 13, PoiseStat = 1ull << 14,
	PoiseDelay = 1ull << 15, State = 1ull << 16
};
struct AttackValues {
	float windup = 0, activeTime = 0, recovery = 0, forward = 0, side = 0, angle = 0;
	std::string easing, facing, targetName;
	uint64_t targetId = 0;
};
struct ReactionValues {
	bool enabled = false;
	float multiplier = 0, duration = 0, minimumPoise = 0, poiseDelay = 0;
	std::string trigger, poiseStat, state;
};
struct Request {
	DocumentToken token;
	Subject subject;
	Operation operation = Operation::None;
	uint64_t fields = 0, serial = 0;
	AttackValues attack;
	ReactionValues reaction;
};
struct EntityChoice { uint64_t id = 0; std::string name; };
struct AttackModel {
	Subject subject;
	AttackValues values;
	std::string ownerName, animation, sourcePrefab;
	std::vector<std::string> sourceStates;
	std::vector<EntityChoice> targets;
	Issue issue;
	bool editable = false;
};
struct ReactionModel {
	Subject subject;
	ReactionValues values;
	std::vector<std::string> states;
	std::vector<Issue> issues;
	bool exists = false, editable = false, canAdd = false;
};
struct ActorModel {
	uint64_t id = 0;
	std::string name;
	std::vector<AttackModel> attacks;
	std::vector<Issue> issues;
	std::vector<EntityChoice> hurtBoxes, receivers;
	ReactionModel reaction;
};
// ImGuiManagerがDocument/Entityごとに保持する表示選択。保存データやUndoへは含めない。
struct Selection { bool initialized = false; uint64_t attackOwner = 0; std::string attackName; };

// 参照解決は読取のみ。曖昧な名前を先頭一致で編集先に採用しない。
ActorModel ResolveActor(const SceneDocument& document, uint64_t actorId);
AttackModel ResolveAttack(const SceneDocument& document, uint64_t entityId, const std::string& name);
ReactionModel ResolveReaction(const SceneDocument& document, uint64_t entityId);
std::string DescribeIssue(const Issue& issue, EditorLanguage language);

// 呼出元のID階層の内側で描画し、値型要求だけを返す。原本/選択Entity/Undoは変更しない。
std::vector<Request> DrawActor(const ActorModel& model, Selection& selection,
	EditorLanguage language, bool editable);
Request DrawAttackFields(const AttackModel& model, EditorLanguage language, bool editable);
Request DrawReactionFields(const ReactionModel& model, EditorLanguage language, bool editable, bool showEnabled = true);
void DrawReference(EditorLanguage language);

// 全検証後に指定fieldのみ更新。失敗時は不変。MarkDirty/Undo/Reloadは呼出元の責務。
// 追加/Inspector移動/Asset操作は対象外で、ImGuiManagerが既存Session経由で実行する。
bool ValidateAndApplyValueEdit(SceneDocument& document, const DocumentToken& current,
	const Request& request, bool editingAllowed, bool& changed, Issue& issue);
bool Matches(const DocumentToken& a, const DocumentToken& b);
}
