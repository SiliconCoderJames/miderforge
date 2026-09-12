// 设置对话框实现
#include "app/SettingsDialog.h"
#include "app/ChatWidgets.h"
#include "app/Theme.h"
#include "core/AppContext.h"
#include "util/AppDirs.h"
#include "util/Dpapi.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QFrame>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QTcpSocket>
#include <QTimer>
#include <QVBoxLayout>

namespace miderforge {

namespace {
constexpr const char* kHiveRel = "config/hive.json";

// 蜂巢是本地回环专属接口（设计约束），只接受环回地址；不经过 NetGuard 公网校验
bool isLoopbackHost(const QString& host) {
    return host == QStringLiteral("127.0.0.1") || host == QStringLiteral("localhost")
           || host == QStringLiteral("::1");
}

// /api/health 响应 → 友好文案（AgentHive 返回 {"code":0,"message":"ok","data":{service,version,...}}）
QString friendlyHealth(const QByteArray& resp) {
    const int sep = resp.indexOf("\r\n\r\n");
    const QByteArray body = sep >= 0 ? resp.mid(sep + 4) : resp;
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
    if (pe.error == QJsonParseError::NoError && doc.isObject()) {
        const QJsonObject obj = doc.object();
        if (obj.value(QStringLiteral("code")).toInt(-1) == 0) {
            const QJsonObject data = obj.value(QStringLiteral("data")).toObject();
            const QString svc = data.value(QStringLiteral("service")).toString();
            const QString ver = data.value(QStringLiteral("version")).toString();
            return QStringLiteral("✅ 已连接：%1%2 · 服务正常")
                .arg(svc.isEmpty() ? QStringLiteral("AgentHive") : svc,
                     ver.isEmpty() ? QString() : QStringLiteral(" v%1").arg(ver));
        }
        if (obj.contains(QStringLiteral("message")))
            return QStringLiteral("⚠️ 服务返回异常：%1").arg(obj.value(QStringLiteral("message")).toString());
    }
    return QStringLiteral("已收到响应：%1").arg(QString::fromUtf8(body).trimmed().left(120));
}
} // namespace

SettingsDialog::SettingsDialog(ProviderManager* pm, EmailNotifier* mail, const Panels& panels,
                               const std::function<void(int)>& applyPermissionMode,
                               QWidget* parent)
    : QWidget(parent), m_pm(pm), m_mail(mail), m_applyPermissionMode(applyPermissionMode) {
    // 嵌入主窗口内容区（非独立窗口）：无边框、不要自己的标题栏
    setAttribute(Qt::WA_StyledBackground, true);

    // ---- 左导航：返回 / 搜索 / 分组条目（对齐参考图信息架构） ----
    m_navHost = new QWidget(this);
    m_navHost->setFixedWidth(228);
    m_navHost->setStyleSheet(QStringLiteral("background-color:%1;border-right:1px solid %2;")
                                 .arg(theme::colors::window().name(), theme::colors::panel().name()));
    m_navLay = new QVBoxLayout(m_navHost);
    m_navLay->setContentsMargins(10, 10, 10, 10);
    m_navLay->setSpacing(2);

    // 返回：设置是"从工作区进来的"，给出明确退路（Esc 也能关，但按钮更直观）
    auto* backBtn = new QPushButton(QStringLiteral("←  返回工作区"), m_navHost);
    backBtn->setFixedHeight(32);
    backBtn->setCursor(Qt::PointingHandCursor);
    backBtn->setStyleSheet(QStringLiteral(
        "QPushButton{background:transparent;border:none;text-align:left;padding-left:6px;"
        "color:%1;font-size:10.5pt;}"
        "QPushButton:hover{background-color:%2;border-radius:6px;}")
                               .arg(theme::colors::text().name(), theme::colors::panel().name()));
    connect(backBtn, &QPushButton::clicked, this, &SettingsDialog::closeRequested);
    m_navLay->addWidget(backBtn);
    m_navLay->addSpacing(6);

    // 搜索：设置项变多之后，靠翻找太慢
    m_search = new QLineEdit(m_navHost);
    m_search->setPlaceholderText(QStringLiteral("搜索设置…"));
    m_search->setClearButtonEnabled(true);
    m_search->setFixedHeight(30);
    m_search->setStyleSheet(QStringLiteral(
        "QLineEdit{background-color:%1;border:1px solid %2;border-radius:6px;"
        "padding:2px 8px;font-size:10pt;color:%3;}"
        "QLineEdit:focus{border-color:%4;}")
                                .arg(theme::colors::panel().name(), theme::colors::line().name(),
                                     theme::colors::text().name(), theme::colors::accent().name()));
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& q) { filterNav(q); });
    m_navLay->addWidget(m_search);
    m_navLay->addSpacing(6);

    m_emptyHint = new QLabel(QStringLiteral("没有匹配的设置项"), m_navHost);
    m_emptyHint->setStyleSheet(QStringLiteral("color:%1;font-size:10pt;padding:8px 6px;")
                                   .arg(theme::colors::textDim().name()));
    m_emptyHint->setVisible(false);
    m_navLay->addWidget(m_emptyHint);
    m_navLay->addStretch(1);

    // 导航条目与堆叠页在同一处依次登记：addNavEntry 返回的序号即随后 addWidget 的页序号，
    // 杜绝"导航顺序 ≠ 页序"的错位（此前分两处写会重复登记分组与条目）
    m_pages = new QStackedWidget(this);

    addNavSection(QStringLiteral("基础设置"));
    int p = addNavEntry(QStringLiteral("⚙"), QStringLiteral("常规"), QStringLiteral("基础设置"));
    m_pages->addWidget(buildGeneralPage()); // p
    p = addNavEntry(QStringLiteral("◑"), QStringLiteral("外观与主题"), QStringLiteral("基础设置"));
    m_pages->addWidget(buildAppearancePage()); // p
    p = addNavEntry(QStringLiteral("◈"), QStringLiteral("模型与供应商"), QStringLiteral("基础设置"));
    m_pages->addWidget(buildProviderPage()); // p

    addNavSection(QStringLiteral("Agent 能力"));
    p = addNavEntry(QStringLiteral("◇"), QStringLiteral("记忆与检索"), QStringLiteral("Agent 能力"));
    m_pages->addWidget(buildMemoryPage()); // p
    p = addNavEntry(QStringLiteral("✧"), QStringLiteral("技能库"), QStringLiteral("Agent 能力"));
    m_pages->addWidget(panels.skills ? panels.skills : new QWidget(this)); // p
    p = addNavEntry(QStringLiteral("⧗"), QStringLiteral("任务与预算"), QStringLiteral("Agent 能力"));
    m_pages->addWidget(buildBudgetPage()); // p
    p = addNavEntry(QStringLiteral("✦"), QStringLiteral("蜂巢互联"), QStringLiteral("Agent 能力"));
    m_pages->addWidget(buildHivePage()); // p

    addNavSection(QStringLiteral("数据与统计"));
    p = addNavEntry(QStringLiteral("▤"), QStringLiteral("任务队列"), QStringLiteral("数据与统计"));
    m_pages->addWidget(panels.taskQueue ? panels.taskQueue : new QWidget(this)); // p
    p = addNavEntry(QStringLiteral("▦"), QStringLiteral("审计日志"), QStringLiteral("数据与统计"));
    m_pages->addWidget(panels.audit ? panels.audit : new QWidget(this)); // p
    p = addNavEntry(QStringLiteral("✉"), QStringLiteral("邮件通知"), QStringLiteral("数据与统计"));
    m_pages->addWidget(buildMailPage()); // p

    auto* stackRow = new QWidget(this);
    auto* lay = new QHBoxLayout(stackRow);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    // 内容页套滚动区（关键修复）：设置页内部含宽表格与长文本，其 minimumSizeHint 会沿着
    // 布局一路顶成主窗口的最小宽度——设置页实测 minW=1222px，窗口因此被撑得比屏幕还宽，
    // 最左边的会话栏第一个被挤出可视区（实机反馈"点设置后左边会话列表消失、设置铺满左侧"）。
    // 套上 widgetResizable 滚动区后，宽度不足时出水平滚动条，窗口最小宽度不再被设置页绑架。
    auto* pageScroll = new QScrollArea(this);
    pageScroll->setWidgetResizable(true);
    pageScroll->setFrameShape(QFrame::NoFrame);
    pageScroll->setWidget(m_pages);
    lay->addWidget(m_navHost);
    lay->addWidget(pageScroll, 1);

    auto* bottom = new QHBoxLayout();
    bottom->setContentsMargins(12, 8, 12, 10);
    bottom->addStretch(1);
    auto* save = new QPushButton(QStringLiteral("保存"), this);
    save->setObjectName(QStringLiteral("primaryBtn"));
    save->setCursor(Qt::PointingHandCursor);
    connect(save, &QPushButton::clicked, this, &SettingsDialog::onSave);
    auto* close = new QPushButton(QStringLiteral("返回工作区"), this);
    close->setCursor(Qt::PointingHandCursor);
    connect(close, &QPushButton::clicked, this, &SettingsDialog::closeRequested);
    bottom->addWidget(save);
    bottom->addWidget(close);
    auto* bottomW = new QWidget(this);
    bottomW->setLayout(bottom);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(stackRow, 1);
    root->addWidget(bottomW);

    m_pages->setCurrentIndex(0);
    m_currentPage = 0;
    if (!m_navEntries.isEmpty() && m_navEntries[0].btn)
        m_navEntries[0].btn->setProperty("navActive", true);
    loadHive();
}

