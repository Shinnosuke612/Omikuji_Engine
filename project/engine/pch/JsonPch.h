// 役割: JSONを使うEngine CPPだけで共有する、安定したPCHの境界を定義する。
#pragma once

// 強制includeで最初に読み込み、個別CPPのヘッダーをPCHの境界へ取り込まない。
#include "../../externals/nlohmann/json.hpp"
