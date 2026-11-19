// 役割: 準備済み資源を共有キャッシュへ公開した結果を定義する。
#pragma once

// Publishedだけが呼出元の資源所有を消費する。
enum class PreparedPublishStatus { Published, ReusedExisting, Failed };