// 分组标题：小号、暗色、非交互（参考图的 "Personal / Integrations / Coding" 同款）
void SettingsDialog::addNavSection(const QString& title) {
    auto* l = new QLabel(title, m_navHost);
    l->setStyleSheet(QStringLiteral("color:%1;font-size:9.5pt;font-weight:bold;padding:10px 6px 2px 6px;")
                         .arg(theme::colors::textDim().name()));
    m_navLay->addWidget(l);
    m_sectionLabels.push_back({title, l});
}

// 导航条目：勾选式按钮（含线性图标），点击切页
int SettingsDialog::addNavEntry(const QString& icon, const QString& title, const QString& section) {
    const int page = m_pages->count(); // 调用方随后 addWidget，序号在此预留
    auto* btn = new QPushButton(QStringLiteral("%1   %2").arg(icon, title), m_navHost);
    btn->setCheckable(true);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFixedHeight(32);
    btn->setToolTip(title);
    btn->setStyleSheet(QStringLiteral(
        "QPushButton{background:transparent;border:none;border-radius:6px;text-align:left;"
        "padding-left:10px;color:%1;font-size:10.5pt;}"
        "QPushButton:hover{background-color:%2;}"
        "QPushButton:checked{background-color:%3;color:white;}"
        "QPushButton:focus{border:1px solid %3;}")
                           .arg(theme::colors::text().name(), theme::colors::panel().name(),
                                theme::colors::accent().name()));
    connect(btn, &QPushButton::clicked, this, [this, page] { selectNav(page); });
    m_navLay->addWidget(btn);
    m_navEntries.push_back({title, icon, section, page, btn});
    return page;
}

