// 役割: ゲームを初期化せず、共通生成サービスの判定/更新をCLIから呼び出す。
#include "../../project/engine/project/ProjectSolutionGenerationService.h"
#include "../../project/engine/base/EditorLocalization.h"
#include "../../project/engine/utility/StringUtility.h"
#include "../../project/externals/nlohmann/json.hpp"

#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
	using json = nlohmann::json;

	EditorLanguage ReadLanguage(const std::filesystem::path& descriptor) {
		// Editorが読む同じsource rootの設定と同じ値変換を使う。CLI専用言語設定は作らない。
		try {
			std::ifstream input(descriptor.parent_path() / L"project/editor_settings.json", std::ios::binary);
			if (input) {
				const auto settings = json::parse(input);
				if (settings.contains("language") && settings.at("language").is_string())
					return ParseEditorLanguage(settings.at("language").get<std::string>());
			}
		} catch (const std::exception&) { }
		return EditorLanguage::Japanese;
	}

	const char* StateName(ProjectAutoGenerationState state) {
		switch (state) {
		case ProjectAutoGenerationState::Latest: return "Latest";
		case ProjectAutoGenerationState::UpdateRequired: return "UpdateRequired";
		case ProjectAutoGenerationState::Updated: return "Updated";
		case ProjectAutoGenerationState::NeedsAttention: return "NeedsAttention";
		default: return "Failed";
		}
	}

	struct Explanation { const char* code; const char* message; const char* remedy; };
	Explanation Explain(const ProjectAutoGenerationResult& result, EditorLanguage language) {
		auto text = [language](const char* ja, const char* en) { return SelectEditorText(language, ja, en); };
		if (result.state == ProjectAutoGenerationState::Latest) return { "None", text("生成設定は最新です。再生成は行いませんでした。", "Generated projects are current. No regeneration was performed."), "" };
		if (result.state == ProjectAutoGenerationState::Updated) return { "None", text("必要なプロジェクト再生成が完了しました。", "The required project regeneration completed."), "" };
		switch (result.issue) {
		case ProjectAutoGenerationIssue::InputsChanged: return { "InputsChanged", text("ファイル構成または生成設定の変更があります。", "Source membership or generation settings changed."), text("Ensure操作で反映できます。", "Use Ensure to apply the update.") };
		case ProjectAutoGenerationIssue::NotRegistered: return { "NotRegistered", text("対象プロジェクトが登録されていません。", "This project is not registered."), text("Project Managerでgame.project.jsonを登録してください。", "Register game.project.json in Project Manager.") };
		case ProjectAutoGenerationIssue::UnsupportedProject: return { "UnsupportedProject", text("この配置は自動再生成の対象外です。", "This project layout is not supported by automatic regeneration."), text("SnapshotとProject Files V2への明示的な設定・移行を確認してください。", "Review the explicit Snapshot / Project Files V2 setup or migration.") };
		case ProjectAutoGenerationIssue::MissingManifest: return { "MissingManifest", text("生成物の管理情報がありません。", "The generation manifest is missing or unsafe."), text("Project Managerで生成物の採用・復旧を確認してください。", "Review adoption or recovery in Project Manager.") };
		case ProjectAutoGenerationIssue::ModifiedArtifact: return { "ModifiedArtifact", text("生成済みファイルに変更または欠落があるため、上書きしませんでした。", "Generated files were modified or removed; they were not overwritten."), text("対象の差分を確認し、必要な変更を退避して既存の復元手順を行ってください。", "Review the affected files, preserve needed edits and use the existing recovery procedure.") };
		case ProjectAutoGenerationIssue::PendingRecovery: return { "PendingRecovery", text("未完了の生成・作成処理があります。", "A generation or creation operation is pending."), text("Project Managerで未完了操作を確認・復旧してから再操作してください。", "Resolve the pending operation in Project Manager, then retry.") };
		case ProjectAutoGenerationIssue::InputChangedDuringGeneration: return { "InputChangedDuringGeneration", text("処理中にファイル構成が変更されました。", "Project inputs changed during generation."), text("ファイル操作が終わってから再操作してください。", "Retry after file changes finish.") };
		case ProjectAutoGenerationIssue::Busy: return { "Busy", text("別の生成操作または管理されたビルドが実行中です。", "Another generation operation or managed build is active."), text("その操作が完了してから再操作してください。", "Retry after that operation completes.") };
		case ProjectAutoGenerationIssue::RegistryConflict: return { "RegistryConflict", text("同じ登録情報が別の操作で変更されました。", "The same registry entry changed in another operation."), text("Project Managerを更新して現在の内容を確認してください。", "Refresh Project Manager and review the current values.") };
		case ProjectAutoGenerationIssue::AccessFailure: return { "AccessFailure", text("プロジェクトまたは登録情報へ安全にアクセスできません。", "Project or registry access could not be secured."), text("パス・アクセス権・登録ファイルの内容を確認してください。", "Check paths, access permissions and registry contents.") };
		case ProjectAutoGenerationIssue::StagingFailed: return { "StagingFailed", text("一時生成物の作成・検証に失敗しました。", "Artifact staging or verification failed."), text("詳細のパスと書込可否を確認してください。完成した生成物には切り替えていません。", "Check the diagnostic paths and write access. The canonical generation was not switched.") };
		case ProjectAutoGenerationIssue::CommitFailed: return { "CommitFailed", text("生成物の反映を完了できませんでした。", "The generation transaction did not complete."), text("ビルドせず、Project Managerで未完了操作と復旧状態を確認してください。", "Do not build; inspect the pending operation and recovery state in Project Manager.") };
		default: return { "InvalidInput", text("生成に必要な設定または参照先を確認できません。", "Required generation settings or references could not be validated."), text("詳細の設定ファイル・ソース・外部参照を確認してください。", "Review the settings, source files and external references shown in the diagnostics.") };
		}
	}
}

