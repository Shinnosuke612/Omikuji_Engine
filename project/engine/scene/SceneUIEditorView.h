// 役割: UI編集Scene Viewだけに適用するUI編集表示の値と、非破壊の表示条件を共有する。
#pragma once
#include <cstdint>
#include <unordered_set>
#include "SceneDocument.h"
#include "SceneEntityQuery.h"
#include "SceneUILayout.h"

struct SceneUIEditorView {
	enum class Scope { All, Panel, Text };
	uint64_t generation = 0, revision = 0, panelId = 0, textId = 0;
	Scope scope = Scope::Panel;
	bool showBounds = true;

	bool Matches(const SceneDocument& document) const {
		return generation != 0 && generation == document.GetContentGeneration() &&
			revision == document.GetRevision();
	}
	bool Isolated() const { return generation != 0 && scope != Scope::All; }
	bool InPanel(const SceneDocument& document, uint64_t id) const {
		return panelId && (id == panelId || document.IsDescendantOf(id, panelId));
	}
	bool Allows(const SceneDocument& document, uint64_t id) const {
		if (scope == Scope::Text) return id == textId;
		if (scope == Scope::Panel) return InPanel(document, id);
		// 全体表示でも、明示したPanel以外のMenu枝は開かない。
		std::unordered_set<uint64_t> visited;
		for (const auto* node = document.FindEntity(id); node && visited.insert(node->id).second;
			node = document.FindEntity(node->parentId)) {
			if (SceneEntityQuery::FindComponent(*node, "MenuPanel")) return node->id == panelId;
		}
		return true;
	}

	// 複製の表示経路だけを補う。値の妥当性や親子構造はResolverに委ねる。
	// Matchesは複製前のDocumentに対して呼ぶ（Document copyで内容世代が変わる）。
	void PrepareDisplay(SceneDocument& copy) const {
		std::unordered_set<uint64_t> visited;
		for (auto* node = copy.FindEntity(textId ? textId : panelId);
			node && visited.insert(node->id).second; node = copy.FindEntity(node->parentId)) {
			node->active = true;
			for (auto& component : node->components) {
				if (component.type == "UITransform" || (node->id == textId && component.type == "TextRenderer"))
					component.enabled = true;
			}
		}
	}
};
