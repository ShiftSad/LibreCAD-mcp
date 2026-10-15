/*******************************************************************************
 *
 This file is part of the LibreCAD project, a 2D CAD program

 Copyright (C) 2026 LibreCAD-mcp contributors

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 ******************************************************************************/

#include "lc_remoteapioptionswidget.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "lc_remoteapicontroller.h"

LC_RemoteApiOptionsWidget::LC_RemoteApiOptionsWidget(QWidget* parent, const LC_RemoteApiController* controller)
    : QWidget(parent) {
    setObjectName(QStringLiteral("remoteApiOptions"));
    const LC_RemoteApiConfig config = LC_RemoteApiConfig::load();
    m_token = config.token;

    auto* mainLayout = new QVBoxLayout(this);

    // ---- server
    auto* serverBox = new QGroupBox(tr("Server"), this);
    auto* serverLayout = new QFormLayout(serverBox);

    m_cbEnable = new QCheckBox(tr("Enable Remote API (local MCP server access)"), serverBox);
    m_cbEnable->setToolTip(tr("Lets programs on this computer that know the token read and edit open drawings."));
    m_cbEnable->setObjectName(QStringLiteral("cbRemoteApiEnable"));
    m_cbEnable->setChecked(config.enabled);
    serverLayout->addRow(m_cbEnable);

    m_sbPort = new QSpinBox(serverBox);
    m_sbPort->setRange(LC_RemoteApiConfig::MinPort, LC_RemoteApiConfig::MaxPort);
    m_sbPort->setObjectName(QStringLiteral("sbRemoteApiPort"));
    m_sbPort->setValue(config.port);
    m_sbPort->setToolTip(tr("The server listens on 127.0.0.1 (this computer only) at this port. Default: %1.")
                             .arg(LC_RemoteApiConfig::DefaultPort));
    serverLayout->addRow(tr("Port:"), m_sbPort);

    auto* tokenRow = new QHBoxLayout();
    m_leToken = new QLineEdit(serverBox);
    m_leToken->setObjectName(QStringLiteral("leRemoteApiToken"));
    m_leToken->setReadOnly(true);
    m_leToken->setEchoMode(QLineEdit::Password);
    m_leToken->setText(m_token);
    m_leToken->setToolTip(tr("Clients send this token with every request (X-Api-Token)."));
    m_pbShowToken = new QPushButton(tr("Show"), serverBox);
    m_pbShowToken->setCheckable(true);
    m_pbCopyToken = new QPushButton(tr("Copy"), serverBox);
    m_pbRegenerate = new QPushButton(tr("Regenerate"), serverBox);
    m_pbRegenerate->setObjectName(QStringLiteral("pbRemoteApiRegenerate"));
    m_pbRegenerate->setToolTip(tr("Creates a new token when you press OK; clients using the old one stop working."));
    tokenRow->addWidget(m_leToken, 1);
    tokenRow->addWidget(m_pbShowToken);
    tokenRow->addWidget(m_pbCopyToken);
    tokenRow->addWidget(m_pbRegenerate);
    serverLayout->addRow(tr("Token:"), tokenRow);

    m_lTokenNote = new QLabel(serverBox);
    m_lTokenNote->setWordWrap(true);
    m_lTokenNote->setVisible(false);
    serverLayout->addRow(QString(), m_lTokenNote);

    m_lStatus = new QLabel(controller != nullptr ? controller->statusText() : tr("Disabled"), serverBox);
    m_lStatus->setObjectName(QStringLiteral("lRemoteApiStatus"));
    m_lStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    serverLayout->addRow(tr("Status:"), m_lStatus);
    mainLayout->addWidget(serverBox);

    // ---- clients
    auto* clientsBox = new QGroupBox(tr("MCP clients"), this);
    auto* clientsLayout = new QVBoxLayout(clientsBox);
    auto* clientsText = new QLabel(
        tr("The librecad-mcp server finds this LibreCAD and its token through the discovery file below, "
           "so a client on this computer needs no further setup. "
           "\"Copy MCP config\" copies a ready .mcp.json entry; it holds no token."),
        clientsBox);
    clientsText->setWordWrap(true);
    clientsLayout->addWidget(clientsText);
    auto* discovery = new QLabel(tr("Discovery file: %1")
                                     .arg(QDir::toNativeSeparators(LC_RemoteApiConfig::discoveryFilePath())),
                                 clientsBox);
    discovery->setWordWrap(true);
    discovery->setTextInteractionFlags(Qt::TextSelectableByMouse);
    clientsLayout->addWidget(discovery);
    m_pbCopyConfig = new QPushButton(tr("Copy MCP config"), clientsBox);
    auto* copyRow = new QHBoxLayout();
    copyRow->addWidget(m_pbCopyConfig);
    copyRow->addStretch(1);
    clientsLayout->addLayout(copyRow);
    mainLayout->addWidget(clientsBox);

    // ---- permissions (read again on every request: no restart needed)
    auto* policyBox = new QGroupBox(tr("Permissions"), this);
    auto* policyLayout = new QFormLayout(policyBox);
    m_cbNotify = new QCheckBox(tr("Show API activity in the command line"), policyBox);
    m_cbNotify->setObjectName(QStringLiteral("cbRemoteApiNotify"));
    m_cbNotify->setChecked(config.notify);
    policyLayout->addRow(m_cbNotify);
    m_cbAllowCommands = new QCheckBox(tr("Allow running command-line commands"), policyBox);
    m_cbAllowCommands->setObjectName(QStringLiteral("cbRemoteApiAllowCommands"));
    m_cbAllowCommands->setChecked(config.allowCommands);
    policyLayout->addRow(m_cbAllowCommands);
    m_leAllowedRoots = new QLineEdit(config.allowedRoots.join(QLatin1Char(';')), policyBox);
    m_leAllowedRoots->setObjectName(QStringLiteral("leRemoteApiAllowedRoots"));
    m_leAllowedRoots->setPlaceholderText(tr("any folder"));
    m_leAllowedRoots->setToolTip(tr("Folders the API may open and write files in, separated by ';'. "
                                    "Empty: any absolute path on this computer."));
    policyLayout->addRow(tr("Restrict file access to these folders:"), m_leAllowedRoots);
    m_cbAllowNetworkPaths = new QCheckBox(tr("Allow network (UNC) paths"), policyBox);
    m_cbAllowNetworkPaths->setObjectName(QStringLiteral("cbRemoteApiAllowNetworkPaths"));
    m_cbAllowNetworkPaths->setChecked(config.allowNetworkPaths);
    policyLayout->addRow(m_cbAllowNetworkPaths);
    mainLayout->addWidget(policyBox);
    mainLayout->addStretch(1);

    connect(m_cbEnable, &QCheckBox::toggled, this, &LC_RemoteApiOptionsWidget::onEnableToggled);
    connect(m_pbShowToken, &QPushButton::toggled, this, &LC_RemoteApiOptionsWidget::onShowTokenToggled);
    connect(m_pbCopyToken, &QPushButton::clicked, this, &LC_RemoteApiOptionsWidget::onCopyToken);
    connect(m_pbRegenerate, &QPushButton::clicked, this, &LC_RemoteApiOptionsWidget::onRegenerateToken);
    connect(m_pbCopyConfig, &QPushButton::clicked, this, &LC_RemoteApiOptionsWidget::onCopyMcpConfig);
    updateEnabledState();
}

