// 役割: Scene／Prefabで共有するComponent Catalogと既存候補順を定義する。
#include "EditorComponentCatalog.h"

#include <algorithm>
#include <array>

namespace {
	constexpr uint8_t SceneContext =
		static_cast<uint8_t>(EditorComponentContext::Scene);
	constexpr uint8_t PrefabContext =
		static_cast<uint8_t>(EditorComponentContext::Prefab);
	constexpr uint8_t SceneAndPrefabContext = SceneContext | PrefabContext;
	constexpr std::array<EditorComponentTag, 15> kTags = {{
		EditorComponentTag::TwoD,
		EditorComponentTag::ThreeD,
		EditorComponentTag::UI,
		EditorComponentTag::Camera,
		EditorComponentTag::Physics,
		EditorComponentTag::Collision,
		EditorComponentTag::Combat,
		EditorComponentTag::Player,
		EditorComponentTag::Enemy,
		EditorComponentTag::Animation,
		EditorComponentTag::Event,
		EditorComponentTag::Spawn,
		EditorComponentTag::Reference,
		EditorComponentTag::PostEffect,
		EditorComponentTag::Prefab
	}};

	constexpr std::array<EditorComponentDefinition, 51> kDefinitions = {{
		{
			"GameFlowDirector", "ゲーム進行", "Game Flow Director",
			"開始案内、敵出現待ち、カウントダウン、Phaseとクリア時間を設定します。初回戦闘開始をEventへ通知します。",
			"Configures briefing, enemy preparation, countdown, phases and clear time. Emits the initial battle start event.",
			EditorComponentCategory::EventAndFlow,
			SceneContext, 43, -1, "",
			EditorComponentTagBit(EditorComponentTag::Event) | EditorComponentTagBit(EditorComponentTag::Spawn)
		},

		{
			"UICanvas", "UI画面", "UI Canvas",
			"制作画面の文字・画像・位置を表示領域いっぱいに拡縮します。",
			"Stretches the authored text, images and positions to fill the view.",
			EditorComponentCategory::Rendering,
			SceneAndPrefabContext, 41, 41, "",
			EditorComponentTagBit(EditorComponentTag::UI) | EditorComponentTagBit(EditorComponentTag::TwoD)
		},
		{
			"UITransform", "UI配置", "UI Layout",
			"親の枠を基準に位置・表示枠・基点を編集します。",
			"Edits position, bounds and pivot relative to the parent UI bounds.",
			EditorComponentCategory::Rendering,
			SceneAndPrefabContext, 42, 42, "",
			EditorComponentTagBit(EditorComponentTag::UI) | EditorComponentTagBit(EditorComponentTag::TwoD)
		},
		{
			"MeshRenderer", "3Dモデル表示", "Mesh Renderer",
			"3DモデルとMaterialをSceneへ表示します。",
			"Displays a 3D model and its materials in the scene.",
			EditorComponentCategory::Rendering,
			SceneAndPrefabContext, 0, 0, "",
			EditorComponentTagBit(EditorComponentTag::ThreeD) |
			EditorComponentTagBit(EditorComponentTag::Prefab)
		},
		{
			"Environment", "環境", "Environment",
			"Skyboxと環境反射の設定を保持します。",
			"Stores skybox and environment reflection settings.",
			EditorComponentCategory::World,
			SceneContext, 1, -1, "",
			EditorComponentTagBit(EditorComponentTag::ThreeD)
		},
		{
			"SpriteRenderer", "2D画像表示", "Sprite Renderer",
			"画像を2D空間または画面上へ表示します。",
			"Displays an image in 2D space or on screen.",
			EditorComponentCategory::Rendering,
			SceneAndPrefabContext, 2, 38, "",
			EditorComponentTagBit(EditorComponentTag::TwoD) |
			EditorComponentTagBit(EditorComponentTag::UI)
		},
		{
			"TextRenderer", "文字表示", "Text Renderer",
			"日本語を含む文字を画面またはScene内へ表示します。",
			"Displays text, including Japanese, on screen or in the scene.",
			EditorComponentCategory::Rendering,
			SceneAndPrefabContext, 3, 39, "",
			EditorComponentTagBit(EditorComponentTag::TwoD) |
			EditorComponentTagBit(EditorComponentTag::UI)
		},
		{
			"TextMotion", "文字演出", "Text Motion",
			"TextRendererに対する再利用可能な2D文字演出clipを設定します。",
			"Configures reusable 2D text motion clips for a TextRenderer.",
			EditorComponentCategory::Animation,
			SceneAndPrefabContext, 37, 40, "TextRenderer",
			EditorComponentTagBit(EditorComponentTag::TwoD) |
			EditorComponentTagBit(EditorComponentTag::UI) |
			EditorComponentTagBit(EditorComponentTag::Animation) |
			EditorComponentTagBit(EditorComponentTag::Event)
		},
		{
			"Camera", "カメラ", "Camera",
			"Sceneを描画する視点と投影設定を保持します。",
			"Stores the viewpoint and projection settings used to render a scene.",
			EditorComponentCategory::Camera,
			SceneContext, 4, -1, "",
			EditorComponentTagBit(EditorComponentTag::ThreeD) |
			EditorComponentTagBit(EditorComponentTag::Camera)
		},
		{
			"Light", "ライト", "Light",
			"Directional、Point、Spot Lightを設定します。",
			"Configures a directional, point, or spot light.",
			EditorComponentCategory::World,
			SceneContext, 5, -1, "",
			EditorComponentTagBit(EditorComponentTag::ThreeD)
		},
		{
			"MonitorRenderer", "モニター表示", "Monitor Renderer",
			"別Cameraの映像をScene内の面へ表示します。",
			"Displays another camera view on a surface in the scene.",
			EditorComponentCategory::Rendering,
			SceneContext, 6, -1, "",
			EditorComponentTagBit(EditorComponentTag::ThreeD) |
			EditorComponentTagBit(EditorComponentTag::Camera)
		},
		{
			"CameraSwitcher", "カメラ切替", "Camera Switcher",
			"複数Cameraの手動切替設定を保持します。",
			"Stores manual switching settings for multiple cameras.",
			EditorComponentCategory::Camera,
			SceneContext, 7, -1, "",
			EditorComponentTagBit(EditorComponentTag::Camera)
		},
		{
			"ThirdPersonCamera", "三人称カメラ", "Third Person Camera",
			"対象を追従する三人称Cameraの動作を設定します。",
			"Configures a third-person camera that follows a target.",
			EditorComponentCategory::Camera,
			SceneContext, 8, -1, "Camera",
			EditorComponentTagBit(EditorComponentTag::Camera) |
			EditorComponentTagBit(EditorComponentTag::Player)
		},
		{
			"EntityReference", "Entity参照", "Entity Reference",
			"別Entityへの名前とID参照を保持します。",
			"Stores a name and ID reference to another entity.",
			EditorComponentCategory::EventAndFlow,
			SceneContext, 9, -1, "",
			EditorComponentTagBit(EditorComponentTag::Reference) |
			EditorComponentTagBit(EditorComponentTag::Event)
		},
		{
			"SceneTransition", "Scene遷移", "Scene Transition",
			"移動先Sceneと遷移条件の設定を保持します。",
			"Stores destination scene and transition settings.",
			EditorComponentCategory::EventAndFlow,
			SceneContext, 10, -1, "",
			EditorComponentTagBit(EditorComponentTag::Event)
		},
		{
			"CameraPath", "カメラ経路", "Camera Path",
			"複数Pointを使ったCamera移動経路を定義します。",
			"Defines camera movement through multiple path points.",
			EditorComponentCategory::Camera,
			SceneContext, 11, -1, "",
			EditorComponentTagBit(EditorComponentTag::Camera) |
			EditorComponentTagBit(EditorComponentTag::Animation)
		},
		{
			"CameraPathPoint", "カメラ経路Point", "Camera Path Point",
			"CameraPath上の位置、時間、補間を設定します。",
			"Configures position, duration, and easing on a camera path.",
			EditorComponentCategory::Camera,
			SceneContext, 12, -1, "",
			EditorComponentTagBit(EditorComponentTag::Camera)
		},
		{
			"PhysicsBody", "物理Body", "Physics Body",
			"重力、速度、固定軸などの物理状態を設定します。",
			"Configures gravity, velocity, and constrained axes for physics.",
			EditorComponentCategory::Physics,
			SceneContext, 13, -1, "",
			EditorComponentTagBit(EditorComponentTag::Physics)
		},
		{
			"PlayerBehavior", "プレイヤー操作", "Player Behavior",
			"プレイヤーの移動と基本入力動作を設定します。",
			"Configures player movement and basic input behavior.",
			EditorComponentCategory::Gameplay,
			SceneContext, 14, -1, "",
			EditorComponentTagBit(EditorComponentTag::Player)
		},
		{
			"PlayerMovementBounds", "プレイヤー移動範囲", "Player Movement Bounds",
			"移動用当たり判定をワールドXZの範囲内に収め、接近時に境界を表示します。",
			"Contains the movement collider within a world XZ area and reveals the boundary nearby.",
			EditorComponentCategory::Gameplay,
			SceneContext, 50, -1, "PlayerBehavior",
			EditorComponentTagBit(EditorComponentTag::Player) | EditorComponentTagBit(EditorComponentTag::Physics)
		},
		{
			"AgentBehavior", "Agent移動", "Agent Behavior",
			"Agentの移動方式、速度、群れ設定を保持します。",
			"Stores agent movement, speed, and crowd settings.",
			EditorComponentCategory::Gameplay,
			SceneContext, 15, -1, "",
			EditorComponentTagBit(EditorComponentTag::Enemy)
		},
		{
			"AgentAttractor", "Agent誘導点", "Agent Attractor",
			"Agentを引き寄せる位置と強さを設定します。",
			"Configures a position and strength that attracts agents.",
			EditorComponentCategory::Gameplay,
			SceneContext, 16, -1, "",
			EditorComponentTagBit(EditorComponentTag::Enemy)
		},
		{
			"WaterVolume", "水中領域", "Water Volume",
			"水面表示、水中効果、移動補正の領域を設定します。",
			"Configures a volume for water rendering, underwater effects, and movement.",
			EditorComponentCategory::World,
			SceneContext, 17, -1, "",
			EditorComponentTagBit(EditorComponentTag::ThreeD) |
			EditorComponentTagBit(EditorComponentTag::Physics)
		},
		{
			"Animator", "Animator", "Animator",
			"Model Animationの再生設定を保持します。",
			"Stores model animation playback settings.",
			EditorComponentCategory::Animation,
			SceneAndPrefabContext, 18, 1, "",
			EditorComponentTagBit(EditorComponentTag::Animation) |
			EditorComponentTagBit(EditorComponentTag::Prefab)
		},
		{
			"OBBCollider", "当たり判定", "Box Collider",
			"向きを持つ箱型の接触・Trigger判定を追加します。",
			"Adds an oriented box for collision or trigger detection.",
			EditorComponentCategory::Physics,
			SceneAndPrefabContext, 19, 2, "",
			EditorComponentTagBit(EditorComponentTag::Physics) |
			EditorComponentTagBit(EditorComponentTag::Collision) |
			EditorComponentTagBit(EditorComponentTag::Prefab)
		},
		{
			"DeathReplay", "死亡後のスロー", "Death Slow Motion",
			"死亡後のスロー、3視点リプレイ、最後の画面保持を設定します。完了はUI Eventでメニューへ接続できます。", "Configures death slow motion, three-view replay and final-frame hold. A UI completion event can open a menu.",
			EditorComponentCategory::Gameplay, SceneAndPrefabContext, 47, 47, "",
			EditorComponentTagBit(EditorComponentTag::Combat)
		},
		{
			"DeathResponse", "死亡時の対応", "Death Response",
			"Healthの死亡確定・停止範囲・演出開始を設定します。", "Configures death, participation stop and presentation start.",
			EditorComponentCategory::Gameplay, SceneAndPrefabContext, 46, 46, "",
			EditorComponentTagBit(EditorComponentTag::Combat)
		},
		{
			"StatTextDisplay", "能力値表示", "Stat Text Display",
			"StatSetの現在値を同じEntityの文字に表示します。", "Displays a StatSet value using this Entity's text.",
			EditorComponentCategory::Gameplay, SceneAndPrefabContext, 48, 48, "",
			EditorComponentTagBit(EditorComponentTag::UI)
		},
		{
			"RunUpgrade", "プレイ中の強化定義", "Run Upgrade Definitions",
			"Playerの強化候補を定義します。フェーズごとの三択から取得し、プレイ終了時に初期化します。",
			"Defines Player upgrades acquired from phase choices and reset at run end.",
			EditorComponentCategory::Gameplay, SceneAndPrefabContext, 49, 49, "",
			EditorComponentTagBit(EditorComponentTag::Combat) | EditorComponentTagBit(EditorComponentTag::Player)
		},
		{
			"UpgradeSelection", "強化の三択画面", "Upgrade Selection",
			"対象Player、三つのカード、決定ボタンを接続します。MenuPanelとGamePauseを併用します。",
			"Connects the Player, three cards and a confirm button using MenuPanel and GamePause.",
			EditorComponentCategory::Gameplay, SceneAndPrefabContext, 50, 50, "",
			EditorComponentTagBit(EditorComponentTag::UI)
		},
		{
			"Health", "被弾・無敵", "Health",
			"StatSetのHPへの被弾受付と、攻撃者共通の無敵時間を設定します。",
			"Configures HP damage acceptance and invulnerability shared across attackers.",
			EditorComponentCategory::Gameplay,
			SceneAndPrefabContext, 44, 44, "",
			EditorComponentTagBit(EditorComponentTag::Combat)
		},
		{
			"StatSet", "能力値", "Stat Set",
			"HPやPoiseなどの値と範囲を定義します。",
			"Defines values and ranges such as HP and poise.",
			EditorComponentCategory::Gameplay,
			SceneAndPrefabContext, 20, 45, "",
			EditorComponentTagBit(EditorComponentTag::Combat)
		},
		{
			"StateMachine", "状態管理", "State Machine",
			"状態とAction IDによる遷移を設定します。",
			"Configures states and transitions driven by action IDs.",
			EditorComponentCategory::Gameplay,
			SceneAndPrefabContext, 21, 9, "",
			EditorComponentTagBit(EditorComponentTag::Animation) |
			EditorComponentTagBit(EditorComponentTag::Event) |
			EditorComponentTagBit(EditorComponentTag::Prefab)
		},
		{
			"EventTrigger", "イベントTrigger", "Event Trigger",
			"開始、入力、状態条件からActionを要求します。",
			"Requests actions from start, input, or state conditions.",
			EditorComponentCategory::EventAndFlow,
			SceneAndPrefabContext, 22, 12, "",
			EditorComponentTagBit(EditorComponentTag::Event)
		},
		{
			"MenuPanel", "メニュー画面", "Menu Panel",
			"制作サイズ・選択肢・標準キー/マウス操作を設定します。マウスは項目の枠内で押して離すと決定。表示開始条件から死亡リプレイ完了にも接続できます。",
			"Configure reference size, items and keyboard/mouse controls. Mouse confirmation requires pressing and releasing inside an item's bounds. Open Conditions can connect death replay completion.",
			EditorComponentCategory::EventAndFlow,
			SceneAndPrefabContext, 38, 13, "",
			EditorComponentTagBit(EditorComponentTag::UI) | EditorComponentTagBit(EditorComponentTag::Event)
		},
		{
			"MenuItem", "メニュー項目", "Menu Item",
			"標準メニューでは配置・文字・画像背景・選択表示・決定処理を一か所で編集します。必要な補助Componentを同じEntityに用意します。",
			"Edit placement, text, image background, selection visuals and confirmation together, using helper components on the same entity.",
			EditorComponentCategory::EventAndFlow,
			SceneAndPrefabContext, 39, 14, "",
			EditorComponentTagBit(EditorComponentTag::UI) | EditorComponentTagBit(EditorComponentTag::Event)
		},
		{
			"GamePause", "ゲーム内ポーズ", "Game Pause",
			"ゲーム内ポーズ画面とポーズ中のBGM方針を設定します。",
			"Configures the in-game pause panel and BGM pause policy.",
			EditorComponentCategory::EventAndFlow,
			SceneContext, 40, -1, "",
			EditorComponentTagBit(EditorComponentTag::UI) | EditorComponentTagBit(EditorComponentTag::Event)
		},
		{
			"AudioSource", "Audio Source", "Audio Source",
			"2Dまたは3D Audio ClipのBus、再生開始、Loopを設定します。",
			"Configures a 2D or 3D audio clip, bus, start playback, and loop.",
			EditorComponentCategory::World,
			SceneAndPrefabContext, 23, 10, "",
			EditorComponentTagBit(EditorComponentTag::TwoD) |
			EditorComponentTagBit(EditorComponentTag::ThreeD) |
			EditorComponentTagBit(EditorComponentTag::Event) |
			EditorComponentTagBit(EditorComponentTag::Prefab)
		},
		{
			"AudioListener", "Audio Listener", "Audio Listener",
			"3D Audioの聴取位置と向きを、CameraまたはEntityから決定します。",
			"Selects the Camera or Entity pose used to hear 3D audio.",
			EditorComponentCategory::World,
			SceneContext, 24, -1, "",
			EditorComponentTagBit(EditorComponentTag::ThreeD) |
			EditorComponentTagBit(EditorComponentTag::Camera)
		},
		{
			"PostProcessProfileManager", "PostEffect Profile管理", "Post Process Profile Manager",
			"複数のPostEffect設定と切替対象を保持します。",
			"Stores multiple post-process settings and selectable profiles.",
			EditorComponentCategory::Rendering,
			SceneContext, 23, -1, "",
			EditorComponentTagBit(EditorComponentTag::PostEffect)
		},
		{
			"PrefabAnimator", "Prefab Pose Animation", "Prefab Animator",
			"Prefab EntityのTransform Pose Clipを再生します。",
			"Plays transform pose clips for prefab entities.",
			EditorComponentCategory::Animation,
			SceneAndPrefabContext, 24, 6, "",
			EditorComponentTagBit(EditorComponentTag::Animation) |
			EditorComponentTagBit(EditorComponentTag::Prefab)
		},
		{
			"Faction", "陣営", "Faction",
			"戦闘判定に使用する所属Teamを設定します。",
			"Configures the team used by combat filtering.",
			EditorComponentCategory::Gameplay,
			SceneAndPrefabContext, 25, 8, "",
			EditorComponentTagBit(EditorComponentTag::Combat) |
			EditorComponentTagBit(EditorComponentTag::Prefab)
		},
		{
			"HitBox", "攻撃判定", "Hit Box",
			"攻撃中のDamageとKnockback情報を保持します。",
			"Stores damage and knockback data used during attacks.",
			EditorComponentCategory::Physics,
			SceneAndPrefabContext, 26, 3, "",
			EditorComponentTagBit(EditorComponentTag::Combat) |
			EditorComponentTagBit(EditorComponentTag::Collision) |
			EditorComponentTagBit(EditorComponentTag::Prefab)
		},
		{
			"HurtBox", "被攻撃判定", "Hurt Box",
			"攻撃を受ける領域とDamage倍率を設定します。",
			"Configures a hittable area and its damage multiplier.",
			EditorComponentCategory::Physics,
			SceneAndPrefabContext, 27, 4, "",
			EditorComponentTagBit(EditorComponentTag::Combat) |
			EditorComponentTagBit(EditorComponentTag::Collision) |
			EditorComponentTagBit(EditorComponentTag::Prefab)
		},
		{
			"HitReaction", "被弾Reaction", "Hit Reaction",
			"Poise、Knockback、被弾Stateの反応を設定します。",
			"Configures poise, knockback, and hit-state reactions.",
			EditorComponentCategory::Gameplay,
			SceneContext, 28, -1, "",
			EditorComponentTagBit(EditorComponentTag::Combat)
		},
		{
			"DeathPresentation", "死亡演出", "Death Presentation",
			"死亡State、非Active化までの時間、Effectを設定します。",
			"Configures death state, deactivation delay, and effects.",
			EditorComponentCategory::Gameplay,
			SceneContext, 29, -1, "",
			EditorComponentTagBit(EditorComponentTag::Combat) |
			EditorComponentTagBit(EditorComponentTag::Enemy)
		},
		{
			"BoneAttachment", "Bone追従", "Bone Attachment",
			"EntityをModelのBoneへ追従させます。",
			"Attaches an entity to a bone in a model.",
			EditorComponentCategory::Animation,
			SceneAndPrefabContext, 30, 5, "",
			EditorComponentTagBit(EditorComponentTag::Animation) |
			EditorComponentTagBit(EditorComponentTag::Prefab)
		},
		{
			"EnemyBehavior", "敵の基本行動", "Enemy Behavior",
			"対象の探し方、位置取り・個体差・体当たり、任意の姿勢演出と中断条件を設定します。",
			"Configures target selection, positioning, variation, charge attacks, optional poses and interruption conditions.",
			EditorComponentCategory::Gameplay,
			SceneAndPrefabContext, 31, 31, "",
			EditorComponentTagBit(EditorComponentTag::Combat) |
			EditorComponentTagBit(EditorComponentTag::Enemy)
		},
		{
			"EnemySpawner", "敵の生成", "Enemy Spawner",
			"Prefabから敵を生成し、最大数と再生成間隔を管理します。",
			"Spawns enemies from a prefab and controls limits and timing.",
			EditorComponentCategory::Gameplay,
			SceneContext, 32, -1, "",
			EditorComponentTagBit(EditorComponentTag::Enemy) |
			EditorComponentTagBit(EditorComponentTag::Spawn)
		},
		{
			"Projectile", "飛翔体", "Projectile",
			"飛翔方向、速度、寿命、命中時Damageを設定します。",
			"Configures projectile direction, speed, lifetime, and hit damage.",
			EditorComponentCategory::Physics,
			SceneContext, 33, -1, "",
			EditorComponentTagBit(EditorComponentTag::Combat) |
			EditorComponentTagBit(EditorComponentTag::Physics)
		},
		{
			"AttackSet", "攻撃Set", "Attack Set",
			"攻撃定義、Hit Window、Effect Eventをまとめて保持します。",
			"Stores attack definitions, hit windows, and effect events.",
			EditorComponentCategory::Animation,
			PrefabContext, -1, 7, "",
			EditorComponentTagBit(EditorComponentTag::Combat) |
			EditorComponentTagBit(EditorComponentTag::Animation) |
			EditorComponentTagBit(EditorComponentTag::Prefab)
		}
	}};