// 切页并把选中态收敛到唯一一个按钮（按钮不是 QButtonGroup 成员，需手动互斥）
void SettingsDialog::selectNav(int pageIndex) {
    if (pageIndex < 0 || pageIndex >= m_pages->count())
        return;
    m_pages->setCurrentIndex(pageIndex);
    m_currentPage = pageIndex;
    for (auto& e : m_navEntries) {
        if (e.btn)
            e.btn->setChecked(e.page == pageIndex);
    }
}

// 设置搜索：按标题过滤条目；某分组下条目全被滤掉时隐藏该分组标题
void SettingsDialog::filterNav(const QString& query) {
    const QString q = query.trimmed();
    QSet<QString> sectionsWithHit;
    for (auto& e : m_navEntries) {
        const bool hit = q.isEmpty() || e.title.contains(q, Qt::CaseInsensitive);
        if (e.btn)
            e.btn->setVisible(hit);
        if (hit)
            sectionsWithHit.insert(e.section);
    }
    for (const auto& s : m_sectionLabels) {
        if (s.second)
            s.second->setVisible(q.isEmpty() || sectionsWithHit.contains(s.first));
    }
    if (m_emptyHint)
        m_emptyHint->setVisible(!q.isEmpty() && sectionsWithHit.isEmpty());
}

