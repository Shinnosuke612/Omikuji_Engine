# Project generation tool

必要時だけプロジェクトを再生成する共通基盤の入口です。ゲーム本体のBuild・起動、VS/F5への自動接続は行いません。既存の登録済みSnapshot / Project Files V2だけを対象にします。

`Invoke-ProjectGeneration.ps1`を入口にしてください。固定ツールの入力とbinaryのhashを確認し、欠落・旧版ならツールだけをBuildします。生成規則はEngineと共用するC++実装です。ソースの本文を編集しただけの場合、ゲーム用の生成ファイルは更新しません。

```powershell
# 作業ルートから。初回・ツール変更時はツール自身のBuildを伴います。
& .\tools\project-generation\Invoke-ProjectGeneration.ps1 -Descriptor "$PWD\game.project.json" -Operation Inspect

# 必要な場合だけ8生成ファイルとmanifestを更新します。ゲームのBuildは行いません。
& .\tools\project-generation\Invoke-ProjectGeneration.ps1 -Descriptor "$PWD\game.project.json" -Operation Ensure
```

事前に既存のC++ toolchain v143とWindows SDK 10.0.26100.0が必要です。依存のインストール・Restoreは自動で行いません。MSBuildを検出できない場合は`-MSBuildPath`で既存の実行ファイルを指定できます。

- `-Json`は結果をJSON一行で返します。bootstrap進捗/失敗は標準エラーへ出します。
- `-Registry`は検証等で明示した別Registryを使用します。省略時はEditorと同じPC-local Registryです。未登録Projectを勝手に登録しません。
- 表示言語は対象Projectの既存`project/editor_settings.json`に従います。言語専用の追加設定はありません。
- 終了code: `0`＝Latest/Updated、`10`＝InspectでUpdateRequired、`2`＝確認・復旧が必要、`3`＝失敗。`10`はInspectで生成を書き込まなかったことを示します。
- 完全Previewのソースコピーは作りません。自動経路のstagingはRegistry親の`solution-generation/automatic/<project-id>/<operation-id>`、復旧情報は既存のjournalとrollback領域を使います。途中失敗を隠す自動削除はしません。
- `ProjectAccessLock`を含むEditor/ツール同士で排他します。初回導入時は旧Editorを閉じてください。旧binaryやこの入口を通さない外部Buildには排他が適用されません。
- 生のexeを古いまま自動処理に組み込まず、bootstrap入口を使ってください。VS連携は後続実装です。現時点で通常Build/F5が自動再生成されるわけではありません。

共通APIの`InspectFreshness`は読取専用、`EnsureCurrent`は必要時だけ反映します。将来のBuild adapterは同一threadで外側の`ProjectAccessLock`を取得し、子Buildが終了するまで保持します。別プロセスへ生成を委譲するadapterでは、同じleaseを誰がBuild終了まで保持するかを接続工程で確定させてください。

手編集された生成物、manifest欠落、旧配置、未完了journalは自動上書き・採用・移行・復旧をしません。診断の原因と対処を確認してください。実際に再生成した回は、従来の8file置換とobject名の規則により再コンパイルが増える場合があります。PCH・並列上限2・既存の除外/コピー設定は生成元で維持しています。

ツールの共有実装へ直接includeを追加する場合は、固定vcxprojのClCompile/ClIncludeにも入力を登録してください。bootstrapはこの入力集合の内容hashを使い、生成したInputFingerprint.hを全翻訳単位の依存へ渡します。ソースの時刻だけで旧binaryを最新と判断せず、ゲーム用のobjを削除するCleanも追加しません。
