[![DebugBuild](https://github.com/Shinnosuke612/LE2C_CG3/actions/workflows/DebugBuild.yml/badge.svg)](https://github.com/Shinnosuke612/LE2C_CG3/actions/workflows/DebugBuild.yml)

# ゲームエディター 操作ガイド

自作C++／DirectX 12エンジンの、Scene・Prefab・Particleを扱うための基本操作をまとめたガイドです。

コンポーネント個別の機能説明はここでは扱いません。まずは「Entityを作る・探す・置く・Prefabとして再利用する」という流れを押さえ、必要になった機能をInspectorから追加して使ってください。

## 最初に覚える画面

| 画面 | 使う場面 |
| --- | --- |
| Scene View | Scene内の見た目を確認し、選択・移動・配置を行う。 |
| Hierarchy | Entityの親子関係、追加、検索、複製、削除を行う。 |
| Inspector | 選択中のEntityやProject内ファイルを編集する。 |
| Project | モデル、テクスチャ、Prefab、Particleなどのリソースを探して開く・配置する。 |
| Prefab Stage / Prefab Inspector | Prefabだけを独立して編集し、見た目や構造を確認する。 |
| Particle Effect Editor / Scene Particles | Particle Effect本体を作る・調整する、Scene内へ配置する。 |

画面配置が崩れた場合は、Settingsからレイアウトをリセットできます。日本語／英語の切替もSettingsにあります。

## 基本の流れ

1. Sceneを開くか、新規作成する。
2. Hierarchyで空のEntityを作る、またはProjectから素材・Prefabを配置する。
3. Entityを選択し、Inspectorで必要なコンポーネントを追加・調整する。
4. 同じ構成を使い回したい場合はPrefabとして保存し、以後はPrefabを配置する。
5. 変更後は上部の`Save Scene`でSceneを保存する。Prefab編集時はPrefab側のSaveを使う。

## Sceneを作る・切り替える

上部の`Scene`メニューからSceneを作成、複製、名前変更、削除できます。作成したSceneは`resources/scenes`以下へ保存され、一覧へ登録されます。

Sceneを切り替える前に未保存の変更がある場合は、保存するか、変更を破棄するかを選択します。迷ったら保存してから切り替えてください。

上部バーでは次の操作ができます。

| 操作 | 内容 |
| --- | --- |
| `Play` / `Stop` | 編集中のSceneを実行し、編集状態へ戻る。ショートカットは`F1`。 |
| `Pause` / `Resume` | 実行中のSceneを停止・再開する。ショートカットは`F2`。 |
| `Save Scene` | Sceneの変更を保存する。 |
| `Undo` / `Redo` | 編集操作を戻す・やり直す。 |

## Entityを追加・整理する

### 空のEntityを作る

Hierarchy上部の`+`を押すと空のEntityを作成できます。選択中のEntityを親にしたい場合は、対象を右クリックして`子Entityを作成`を選びます。

`+ Folder`はHierarchyを整理するためのフォルダーを作成します。ゲーム内で意味を持たせる必要がないグループ分けに使えます。

### 選択・名前変更・複製・削除

- Scene ViewまたはHierarchyでEntityを選択します。
- Hierarchy上で名前を再クリックすると名前を変更できます。`Enter`で確定、`Esc`で中止です。
- `Ctrl + D`で選択したEntityを複製できます。
- `Delete`で選択したEntityを削除できます。
- Hierarchy内のドラッグ操作で順序や親子関係を変更できます。
- 選択中のEntityへScene Viewを寄せたいときは、Hierarchyにフォーカスを置いて`F`を押します。

ロックされたEntityは、誤って移動・複製・削除しないよう編集操作が制限されます。

### Entityを検索する

Hierarchy上部の検索欄に名前の一部を入力すると絞り込めます。以下の条件検索も使えます。

| 入力例 | 絞り込み内容 |
| --- | --- |
| `enemy` | 名前に`enemy`を含むEntity |
| `team:Fish` | Team名に`Fish`を含むEntity |
| `type:Camera` | Cameraコンポーネントを持つEntity |
| `type:folder` | フォルダー |
| `is:inactive` | 無効化されているEntity |

複数の条件をスペースで並べると、すべてに一致するものだけを表示します。

## リソースをSceneへ置く

Projectウィンドウでファイルを選び、Scene ViewまたはHierarchyへドラッグ＆ドロップします。

| リソース | ドロップ先と結果 |
| --- | --- |
| モデル | Scene ViewまたはHierarchyへドロップすると、新しいEntityとして配置される。 |
| テクスチャ | Scene Viewへドロップすると、Sprite用のEntityとして配置される。 |
| Prefab | Scene Viewへドロップすると、Linked Instanceとして配置される。Hierarchy上のEntityへドロップすると、そのEntityの子として配置される。 |

置いた後の位置・回転・大きさは、Entityを選択してScene ViewのGizmoまたはInspectorから変更します。個別の設定項目が必要になった時だけ、Inspectorで対象コンポーネントを追加してください。

## コンポーネントを追加・探す

Entityを選択してInspectorの`Add Component`を開きます。追加画面では、名前検索、カテゴリ、タグ、Favorites onlyで候補を絞り込めます。

- よく使うコンポーネントは星ボタンでお気に入りにすると、次回から探しやすくなります。
- 複数追加したい場合は候補をTrayに入れてから、まとめて確定できます。
- 候補カードをTrayへドラッグ＆ドロップすることもできます。
- 追加後の各項目は、必要になった時にInspectorで確認してください。本ガイドでは個々のコンポーネント仕様は省略します。

## Prefabを作る・編集する

### Prefabにする場面

敵、背景物、武器など、同じ構成を複数のSceneで使いたい時はPrefabにします。Prefabを直接直せば、同じPrefabを使う場所へ変更を反映できます。一方で、Sceneだけで異なる見た目や配置にしたい場合は、Instance側の変更を残せます。

### 開く・探す

- Project内のPrefabを選び、Inspectorの`Open Prefab Editor`で開きます。
- 上部メニューの`Prefab > Quick Open...`、または`Ctrl + Shift + P`でPrefabを名前検索して開けます。
- Quick OpenではRecent、Favorites、Prefabs Onlyを利用できます。

Prefab EditorではPrefab専用のHierarchy、Inspector、Scene表示を使います。ここでの編集履歴と未保存状態は、通常のSceneとは別に管理されます。

### 配置する

ProjectからPrefabをScene Viewへドラッグ＆ドロップするとLinked Instanceとして配置されます。HierarchyのEntityへドロップすれば、子Entityとして配置できます。

### Instanceだけを変える

Scene上のPrefab Instanceを選択すると、InspectorでLinked AssetとOverrideを確認できます。

| 操作 | 使う場面 |
| --- | --- |
| `Open Prefab` | 元のPrefabを開いて編集する。 |
| `Apply` | Instance側で変えた項目を元Prefabへ反映する。必要な項目だけ反映したい時に使う。 |
| `Revert` | Instance側の変更を捨て、元Prefabの状態へ戻す。 |
| `Apply Instance To Prefab` | Instance全体の変更を元Prefabへ反映する。影響範囲を確認してから使う。 |
| `Revert Instance` | Instance全体を元Prefabの状態へ戻す。 |
| `Unpack` | Prefabとのリンクを外し、通常のEntityとして個別編集する。 |

元Prefabを開いたままの時は、競合を避けるためInstanceの反映操作が制限されます。Prefabを保存または閉じてから操作してください。

## Particle Effectを作る・編集する

Particleは、まずEffectファイルを作り、そのEffectをScene用の配置Assetへ登録して使います。Effectそのものの設定と、Scene内での位置・個数は分けて管理します。

### Effectファイルを作る

1. `Particle Effect Editor`を開きます。
2. `FilePath`に保存先を入力します。例: `resources/particles/fire_burst.json`。
3. `Name`と`Texture`を設定します。Textureは`Resource Texture`の一覧から選ぶこともできます。
4. `Simulation`を選び、Emitterなど必要な項目を調整します。
5. `Apply`でプレビューへ反映し、見た目を確認します。
6. 問題なければ`Save`でEffectファイルとして保存します。

既存のEffectは`Existing Effect`から選ぶか、ProjectでParticle JSONを選択してInspectorの`Load into Particle Editor`を押すと編集できます。保存前に元へ戻したい時は`Load`でファイルの内容を読み直します。

GPU Particleを使う場合は、`GPU Runtime`の`Apply GPU Preview`でプレビューへ反映できます。`Emit Once`は一度だけ発生させて確認したい時、`Reset GPU Buffer`は表示をリセットしたい時に使います。GPUでは未対応の設定がある場合、Editor内に注意が表示されるため、必ずプレビューを確認してください。

## ParticleをSceneへ配置・変更する

`Scene Particles`ウィンドウで、Effectを再利用できる配置Assetとして登録し、各SceneへInstanceを置きます。`Particle Effect Editor`でEffectの見た目を作り、`Scene Particles`で出現位置とSceneごとの差を決める、という使い分けです。

### 配置Assetを作る

1. `Scene Particles`の`Assets`タブを開きます。
2. `New Asset`へ名前を入力し、`Create Asset`を押します。
3. 作成した`Placement Asset`を選択します。
4. `Add Placement`を押します。
5. Placementの`Effect`で作成済みEffectを選びます。
6. `Translate`、`SpawnSize`、`Count`、`Frequency`、`Emitter Active`を調整します。
7. `Save Layout`で配置情報を保存します。

`Reload Emitter from Effect`を押すと、EffectファイルのEmitter設定をPlacementへ読み直せます。Effectの基準値を使い直したい時に便利です。

### Sceneへ置く

1. `Scene Particles`の`Scenes`タブを開き、対象Sceneを選択します。
2. `Asset to Add`から配置Assetを選びます。
3. `Add Asset Instance`を押します。
4. Instanceの`Offset`で、そのScene内だけの配置位置を調整します。
5. `Emit Now`でその場で発生させ、位置と見た目を確認します。
6. `Save Layout`で保存します。

同じ配置Assetを複数のSceneに置けます。Asset側を変更すると、そのAssetを使う配置に反映されます。特定のSceneだけ位置をずらしたい場合は、AssetではなくScene Instanceの`Offset`を変更してください。

## 保存と安全な使い方

- Sceneを編集したら`Save Scene`、Particle配置を編集したら`Save Layout`、Prefabを編集したらPrefab側のSaveを使います。
- 上部に`Unsaved`、Scene Particlesに`Unsaved layout changes`が表示されている間は、別Sceneへ移動・終了する前に保存します。
- まずは既存のPrefabやParticleを複製・配置して変更し、動きが分かってから新規作成すると安全です。
- 画面上で意味が分かる項目は、このREADMEでは省略しています。目的の機能が必要になった時に、検索・追加・プレビューを使って試してください。

## Project ManagerのSolutionと配置

ゲームのSourceとAssetは`project/`に置き、IDE用の`.sln`・`.vcxproj`・`.filters`・`.props`は新しい生成方式ではGame rootの`intermediate/project-files/`へ生成します。Build出力の`generated/outputs/`・`generated/obj/`とは別です。

既存Projectの配置は自動で変えません。`Open Solution`は`game.project.json`に指定された正式Solutionを開きます。`Generate Preview`はPC localの確認用Solutionを作り、現在の生成入力に一致するものだけ`Open Preview Solution`で開けます。不一致の案内が出たら再生成してください。古いPreviewは自動削除されません。

旧配置を変更するには、対応SourceでのBuildとEditorのAsset表示を確認し、生成したPreviewを確認してから`Migrate Project Files...`を使います。移行先が正式配置になった後は`Regenerate Verified Preview...`で同じ配置のIDEファイルを更新します。手編集された生成物・移行先の衝突・未完了journalがある場合は移行を止めます。復旧要求が出たらProject Managerの対象operationの状態を確認してください。

IDE再生成はゲームSourceの更新や、Engine／Game rootの物理的な分離を行いません。`.vs`・`.user`など個人IDE設定は生成物の所有集合へ含めません。