// 直达某页（命令面板 / 视图菜单用）
void SettingsDialog::showPage(int pageIndex) {
    selectNav(pageIndex);
}

QWidget* SettingsDialog::buildGeneralPage() {
    auto* w = new QWidget(this);
    auto* form = new QFormLayout(w);
    form->setContentsMargins(20, 18, 20, 18);

    m_permCombo = new QComboBox(w);
    m_permCombo->addItems({QStringLiteral("Suggest（读自动/写提案/命令与网络禁止）"),
                           QStringLiteral("Auto Edit（工作区写自动，命令逐条确认）"),
                           QStringLiteral("Full Access（全自动，网络白名单放行）")});
    m_permCombo->setCurrentIndex(int(AppContext::instance().permissionMode));
    form->addRow(QStringLiteral("权限模式"), m_permCombo);
    // 说明性长文本一律自动换行：不换行时 QLabel 的最小宽度 = 整行文本宽度，
    // 会沿布局把设置页、进而主窗口的最小宽度顶到屏幕之外
    auto* permHint = new QLabel(
        QStringLiteral("与 Codex 同源权限模型对齐；Composer 底行可随时切换，两侧始终同一状态。"), w);
    permHint->setWordWrap(true);
    form->addRow(QString(), permHint);

    auto* langLabel = new QLabel(QStringLiteral("简体中文（内置文案），随系统输入法。"), w);
    form->addRow(QStringLiteral("语言"), langLabel);
    return w;
}

// 外观与主题：从原「通用」页拆出（对齐参考图把 General / Appearance 分开的分组方式）
QWidget* SettingsDialog::buildAppearancePage() {
    auto* w = new QWidget(this);
    auto* form = new QFormLayout(w);
    form->setContentsMargins(20, 18, 20, 18);

    m_paletteCombo = new QComboBox(w);
    for (const auto& pal : theme::palettes())
        m_paletteCombo->addItem(QString::fromUtf8(pal.zh));
    m_paletteCombo->setCurrentIndex(theme::paletteIndex());
    form->addRow(QStringLiteral("主题皮肤"), m_paletteCombo);
    auto* themeHint = new QLabel(
        QStringLiteral("四套色板：品牌「熔炉·铁灰炉火」为默认，另有 Codex/VS/Claude 致敬皮肤。\n"
                       "保存后**重启应用**完全生效；品牌行 ⌄ 菜单或 Ctrl+Shift+T 可快速切换。"), w);
    themeHint->setWordWrap(true);
    form->addRow(QString(), themeHint);
    return w;
}