LC_RemoteApiConfig LC_RemoteApiOptionsWidget::currentConfig() const {
    LC_RemoteApiConfig config;
    config.enabled = m_cbEnable->isChecked();
    config.port = m_sbPort->value();
    config.token = m_token;
    config.notify = m_cbNotify->isChecked();
    config.allowCommands = m_cbAllowCommands->isChecked();
    for (const QString& root : m_leAllowedRoots->text().split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        if (!root.trimmed().isEmpty()) {
            config.allowedRoots << root.trimmed();
        }
    }
    config.allowNetworkPaths = m_cbAllowNetworkPaths->isChecked();
    return config;
}

void LC_RemoteApiOptionsWidget::save() const {
    LC_RemoteApiConfig config = currentConfig();
    if (!LC_RemoteApiConfig::isValidPort(config.port)) {
        config.port = LC_RemoteApiConfig::load().port; // persisted only when valid
    }
    config.save();
}

QString LC_RemoteApiOptionsWidget::mcpConfigSnippet() const {
    // No token and no URL: the server reads both from the discovery file on every
    // call, so the entry keeps working after Regenerate or a port change, and can
    // be committed with a project's .mcp.json.
    QJsonObject server;
    const QString sourceDir = LC_RemoteApiController::mcpSourceDir();
    if (!sourceDir.isEmpty()) {
        // a development build: the server of this very checkout
        server.insert(QStringLiteral("command"), QStringLiteral("uv"));
        server.insert(QStringLiteral("args"),
                      QJsonArray{QStringLiteral("run"), QStringLiteral("--frozen"), QStringLiteral("--directory"),
                                 QDir::toNativeSeparators(sourceDir), QStringLiteral("librecad-mcp")});
    }
    else {
        // the server of this build's release tag (v<version>-mcp.<build>)
        const QString source = QStringLiteral("git+https://github.com/ShiftSad/LibreCAD-mcp@%1#subdirectory=mcp")
                                   .arg(LC_RemoteApiController::releaseTag());
        server.insert(QStringLiteral("command"), QStringLiteral("uvx"));
        server.insert(QStringLiteral("args"), QJsonArray{QStringLiteral("--from"), source, QStringLiteral("librecad-mcp")});
    }
    server.insert(QStringLiteral("env"), QJsonObject());
    const QJsonObject root{{QStringLiteral("mcpServers"), QJsonObject{{QStringLiteral("librecad"), server}}}};
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void LC_RemoteApiOptionsWidget::onEnableToggled(const bool enabled) {
    if (enabled && m_token.isEmpty()) {
        m_token = LC_RemoteApiConfig::generateToken();
        m_leToken->setText(m_token);
    }
    updateEnabledState();
}

void LC_RemoteApiOptionsWidget::onShowTokenToggled(const bool show) {
    m_leToken->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
    m_pbShowToken->setText(show ? tr("Hide") : tr("Show"));
}

void LC_RemoteApiOptionsWidget::onCopyToken() const {
    if (QClipboard* clipboard = QApplication::clipboard()) {
        clipboard->setText(m_token);
    }
}

void LC_RemoteApiOptionsWidget::onRegenerateToken() {
    const QString token = LC_RemoteApiConfig::generateToken();
    if (token.isEmpty()) {
        return;
    }
    m_token = token;
    m_leToken->setText(m_token);
    m_lTokenNote->setText(tr("New token: it takes effect when you press OK. MCP servers that read the discovery "
                             "file pick it up; a client given the old token (LIBRECAD_API_TOKEN) must be updated."));
    m_lTokenNote->setVisible(true);
    updateEnabledState();
}

void LC_RemoteApiOptionsWidget::onCopyMcpConfig() const {
    if (QClipboard* clipboard = QApplication::clipboard()) {
        clipboard->setText(mcpConfigSnippet());
    }
}

void LC_RemoteApiOptionsWidget::updateEnabledState() {
    const bool hasToken = !m_token.isEmpty();
    m_pbCopyToken->setEnabled(hasToken);
    m_pbShowToken->setEnabled(hasToken);
    m_pbCopyConfig->setEnabled(hasToken);
}
