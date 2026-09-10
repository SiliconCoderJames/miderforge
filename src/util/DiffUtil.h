// 行级 diff（Codex 式线程内变更审查用）：变更统计 + 紧凑 unified 文本。
// 采用「公共前缀/后缀修剪 + 中段替换」模型：计数精确、O(n) 确定性；多 hunk 场景
// 输出为带 @@ 头的单一区段（展示用足够，非严格 git unified 语义）
#pragma once
#include <QString>

namespace miderforge::DiffUtil {

struct Diff {
    int added = 0;       // 新增行数
    int removed = 0;     // 删除行数
    QString unified;     // 紧凑 unified 文本（@@ 头 + 上下文 + −/＋ 行）；文本相同则为空
    bool truncated = false; // diff 正文超出行数上限被截断
};

// oldText/newText 均按 '\n' 分行（行尾 \r 归一）；maxDiffLines 限制 unified 正文行数
Diff unified(const QString& oldText, const QString& newText,
             int contextLines = 2, int maxDiffLines = 60);

} // namespace miderforge::DiffUtil