QWidget* SettingsDialog::buildProviderPage() {
    auto* w = new QWidget(this);
    auto* lay = new QVBoxLayout(w);
    lay->setContentsMargins(20, 18, 20, 18);
    m_providerTable = new QTableWidget(int(m_pm->all().size()), 3, w);
    m_providerTable->setHorizontalHeaderLabels(
        {QStringLiteral("供应商"), QStringLiteral("接口地址 base_url"), QStringLiteral("API Key")});
    m_providerTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_providerTable->verticalHeader()->setVisible(false);
    int row = 0;
    for (const auto& cfg : m_pm->all()) {
        auto* nameItem = new QTableWidgetItem(cfg.name);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        m_providerTable->setItem(row, 0, nameItem);
        m_providerTable->setItem(row, 1, new QTableWidgetItem(cfg.baseUrl));
        auto* key = new QLineEdit(w);
        key->setEchoMode(QLineEdit::Password);
        key->setPlaceholderText(cfg.configured ? QStringLiteral("已配置（输入则覆盖）")
                                               : QStringLiteral("粘贴 API Key（DPAPI 加密存储）"));
        m_keyEdits.append(key);
        m_providerTable->setCellWidget(row, 2, key);
        ++row;
    }
    lay->addWidget(new QLabel(QStringLiteral("API Key 修改后点〔保存〕；落盘为 DPAPI 密文。"), w), 0);
    lay->addWidget(m_providerTable, 1);

    // ---- 语义检索（M6-A）：随所选供应商复用其 base_url 与 Key ----
    const auto& emb = m_pm->embedding();
    m_embEnabled = new QCheckBox(QStringLiteral("启用语义检索（FTS5 + 语义向量混合检索）"), w);
    m_embEnabled->setChecked(emb.enabled);
    m_embProvider = new QComboBox(w);
    m_embModel = new QLineEdit(emb.model, w);
    int embIdx = 0;
    for (int i = 0; i < m_pm->all().size(); ++i) {
        const auto& cfg = m_pm->all().at(i);
        m_embProvider->addItem(cfg.name);
        if (cfg.name == emb.provider)
            embIdx = i;
    }
    m_embProvider->setCurrentIndex(embIdx);
    auto* embForm = new QFormLayout();
    embForm->addRow(m_embEnabled);
    embForm->addRow(QStringLiteral("嵌入供应商"), m_embProvider);
    embForm->addRow(QStringLiteral("嵌入模型"), m_embModel);
    lay->addLayout(embForm);
    auto* embNote = new QLabel(QStringLiteral("复用所选供应商的接口地址与 API Key；修改后重启应用生效。"), w);
    embNote->setStyleSheet(QStringLiteral("color:%1;font-size:9.5pt;").arg(theme::colors::textDim().name()));
    lay->addWidget(embNote);
    return w;
}

QWidget* SettingsDialog::buildMailPage() {
    m_mail->loadConfig();
    const auto& cfg = m_mail->config();
    auto* w = new QWidget(this);
    auto* form = new QFormLayout(w);
    form->setContentsMargins(20, 18, 20, 18);
    m_smtpUrl = new QLineEdit(cfg.smtpUrl.isEmpty() ? QStringLiteral("smtps://smtp.qq.com:465")
                                                    : cfg.smtpUrl, w);
    m_from = new QLineEdit(cfg.from, w);
    m_mailPass = new QLineEdit(cfg.authCode, w);
    m_mailPass->setEchoMode(QLineEdit::Password);
    m_mailPass->setPlaceholderText(QStringLiteral("授权码（不是邮箱登录密码；QQ/163 需开启 SMTP 后生成）"));
    m_to = new QLineEdit(cfg.to, w);
    form->addRow(QStringLiteral("SMTP 服务器"), m_smtpUrl);
    form->addRow(QStringLiteral("发件人"), m_from);
    form->addRow(QStringLiteral("授权码"), m_mailPass);
    form->addRow(QStringLiteral("收件人"), m_to);
    auto* testRow = new QHBoxLayout();
    auto* testBtn = new QPushButton(QStringLiteral("发测试邮件"), w);
    connect(testBtn, &QPushButton::clicked, this, &SettingsDialog::onSendTestMail);
    testRow->addStretch(1);
    testRow->addWidget(testBtn);
    form->addRow(testRow);
    return w;
}

QWidget* SettingsDialog::buildBudgetPage() {
    const auto& limits = AppContext::instance().limits;
    auto* w = new QWidget(this);
    auto* form = new QFormLayout(w);
    form->setContentsMargins(20, 18, 20, 18);
    m_maxRounds = new QSpinBox(w);
    m_maxRounds->setRange(3, 200);
    m_maxRounds->setValue(limits.maxRounds);
    m_maxTokens = new QSpinBox(w);
    m_maxTokens->setRange(10000, 100000000);
    m_maxTokens->setSingleStep(50000);
    m_maxTokens->setValue(int(limits.maxTokens));
    m_maxSameFail = new QSpinBox(w);
    m_maxSameFail->setRange(2, 20);
    m_maxSameFail->setValue(limits.maxSameFailures);
    form->addRow(QStringLiteral("轮数上限"), m_maxRounds);
    form->addRow(QStringLiteral("单任务 token 限额"), m_maxTokens);
    form->addRow(QStringLiteral("连续相同失败次数"), m_maxSameFail);
    form->addRow(QString(), new QLabel(QStringLiteral("熔断条件：轮数 / token / 同类失败次数任一触顶即熔断。"), w));
    return w;
}

