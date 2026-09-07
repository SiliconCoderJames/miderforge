// 供应商视图（规格 4.7）：卡片流——状态灯/三档映射/测试连接/设为当前；顶部故障转移链
#pragma once
#include "llm/ProviderManager.h"
#include <QGridLayout>
#include <QScrollArea>
#include <QWidget>

namespace miderforge {

class ProviderPanel : public QWidget {
    Q_OBJECT
public:
    ProviderPanel(ProviderManager* pm, QWidget* parent = nullptr);

public slots:
    void rebuild(); // 重建卡片（供应商切换/健康变化）

private:
    QWidget* makeCard(const ProviderConfig& cfg);

    ProviderManager* m_pm = nullptr;
    QGridLayout* m_grid = nullptr;
};

} // namespace miderforge