	int GetContextOrder(
		const EditorComponentDefinition& definition,
		EditorComponentContext context
	) {
		return context == EditorComponentContext::Scene
			? definition.sceneOrder
			: definition.prefabOrder;
	}

	std::vector<const EditorComponentDefinition*> BuildContextDefinitions(
		EditorComponentContext context
	) {
		std::vector<const EditorComponentDefinition*> result;
		for (const EditorComponentDefinition& definition : kDefinitions) {
			if (SupportsEditorComponentContext(definition, context)) {
				result.push_back(&definition);
			}
		}
		std::sort(
			result.begin(),
			result.end(),
			[context](
				const EditorComponentDefinition* left,
				const EditorComponentDefinition* right
			) {
				return GetContextOrder(*left, context) <
					GetContextOrder(*right, context);
			}
		);
		return result;
	}
}

const std::vector<const EditorComponentDefinition*>&
GetEditorComponentDefinitions(EditorComponentContext context) {
	static const std::vector<const EditorComponentDefinition*> sceneDefinitions =
		BuildContextDefinitions(EditorComponentContext::Scene);
	static const std::vector<const EditorComponentDefinition*> prefabDefinitions =
		BuildContextDefinitions(EditorComponentContext::Prefab);
	return context == EditorComponentContext::Scene
		? sceneDefinitions
		: prefabDefinitions;
}