QWidget* SettingsDialog::buildMemoryPage() {
    auto* w = new QWidget(this);
    auto* form = new QFormLayout(w);
    form->setContentsMargins(20, 18, 20, 18);
    m_l1Limit = new QSpinBox(w);
    m_l1Limit->setRange(500, 100000);
    m_l1Limit->setSingleStep(500);
    m_l1Limit->setValue(AppContext::instance().l1TokenLimit);
    form->addRow(QStringLiteral("L1 核心记忆上限（tokens）"), m_l1Limit);

    m_memApproval = new QCheckBox(QStringLiteral("记忆写入需人工批准（暂存为待审，记忆页可批准/拒绝）"), w);
    m_memApproval->setChecked(AppContext::instance().memoryWriteApproval);
    form->addRow(QStringLiteral("写入审批门"), m_memApproval);

    auto* embLabel = new QLabel(
        QStringLiteral("语义检索（L3 记忆）：FTS5 关键词 + 语义向量双通道融合。\n"
                       "嵌入服务随「供应商」页启用（providers.json 顶层 embedding 节，模型默认 embedding-3）。"), w);
    embLabel->setWordWrap(true);
    form->addRow(QStringLiteral("语义检索"), embLabel);
    return w;
}

QWidget* SettingsDialog::buildHivePage() {
    auto* w = new QWidget(this);
    auto* form = new QFormLayout(w);
    form->setContentsMargins(20, 18, 20, 18);

    m_hiveEnabled = new QCheckBox(QStringLiteral("启用（连接本机 AgentHive 服务）"), w);
    m_hiveHost = new QLineEdit(QStringLiteral("127.0.0.1"), w);
    m_hivePort = new QLineEdit(QStringLiteral("8787"), w);
    m_hivePort->setMaximumWidth(90);
    auto* addrRow = new QHBoxLayout();
    addrRow->addWidget(m_hiveHost);
    addrRow->addWidget(m_hivePort);
    auto* addrW = new QWidget(w);
    addrW->setLayout(addrRow);

    m_hiveName = new QLineEdit(QStringLiteral("Miderforge"), w);
    m_hiveKey = new QLineEdit(w);
    m_hiveKey->setEchoMode(QLineEdit::Password);
    m_hiveKey->setPlaceholderText(QStringLiteral("agent-cli register 得到的主密钥（DPAPI 加密存储）"));

    m_hiveStatus = new QLabel(QStringLiteral("未测试"), w);
    m_hiveStatus->setWordWrap(true);
    auto* testRow = new QHBoxLayout();
    testRow->addWidget(m_hiveStatus, 1);
    auto* testBtn = new QPushButton(QStringLiteral("测试连接"), w);
    connect(testBtn, &QPushButton::clicked, this, &SettingsDialog::onTestHive);
    testRow->addWidget(testBtn);
    auto* testW = new QWidget(w);
    testW->setLayout(testRow);

    form->addRow(QString(), m_hiveEnabled);
    form->addRow(QStringLiteral("服务地址"), addrW);
    form->addRow(QStringLiteral("Agent 名称"), m_hiveName);
    form->addRow(QStringLiteral("主密钥"), m_hiveKey);
    form->addRow(QStringLiteral("连接状态"), testW);
    auto* hiveHint = new QLabel(
        QStringLiteral("AgentHive 是本机多 Agent 协作平台（独立开源项目）。\n"
                       "仅连接本机环回地址，未开启时 Miderforge 不发任何请求。"
                       "健康检查走 GET /api/health，无鉴权；其余接口使用上方主密钥。"), w);
    hiveHint->setWordWrap(true);
    form->addRow(QString(), hiveHint);
    return w;
}

