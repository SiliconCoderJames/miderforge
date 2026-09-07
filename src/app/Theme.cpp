// 深色主题实现
#include "app/Theme.h"
#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace miderforge::theme {

void apply(QApplication& app) {
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QPalette pal;
    pal.setColor(QPalette::Window, colors::window);
    pal.setColor(QPalette::WindowText, colors::text);
    pal.setColor(QPalette::Base, colors::panel);
    pal.setColor(QPalette::AlternateBase, colors::window);
    pal.setColor(QPalette::ToolTipBase, colors::panel);
    pal.setColor(QPalette::ToolTipText, colors::text);
    pal.setColor(QPalette::Text, colors::text);
    pal.setColor(QPalette::Button, colors::panel);
    pal.setColor(QPalette::ButtonText, colors::text);
    pal.setColor(QPalette::BrightText, Qt::white);
    pal.setColor(QPalette::Link, colors::accent);
    pal.setColor(QPalette::Highlight, colors::accent);
    pal.setColor(QPalette::HighlightedText, Qt::white);
    pal.setColor(QPalette::Disabled, QPalette::Text, colors::textDim);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, colors::textDim);
    pal.setColor(QPalette::Disabled, QPalette::Window, colors::window);
    app.setPalette(pal);

    app.setFont(uiFont());
}

QFont uiFont() {
    return QFont(QStringLiteral("Microsoft YaHei UI"), 9);
}

QFont monoFont() {
    return QFont(QStringLiteral("Consolas"), 9);
}

QString coloredDot(const QColor& c) {
    return QStringLiteral("<span style=\"color:%1\">●</span>").arg(c.name());
}

} // namespace miderforge::theme