const EditorComponentDefinition* FindEditorComponentDefinition(
	std::string_view type
) {
	const auto found = std::find_if(
		kDefinitions.begin(),
		kDefinitions.end(),
		[type](const EditorComponentDefinition& definition) {
			return type == definition.type;
		}
	);
	return found == kDefinitions.end() ? nullptr : &*found;
}

bool SupportsEditorComponentContext(
	const EditorComponentDefinition& definition,
	EditorComponentContext context
) {
	return (
		definition.contextMask & static_cast<uint8_t>(context)
	) != 0;
}

const char* GetEditorComponentDisplayName(
	const EditorComponentDefinition& definition,
	EditorLanguage language
) {
	return SelectEditorText(
		language,
		definition.japaneseName,
		definition.englishName
	);
}

const char* GetEditorComponentDescription(
	const EditorComponentDefinition& definition,
	EditorLanguage language
) {
	return SelectEditorText(
		language,
		definition.japaneseDescription,
		definition.englishDescription
	);
}

const char* GetEditorComponentCategoryDisplayName(
	EditorComponentCategory category,
	EditorLanguage language
) {
	switch (category) {
	case EditorComponentCategory::Rendering:
		return SelectEditorText(language, "描画", "Rendering");
	case EditorComponentCategory::World:
		return SelectEditorText(language, "環境", "World");
	case EditorComponentCategory::Camera:
		return SelectEditorText(language, "カメラ", "Camera");
	case EditorComponentCategory::Physics:
		return SelectEditorText(language, "物理・判定", "Physics");
	case EditorComponentCategory::Gameplay:
		return SelectEditorText(language, "ゲーム動作", "Gameplay");
	case EditorComponentCategory::Animation:
		return SelectEditorText(language, "Animation", "Animation");
	case EditorComponentCategory::EventAndFlow:
		return SelectEditorText(language, "イベント・遷移", "Events and Flow");
	}
	return "";
}

