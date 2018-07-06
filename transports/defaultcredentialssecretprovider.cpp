/*
 * Copyright (C) 2018 Ispirata Srl
 *
 * This file is part of Astarte.
 * Astarte is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Astarte is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Astarte.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "defaultcredentialssecretprovider.h"

#include <QtCore/QLoggingCategory>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSettings>
#include <QtCore/QTimer>

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <hyperdriveutils.h>

#define RETRY_INTERVAL 15000

Q_LOGGING_CATEGORY(defaultCredSecretProviderDC, "astarte.defaultcredentialssecretprovider", DEBUG_MESSAGES_DEFAULT_LEVEL)

namespace Astarte {

DefaultCredentialsSecretProvider::DefaultCredentialsSecretProvider(HTTPEndpoint *parent)
    : CredentialsSecretProvider(parent)
{
}

DefaultCredentialsSecretProvider::~DefaultCredentialsSecretProvider()
{
}

void DefaultCredentialsSecretProvider::ensureCredentialsSecret()
{
    QSettings settings(QStringLiteral("%1/endpoint_crypto.conf").arg(m_endpointConfigurationPath), QSettings::IniFormat);
    if (settings.contains(QStringLiteral("credentialsSecret"))) {
        m_credentialsSecret = settings.value(QStringLiteral("credentialsSecret")).toString().toLatin1();
        emit credentialsSecretReady(m_credentialsSecret);
    } else {
        sendRegistrationRequest();
    }
}

QByteArray DefaultCredentialsSecretProvider::credentialsSecret() const
{
    return m_credentialsSecret;
}

void DefaultCredentialsSecretProvider::setAgentKey(const QByteArray &agentKey)
{
    m_agentKey = agentKey;
}

void DefaultCredentialsSecretProvider::setEndpointConfigurationPath(const QString &endpointConfigurationPath)
{
    m_endpointConfigurationPath = endpointConfigurationPath;
}

void DefaultCredentialsSecretProvider::setEndpointUrl(const QUrl &endpointUrl)
{
    m_endpointUrl = endpointUrl;
}

void DefaultCredentialsSecretProvider::setHardwareId(const QByteArray &hardwareId)
{
    m_hardwareId = hardwareId;
}

void DefaultCredentialsSecretProvider::setNAM(QNetworkAccessManager *nam)
{
    m_nam = nam;
}

void DefaultCredentialsSecretProvider::setSslConfiguration(const QSslConfiguration &configuration)
{
    m_sslConfiguration = configuration;
}

void DefaultCredentialsSecretProvider::sendRegistrationRequest()
{
    if (!m_nam) {
        qCWarning(defaultCredSecretProviderDC) << "NAM is null, can't send registration request";
        retryRegistrationLater();
        return;
    }

    qWarning() << "Registering the device";
    QJsonObject data;
    data.insert(QStringLiteral("hw_id"), QLatin1String(m_hardwareId));

    QJsonObject payload;
    payload.insert(QStringLiteral("data"), data);

    QByteArray serializedPayload = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    QNetworkRequest req;
    req.setSslConfiguration(m_sslConfiguration);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Authorization", "Bearer " + m_agentKey);
    QUrl reqUrl = m_endpointUrl;
    reqUrl.setPath(m_endpointUrl.path() + QStringLiteral("/agent/devices"));
    req.setUrl(reqUrl);

    qCDebug(defaultCredSecretProviderDC) << "Sending registration request.";
    qCDebug(defaultCredSecretProviderDC) << "URL:" << req.url() << "Payload:" << serializedPayload;

    QNetworkReply *r = m_nam->post(req, serializedPayload);
    connect(r, &QNetworkReply::finished, this, [this, r] {
        if (r->error() != QNetworkReply::NoError) {
            qCWarning(defaultCredSecretProviderDC) << "Registration error! Error: " << r->error();
            retryRegistrationLater();
            r->deleteLater();
            return;
        }

        QByteArray replyBytes = r->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(replyBytes);
        r->deleteLater();
        qCDebug(defaultCredSecretProviderDC) << "Device registered";
        if (!doc.isObject()) {
            qCWarning(defaultCredSecretProviderDC) << "Registration result is not a valid JSON document: " << replyBytes;
            retryRegistrationLater();
            return;
        }

        qCDebug(defaultCredSecretProviderDC) << "Payload is " << doc.toJson().constData();

        QString credentialsSecretString = doc.object().value(QStringLiteral("data")).toObject()
                                                      .value(QStringLiteral("credentials_secret")).toString();
        if (credentialsSecretString.isEmpty()) {
            qCWarning(defaultCredSecretProviderDC) << "Missing credentials_secret in the response: " << doc;
            retryRegistrationLater();
            return;
        }

        m_credentialsSecret = credentialsSecretString.toLatin1();

        // Ok, we need to write the files now.
        {
            QSettings settings(QStringLiteral("%1/endpoint_crypto.conf").arg(m_endpointConfigurationPath),
                               QSettings::IniFormat);
            settings.setValue(QStringLiteral("credentialsSecret"), credentialsSecretString);
        }

        emit credentialsSecretReady(m_credentialsSecret);
    });
}

void DefaultCredentialsSecretProvider::retryRegistrationLater()
{
    int retryInterval = Hyperdrive::Utils::randomizedInterval(RETRY_INTERVAL, 1.0);
    qCWarning(defaultCredSecretProviderDC) << "Retrying in" << (retryInterval / 1000) << "seconds";
    QTimer::singleShot(retryInterval, this, &DefaultCredentialsSecretProvider::sendRegistrationRequest);
}

}