void SettingsDialog::loadHive() {
    QFile f(appdirs::file(QString::fromLatin1(kHiveRel)));
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    m_hiveEnabled->setChecked(obj.value(QStringLiteral("enabled")).toBool(false));
    if (!obj.value(QStringLiteral("host")).toString().isEmpty())
        m_hiveHost->setText(obj.value(QStringLiteral("host")).toString());
    if (!obj.value(QStringLiteral("port")).toString().isEmpty())
        m_hivePort->setText(obj.value(QStringLiteral("port")).toString());
    if (!obj.value(QStringLiteral("agentName")).toString().isEmpty())
        m_hiveName->setText(obj.value(QStringLiteral("agentName")).toString());
    const QString b64 = obj.value(QStringLiteral("masterKey")).toString();
    if (!b64.isEmpty()) {
        m_hiveKeyCipher = b64.toLatin1();
        m_hiveKey->setPlaceholderText(QStringLiteral("已配置主密钥（输入则覆盖）"));
    }
}

void SettingsDialog::saveHive() {
    QDir().mkpath(appdirs::file(QStringLiteral("config")));
    QJsonObject obj;
    obj[QStringLiteral("enabled")] = m_hiveEnabled->isChecked();
    obj[QStringLiteral("host")] = m_hiveHost->text().trimmed();
    obj[QStringLiteral("port")] = m_hivePort->text().trimmed();
    obj[QStringLiteral("agentName")] = m_hiveName->text().trimmed();
    const QString key = m_hiveKey->text();
    if (!key.isEmpty()) {
        const auto enc = dpapi::encryptToBase64(key);
        if (enc) {
            m_hiveKeyCipher = enc->toLatin1();
            obj[QStringLiteral("masterKey")] = *enc;
        } else {
            m_hiveStatus->setText(QStringLiteral("主密钥加密失败（DPAPI）"));
        }
    } else if (!m_hiveKeyCipher.isEmpty()) {
        obj[QStringLiteral("masterKey")] = QString::fromLatin1(m_hiveKeyCipher);
    }
    // 允许环回外的地址一律拒绝（连接只面向本机服务）
    if (!isLoopbackHost(m_hiveHost->text().trimmed())) {
        m_hiveStatus->setText(
            QStringLiteral("地址 %1 非本机回环，已拒绝：AgentHive 只支持本机连接。").arg(m_hiveHost->text().trimmed()));
        obj[QStringLiteral("host")] = QStringLiteral("127.0.0.1");
    }
    QFile f(appdirs::file(QString::fromLatin1(kHiveRel)));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
        f.close();
    }
}