int wmain(int argc, wchar_t** argv) {
	std::filesystem::path descriptor, registryPath;
	bool ensure = false, structured = false, valid = true, operationSeen = false;
	for (int index = 1; index < argc; ++index) {
		const std::wstring argument = argv[index];
		if ((argument == L"--inspect" || argument == L"--ensure") && !operationSeen) { ensure = argument == L"--ensure"; operationSeen = true; }
		else if (argument == L"--json") structured = true;
		else if (argument == L"--descriptor" && descriptor.empty() && index + 1 < argc) descriptor = argv[++index];
		else if (argument == L"--registry" && registryPath.empty() && index + 1 < argc) registryPath = argv[++index];
		else valid = false;
	}
	SetConsoleOutputCP(CP_UTF8);
	const EditorLanguage language = ReadLanguage(descriptor);
	ProjectAutoGenerationResult result;
	try {
		if (!valid || !operationSeen || !descriptor.is_absolute() || (!registryPath.empty() && !registryPath.is_absolute())) {
			result.detail = "Use --inspect or --ensure with --descriptor <absolute game.project.json> [--registry <absolute path>] [--json].";
		} else {
			ProjectRegistry registry;
			std::string error;
			if (!(registryPath.empty() ? registry.Load(error) : registry.Load(registryPath, error))) {
				result.issue = ProjectAutoGenerationIssue::AccessFailure; result.detail = error;
			} else {
				ProjectSolutionGenerationService service(registry);
				result = ensure ? service.EnsureCurrent(descriptor.lexically_normal()) : service.InspectFreshness(descriptor.lexically_normal());
			}
		}
	} catch (const std::exception& error) { result.state = ProjectAutoGenerationState::Failed; result.detail = error.what(); }
	const Explanation explanation = Explain(result, language);
	if (structured) {
		json paths = json::array();
		for (const auto& path : result.affectedPaths) paths.push_back(StringUtility::ToUtf8(path));
		std::cout << json({ { "schemaVersion", 1 }, { "state", StateName(result.state) }, { "code", explanation.code },
			{ "message", explanation.message }, { "remedy", explanation.remedy }, { "detail", result.detail },
			{ "inputIdentity", result.inputIdentity }, { "operationId", result.operationId }, { "affectedPaths", paths } }).dump() << '\n';
	} else {
		std::cout << explanation.message << '\n';
		if (explanation.remedy[0]) std::cout << explanation.remedy << '\n';
		for (const auto& path : result.affectedPaths) std::cout << "  " << StringUtility::ToUtf8(path) << '\n';
		if (!result.detail.empty()) std::cout << SelectEditorText(language, "診断原文: ", "Diagnostic details: ") << result.detail << '\n';
	}
	if (result.state == ProjectAutoGenerationState::Latest || result.state == ProjectAutoGenerationState::Updated) return 0;
	return result.state == ProjectAutoGenerationState::UpdateRequired ? 10 : result.state == ProjectAutoGenerationState::NeedsAttention ? 2 : 3;
}
