// 供应商视图实现
#include "app/ProviderPanel.h"
#include "app/Theme.h"
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace miderforge {

ProviderPanel::ProviderPanel(ProviderManager* pm, QWidget* parent) : QWidget(parent), m_pm(pm) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 10, 12, 10);

    // 顶部：故障转移链展示
    QString chain;
    const ProviderConfig* active = m_pm->activeProvider();
    if (active)
        chain += active->name;
    if (const ProviderConfig* backup = m_pm->failoverProvider())
        chain += QStringLiteral("  →  %1").arg(backup->name);
    auto* chainLabel = new QLabel(QStringLiteral("当前故障转移链：%1").arg(chain), this);
    chainLabel->setStyleSheet(QStringLiteral(
        "QLabel{background-color:%1;color:%2;border:1px solid #35383d;border-radius:6px;padding:8px;}")
                                  .arg(theme::colors::panel.name(), theme::colors::textDim.name()));
    outer->addWidget(chainLabel);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* host = new QWidget(scroll);
    m_grid = new QGridLayout(host);
    m_grid->setContentsMargins(0, 8, 0, 8);
    m_grid->setSpacing(10);
    scroll->setWidget(host);
    outer->addWidget(scroll, 1);

    connect(m_pm, &ProviderManager::providerChanged, this, &ProviderPanel::rebuild);
    rebuild();
}

void ProviderPanel::rebuild() {
    // 清空旧卡片
    while (m_grid->count() > 0) {
        QLayoutItem* it = m_grid->takeAt(0);
        if (it->widget())
            it->widget()->deleteLater();
        delete it;
    }
    m_testLabels.clear();
    int row = 0, col = 0;
    for (const auto& cfg : m_pm->all()) {
        m_grid->addWidget(makeCard(cfg), row, col);
        if (++col >= 2) {
            col = 0;
            ++row;
        }
    }
}

QWidget* ProviderPanel::makeCard(const ProviderConfig& cfg) {
    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("providerCard_%1").arg(cfg.name));
    card->setMinimumSize(320, 150);
    card->setMaximumWidth(420);

    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(12, 10, 12, 10);

    // 名称 + 状态灯（绿=健康/红=故障/灰=未配置）
    const ProviderManager::Health h = m_pm->health(cfg.name);
    QColor dotColor = theme::colors::textDim; // 灰=未配置
    if (!cfg.configured)
        dotColor = theme::colors::textDim;
    else if (h == ProviderManager::Health::Ok)
        dotColor = theme::colors::success;
    else if (h == ProviderManager::Health::Fail)
        dotColor = theme::colors::error;
    else if (cfg.name == m_pm->activeProvider()->name)
        dotColor = theme::colors::success;

    auto* header = new QLabel(
        QStringLiteral("%1 <b>%2</b> %3")
            .arg(theme::coloredDot(dotColor), cfg.name,
                 cfg.name == m_pm->activeProvider()->name ? QStringLiteral("(当前)") : QString()),
        card);
    header->setTextFormat(Qt::RichText);
    lay->addWidget(header);

    // 三档映射展示
    lay->addWidget(new QLabel(
        QStringLiteral("快速：%1 | 主力：%2 | 旗舰：%3")
            .arg(m_pm->modelForTier(cfg, QStringLiteral("fast")),
                 m_pm->modelForTier(cfg, QStringLiteral("main")),
                 m_pm->modelForTier(cfg, QStringLiteral("flagship"))), card));
    lay->addWidget(new QLabel(cfg.role.isEmpty()
                                  ? QStringLiteral("角色：主供应商")
                                  : QStringLiteral("角色：%1").arg(cfg.role), card));

    // 实时延迟/费用：v1 由测试连接与使用事件回填（decision: 显示占位“—”不建假数据）
    lay->addWidget(new QLabel(QStringLiteral("延迟：— · 今日 tokens：— · 估算费用：—"), card));

    // 测试连接结果回显行（默认隐藏，点击后显示，避免静态冗余）
    auto* testResult = new QLabel(card);
    testResult->setWordWrap(true);
    testResult->hide();
    m_testLabels.insert(cfg.name, testResult);
    lay->addWidget(testResult);

    auto* btnRow = new QHBoxLayout();
    auto* testBtn = new QPushButton(QStringLiteral("测试连接"), card);
    auto* useBtn = new QPushButton(QStringLiteral("设为当前"), card);
    useBtn->setEnabled(cfg.name != m_pm->activeProvider()->name);
    testBtn->setEnabled(cfg.configured); // 未配置 Key 时探测必然失败，禁用并提示原因
    connect(testBtn, &QPushButton::clicked, this, [this, name = cfg.name] {
        QString err;
        int latency = 0;
        const bool ok = m_pm->testConnection(name, &err, &latency);
        if (auto* lb = m_testLabels.value(name)) {
            if (ok)
                lb->setText(QStringLiteral("✔ 连接正常（%1 ms）").arg(latency));
            else
                lb->setText(QStringLiteral("✖ 连接失败：%1").arg(err));
            lb->setStyleSheet(QStringLiteral("color:%1;")
                                  .arg(ok ? theme::colors::success.name() : theme::colors::error.name()));
            lb->show();
        }
    });
    connect(useBtn, &QPushButton::clicked, this, [this, name = cfg.name] {
        m_pm->setActive(name);
        m_pm->setHealth(name, ProviderManager::Health::Ok);
    });
    btnRow->addWidget(testBtn);
    btnRow->addWidget(useBtn);
    if (!cfg.configured) {
        auto* hint = new QLabel(QStringLiteral("（未配置 Key）"), card);
        hint->setStyleSheet(QStringLiteral("color:%1;font-size:9pt;").arg(theme::colors::warn.name()));
        btnRow->addWidget(hint);
    }
    btnRow->addStretch(1);
    lay->addLayout(btnRow);

    card->setStyleSheet(QStringLiteral(
        "QFrame#providerCard_%1{background-color:%2;border:1px solid #35383d;border-radius:8px;}")
                            .arg(cfg.name, theme::colors::panel.name()));
    return card;
}

} // namespace miderforge