void SettingsDialog::onTestHive() {
    const QString host = m_hiveHost->text().trimmed().isEmpty() ? QStringLiteral("127.0.0.1")
                                                                : m_hiveHost->text().trimmed();
    const quint16 port = static_cast<quint16>(m_hivePort->text().trimmed().toUShort());
    if (!isLoopbackHost(host)) {
        m_hiveStatus->setText(QStringLiteral("已拒绝：仅允许 127.0.0.1 / ::1 / localhost（设计约束：本机服务）"));
        return;
    }
    m_hiveResp.clear();
    m_hiveStatus->setText(QStringLiteral("正在连接 %1:%2（/api/health）…").arg(host).arg(port));
    auto* sock = new QTcpSocket(this);
    connect(sock, &QTcpSocket::connected, sock, [sock] {
        sock->write("GET /api/health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    });
    connect(sock, &QTcpSocket::readyRead, sock, [this, sock] {
        m_hiveResp += sock->readAll();
    });
    connect(sock, &QTcpSocket::disconnected, sock, [this, sock] {
        if (!m_hiveResp.isEmpty())
            m_hiveStatus->setText(friendlyHealth(m_hiveResp));
        sock->deleteLater();
    });
    connect(sock, &QTcpSocket::errorOccurred, sock, [this, sock, port](QAbstractSocket::SocketError) {
        // 已收到响应后的连接关闭（Connection: close）不按失败处理，避免覆盖成功文案
        if (m_hiveResp.isEmpty())
            m_hiveStatus->setText(
                QStringLiteral("❌ 连接失败：%1（请确认 AgentHive 服务已在本机 %2 端口启动）")
                    .arg(sock->errorString()).arg(port));
        sock->deleteLater();
    });
    QTimer::singleShot(4000, sock, [sock] {
        if (sock->state() != QAbstractSocket::UnconnectedState)
            sock->abort();
    });
    sock->connectToHost(host, port);
}

void SettingsDialog::onSave() {
    // 供应商：base_url 与 Key
    int row = 0;
    for (const auto& cfg : m_pm->all()) {
        const QString newUrl = m_providerTable->item(row, 1)->text().trimmed();
        if (!newUrl.isEmpty() && newUrl != cfg.baseUrl)
            m_pm->setBaseUrl(cfg.name, newUrl);
        const QString key = m_keyEdits.at(row)->text().trimmed();
        if (!key.isEmpty())
            m_pm->saveKey(cfg.name, key);
        ++row;
    }
    // 语义检索（M6-A）：重启后经 main.cpp 装配生效
    EmbeddingConfig emb;
    emb.enabled = m_embEnabled->isChecked();
    emb.provider = m_embProvider->currentText();
    emb.model = m_embModel->text().trimmed().isEmpty() ? QStringLiteral("embedding-3")
                                                       : m_embModel->text().trimmed();
    m_pm->setEmbeddingConfig(emb);
    // 权限模式（与主窗口同源）
    if (m_applyPermissionMode)
        m_applyPermissionMode(m_permCombo->currentIndex());
    // 主题皮肤
    theme::setPaletteIndex(m_paletteCombo->currentIndex());
    // 邮件
    auto cfg = m_mail->config();
    cfg.smtpUrl = m_smtpUrl->text().trimmed();
    cfg.from = m_from->text().trimmed();
    cfg.to = m_to->text().trimmed();
    if (!m_mailPass->text().isEmpty())
        cfg.authCode = m_mailPass->text();
    cfg.enabled = !cfg.from.isEmpty() && !cfg.to.isEmpty() && !cfg.authCode.isEmpty();
    m_mail->saveConfig(cfg);
    // 预算与记忆
    auto& limits = AppContext::instance().limits;
    limits.maxRounds = m_maxRounds->value();
    limits.maxTokens = m_maxTokens->value();
    limits.maxSameFailures = m_maxSameFail->value();
    AppContext::instance().l1TokenLimit = m_l1Limit->value();
    AppContext::instance().memoryWriteApproval = m_memApproval->isChecked();
    // 蜂巢
    saveHive();
    // 嵌入模式下不再 accept()（无对话框可关）：改为广播"已保存"，由主窗口刷新派生显示。
    // 停留当前页——用户可能还要继续改别的设置，保存不应把人踢走。
    Toast::post(window(), QStringLiteral("设置已保存"), Toast::Level::Success, 1800);
    emit saved();
}

void SettingsDialog::onSendTestMail() {
    // 先暂存当前表单再发（避免“填完没保存测试的是旧配置”）
    auto cfg = m_mail->config();
    cfg.smtpUrl = m_smtpUrl->text().trimmed();
    cfg.from = m_from->text().trimmed();
    cfg.to = m_to->text().trimmed();
    if (!m_mailPass->text().isEmpty())
        cfg.authCode = m_mailPass->text();
    cfg.enabled = true;
    m_mail->saveConfig(cfg);

    QString err;
    if (m_mail->send(QStringLiteral("Miderforge 测试邮件"),
                     QStringLiteral("<h3>Miderforge 邮件通知已就绪</h3><p>任务完成/失败/熔断时将发信至此。</p>"),
                     &err))
        QMessageBox::information(this, QStringLiteral("Miderforge"), QStringLiteral("测试邮件已发送，请查收。"));
    else
        QMessageBox::warning(this, QStringLiteral("Miderforge"),
                             QStringLiteral("发送失败：%1\n\n请检查：服务器地址/端口、授权码（非登录密码）、SMTP 服务是否已开启。").arg(err));
}

} // namespace miderforge
