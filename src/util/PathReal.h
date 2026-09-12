// P0-1 真实路径解析与工作区边界判定（纯逻辑，可脱离 GUI 单测；对抗用例真实创建 junction/symlink）。
// 原则：
//  1) 逐组件解析：已存在的组件经 GetFinalPathNameByHandleW 展开 junction/symlink/挂载点；
//  2) 首个不存在的组件之后只做词法拼接（尚不落盘的组件不可能藏 reparse point），其中
//     "." 跳过、".." 对已解析真实路径做父级回退（真实父=词法父；到根后按 Windows 语义钳制）；
//  3) 解析前置净化：不做 cleanPath 之类的词法折叠——那会把 ws/junction/../x 折成区内的 ws/x；
//  4) 一切解析不了（根不可达/拒绝访问/悬空 reparse point/超长）→ 空串，调用方按越界处理（fail-closed）。
#pragma once
#include <QString>

namespace miderforge::pathreal {

// 解析 absPath 的真实路径；失败返回空串（调用方必须按越界处理）
QString resolveReal(const QString& absPath);

// 真实路径包含判定：realPath == realRoot 或位于 realRoot 之下。
// 大小写不敏感（Windows 语义）、组件边界严格（/ws 不匹配 /ws2）；任一侧为空 → false
bool isInsideOrEqual(const QString& realPath, const QString& realRoot);

} // namespace miderforge::pathreal
