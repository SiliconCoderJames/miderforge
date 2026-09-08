// 记忆矛盾裁决服务（矛盾扫描 v2）：独立的 LLM 客户端对候选对做语义三态判定，
// 不与 AgentLoop 的主 ChatClient 抢占（设计见 docs/MEMORY-DESIGN.md 第六节）
#pragma once
#include "llm/ChatClient.h"
#include "llm/ProviderManager.h"
#include "memory/MemoryManager.h"
#include <QObject>
#include <QVector>

namespace miderforge {

class AdjudicationService : public QObject {
    Q_OBJECT
public:
    struct Verdict {
        qint64 idA = 0;
        qint64 idB = 0;
        QString label;  // contradiction | duplicate | complement（其余归 unknown）
        QString reason;
    };

    explicit AdjudicationService(ProviderManager* providers, QObject* parent = nullptr);
    ~AdjudicationService() override;

    // 是否具备裁决条件（有已配置的供应商）
    bool available() const;

    // 异步批量裁决：完成后发 finished（只含能对上候选对的判定），失败发 failed
    void judge(const QVector<MemoryManager::ContradictionPair>& pairs);
    bool busy() const { return m_busy; }

    // 纯逻辑、可单测：从自由文本解析判定列表（jsonextract + 标签白名单 + id 校验对齐候选对）
    static QVector<Verdict> parseVerdicts(const QString& llmText,
                                          const QVector<MemoryManager::ContradictionPair>& pairs);
    // 裁决提示词组装（暴露给单测）
    static QString buildPrompt(const QVector<MemoryManager::ContradictionPair>& pairs);

signals:
    void finished(const QVector<AdjudicationService::Verdict>& verdicts);
    void failed(const QString& error);

private:
    ProviderManager* m_providers = nullptr;
    ChatClient* m_chat = nullptr;
    QVector<MemoryManager::ContradictionPair> m_pairs;
    bool m_busy = false;
};

} // namespace miderforge