const std::vector<EditorComponentTag>& GetEditorComponentTags() {
	static const std::vector<EditorComponentTag> tags(
		kTags.begin(),
		kTags.end()
	);
	return tags;
}

bool HasEditorComponentTag(
	const EditorComponentDefinition& definition,
	EditorComponentTag tag
) {
	return (definition.tagMask & EditorComponentTagBit(tag)) != 0;
}

const char* GetEditorComponentTagId(EditorComponentTag tag) {
	switch (tag) {
	case EditorComponentTag::TwoD: return "2D";
	case EditorComponentTag::ThreeD: return "3D";
	case EditorComponentTag::UI: return "UI";
	case EditorComponentTag::Camera: return "Camera";
	case EditorComponentTag::Physics: return "Physics";
	case EditorComponentTag::Collision: return "Collision";
	case EditorComponentTag::Combat: return "Combat";
	case EditorComponentTag::Player: return "Player";
	case EditorComponentTag::Enemy: return "Enemy";
	case EditorComponentTag::Animation: return "Animation";
	case EditorComponentTag::Event: return "Event";
	case EditorComponentTag::Spawn: return "Spawn";
	case EditorComponentTag::Reference: return "Reference";
	case EditorComponentTag::PostEffect: return "PostEffect";
	case EditorComponentTag::Prefab: return "Prefab";
	}
	return "";
}

