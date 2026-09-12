// 内置查看器实现
#include "app/PreviewPane.h"

#include "app/ChatWidgets.h"   // Toast
#include "app/PreviewDoc.h"
#include "app/Theme.h"
#include "core/AppContext.h"
#include "tools/PermissionGate.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QSplitter>
#include <QVBoxLayout>

namespace miderforge {

namespace {
constexpr int kMaxFiles = 400;   // 文件列表上限（大仓库不至于卡住 UI）
constexpr int kMaxPreviewBytes = 1024 * 1024;
constexpr int kMaxDepth = 4;
} // namespace

PreviewPane::PreviewPane(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("PreviewPane{background-color:%1;}").arg(theme::colors::window().name()));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(theme::metrics::spaceLg, theme::metrics::spaceMd,
                             theme::metrics::spaceLg, theme::metrics::spaceMd);
    root->setSpacing(theme::metrics::spaceSm);

    // ---- 标题行 ----
    auto* head = new QHBoxLayout();
    head->setSpacing(theme::metrics::spaceSm);
    auto* heading = new QLabel(QStringLiteral("📄 查看器"), this);
    heading->setStyleSheet(QStringLiteral("color:%1;font-size:13pt;font-weight:600;")
                               .arg(theme::colors::brand().name()));
    m_title = new QLabel(this);
    m_title->setStyleSheet(QStringLiteral("color:%1;font-size:10pt;").arg(theme::colors::textDim().name()));
    head->addWidget(heading);
    head->addWidget(m_title);
    head->addStretch(1);
    m_editToggle = new QToolButton(this);
    m_editToggle->setText(QStringLiteral("✏ 编辑"));
    m_editToggle->setCheckable(true);
    m_editToggle->setCursor(Qt::PointingHandCursor);
    m_editToggle->setToolTip(QStringLiteral("切换到源码编辑（保存前会过权限判定）"));
    m_editToggle->setStyleSheet(QStringLiteral(
        "QToolButton{background:transparent;border:1px solid %1;border-radius:6px;padding:3px 10px;"
        "color:%2;font-size:10pt;}"
        "QToolButton:hover{background-color:%3;}"
        "QToolButton:checked{background-color:%4;color:white;border-color:%4;}")
                                    .arg(theme::colors::line().name(), theme::colors::text().name(),
                                         theme::colors::btnHover().name(), theme::colors::accent().name()));
    m_saveBtn = new QPushButton(QStringLiteral("保存"), this);
    m_saveBtn->setObjectName(QStringLiteral("primaryBtn"));
    m_saveBtn->setCursor(Qt::PointingHandCursor);
    m_saveBtn->setVisible(false);
    connect(m_editToggle, &QToolButton::toggled, this, &PreviewPane::setEditing);
    connect(m_saveBtn, &QPushButton::clicked, this, &PreviewPane::saveEdits);
    head->addWidget(m_editToggle);
    head->addWidget(m_saveBtn);
    root->addLayout(head);

    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);
    m_hint->setStyleSheet(QStringLiteral("color:%1;font-size:9.5pt;").arg(theme::colors::textDim().name()));
    root->addWidget(m_hint);

    // ---- 左：文件列表；右：预览/编辑 ----
    auto* split = new QSplitter(Qt::Horizontal, this);
    m_files = new QListWidget(split);
    m_files->setMinimumWidth(200);
    m_files->setStyleSheet(QStringLiteral(
        "QListWidget{background-color:%1;border:1px solid %2;border-radius:8px;font-size:10pt;"
        "outline:none;padding:4px;}"
        "QListWidget::item{height:32px;border-radius:5px;padding-left:6px;color:%3;}"
        "QListWidget::item:hover{background-color:%4;}"
        "QListWidget::item:selected{background-color:%5;color:white;}")
                               .arg(theme::colors::panel().name(), theme::colors::line().name(),
                                    theme::colors::text().name(), theme::colors::btnHover().name(),
                                    theme::colors::accent().name()));

    m_view = new QTextBrowser(split);
    m_view->setOpenExternalLinks(true);
    m_view->setStyleSheet(QStringLiteral(
        "QTextBrowser{background-color:%1;border:1px solid %2;border-radius:8px;padding:10px;"
        "font-size:11pt;color:%3;}")
                              .arg(theme::colors::panel().name(), theme::colors::line().name(),
                                   theme::colors::text().name()));

    m_editor = new QPlainTextEdit(split);
    m_editor->setVisible(false);
    m_editor->setStyleSheet(QStringLiteral(
        "QPlainTextEdit{background-color:%1;border:1px solid %2;border-radius:8px;padding:8px;"
        "font-family:'Consolas';font-size:10pt;color:%3;}")
                                .arg(theme::colors::codeBg().name(), theme::colors::line().name(),
                                     theme::colors::text().name()));

    split->addWidget(m_files);
    split->addWidget(m_view);
    split->addWidget(m_editor);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setStretchFactor(2, 1);
    root->addWidget(split, 1);

    connect(m_files, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) {
        if (it)
            openFile(it->data(Qt::UserRole).toString());
    });
    connect(m_files, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        if (it)
            openFile(it->data(Qt::UserRole).toString());
    });

    setWorkspaceRoot(AppContext::instance().workspaceRoot);
}