const char* GetEditorComponentTagDisplayName(
	EditorComponentTag tag,
	EditorLanguage language
) {
	switch (tag) {
	case EditorComponentTag::TwoD:
		return "2D";
	case EditorComponentTag::ThreeD:
		return "3D";
	case EditorComponentTag::UI:
		return "UI";
	case EditorComponentTag::Camera:
		return SelectEditorText(language, "カメラ", "Camera");
	case EditorComponentTag::Physics:
		return SelectEditorText(language, "物理", "Physics");
	case EditorComponentTag::Collision:
		return SelectEditorText(language, "判定", "Collision");
	case EditorComponentTag::Combat:
		return SelectEditorText(language, "戦闘", "Combat");
	case EditorComponentTag::Player:
		return SelectEditorText(language, "プレイヤー", "Player");
	case EditorComponentTag::Enemy:
		return SelectEditorText(language, "敵", "Enemy");
	case EditorComponentTag::Animation:
		return SelectEditorText(language, "Animation", "Animation");
	case EditorComponentTag::Event:
		return SelectEditorText(language, "イベント", "Event");
	case EditorComponentTag::Spawn:
		return SelectEditorText(language, "生成", "Spawn");
	case EditorComponentTag::Reference:
		return SelectEditorText(language, "参照", "Reference");
	case EditorComponentTag::PostEffect:
		return SelectEditorText(language, "PostEffect", "PostEffect");
	case EditorComponentTag::Prefab:
		return SelectEditorText(language, "Prefab", "Prefab");
	}
	return "";
}