// 保护区/名字清单命中即拒：与工具层共用同一判定，查看器不是读后门
bool PreviewPane::pathBlocked(const QString& path, QString* reason) const {
    const QString hard = PermissionGate().hardDenyReason(PermissionGate::Kind::ReadFile, path,
                                                        AppContext::instance().workspaceRoot);
    if (hard.isEmpty())
        return false;
    if (reason)
        *reason = hard;
    return true;
}

void PreviewPane::setWorkspaceRoot(const QString& root) {
    m_root = root;
    refreshFileList();
}

void PreviewPane::refreshFileList() {
    m_files->clear();
    if (m_root.isEmpty() || !QDir(m_root).exists()) {
        m_hint->setText(QStringLiteral("工作区目录不存在，先在左栏顶部选择一个工作目录。"));
        return;
    }
    int shown = 0;
    QDirIterator it(m_root, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext() && shown < kMaxFiles) {
        const QString path = it.next();
        const QString rel = QDir(m_root).relativeFilePath(path);
        if (rel.count(QLatin1Char('/')) > kMaxDepth)
            continue;
        if (rel.startsWith(QStringLiteral(".git/")) || rel.contains(QStringLiteral("/.git/"))
            || rel.startsWith(QStringLiteral("build/")) || rel.contains(QStringLiteral("/build/")))
            continue;
        if (!previewdoc::isPreviewable(path))
            continue;
        QString why;
        if (pathBlocked(path, &why)) // 保护区文件不出现在列表里
            continue;
        auto* item = new QListWidgetItem(rel, m_files);
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
        ++shown;
    }
    if (shown == 0)
        m_hint->setText(QStringLiteral("工作区里还没有可查看的文件"
                                       "（支持 Markdown / HTML / 文本 / 代码等）"));
    else
        m_hint->setText(QStringLiteral("共 %1 个可查看文件 · 点击左侧打开 · Markdown 与 HTML 渲染预览，"
                                       "其余按源码显示")
                            .arg(shown));
}

bool PreviewPane::openFile(const QString& path) {
    QString err;
    if (!loadInto(path, &err)) {
        m_hint->setText(err);
        emit statusMessage(err);
        return false;
    }
    return true;
}

bool PreviewPane::loadInto(const QString& path, QString* err) {
    if (path.isEmpty()) {
        if (err) *err = QStringLiteral("未选择文件");
        return false;
    }
    QString why;
    if (pathBlocked(path, &why)) {
        if (err) *err = QStringLiteral("该文件受保护，查看器不予打开（%1）").arg(why);
        return false;
    }
    QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        if (err) *err = QStringLiteral("文件不存在：%1").arg(path);
        return false;
    }
    if (info.size() > kMaxPreviewBytes) {
        if (err) *err = QStringLiteral("文件过大（%1 MB），查看器上限 1MB").arg(info.size() / 1024 / 1024);
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("无法读取：%1").arg(f.errorString());
        return false;
    }
    m_rawText = QString::fromUtf8(f.readAll());
    f.close();
    m_path = path;
    m_title->setText(QStringLiteral("· %1（%2）")
                         .arg(info.fileName(), previewdoc::modeName(path)));
    render();
    return true;
}

void PreviewPane::render() {
    if (m_path.isEmpty()) {
        m_view->setHtml(QStringLiteral("<div style=\"color:%1;font-size:11pt;\">"
                                       "从左侧选一个文件开始查看。</div>")
                            .arg(theme::colors::textDim().name()));
        return;
    }
    m_view->setHtml(previewdoc::render(m_path, m_rawText));
}

void PreviewPane::setEditing(bool on) {
    if (on && m_path.isEmpty()) {
        m_editToggle->setChecked(false);
        m_hint->setText(QStringLiteral("先选一个文件再编辑。"));
        return;
    }
    m_editing = on;
    m_view->setVisible(!on);
    m_editor->setVisible(on);
    m_saveBtn->setVisible(on);
    if (on)
        m_editor->setPlainText(m_rawText);
    else
        render();
}

void PreviewPane::saveEdits() {
    if (m_path.isEmpty())
        return;
    // 与工具层同一套权限判定：UI 保存不开后门
    const auto decision = PermissionGate().evaluate(AppContext::instance().permissionMode,
                                                    PermissionGate::Kind::WriteFile, m_path,
                                                    AppContext::instance().workspaceRoot);
    if (decision != PermissionGate::Decision::Allowed) {
        const QString msg = decision == PermissionGate::Decision::Denied
                                ? QStringLiteral("保存被拒绝：目标不在工作区内或命中永不解禁清单。")
                                : QStringLiteral("当前权限档下写文件需逐次确认——切到 Auto Edit / "
                                                 "Full Access 后再保存。");
        m_hint->setText(msg);
        Toast::post(this, msg, Toast::Level::Warn, 2600);
        return;
    }
    QFile f(m_path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_hint->setText(QStringLiteral("保存失败：%1").arg(f.errorString()));
        return;
    }
    const QByteArray bytes = m_editor->toPlainText().toUtf8();
    const qint64 n = f.write(bytes);
    f.close();
    if (n != bytes.size()) {
        m_hint->setText(QStringLiteral("保存不完整（写入 %1 / %2 字节）").arg(n).arg(bytes.size()));
        return;
    }
    m_rawText = m_editor->toPlainText();
    Toast::post(this, QStringLiteral("已保存 %1（%2 字节）").arg(QFileInfo(m_path).fileName()).arg(n),
                Toast::Level::Success, 2000);
    m_hint->setText(QStringLiteral("已保存 · %1").arg(m_path));
    render();
}

} // namespace miderforge
