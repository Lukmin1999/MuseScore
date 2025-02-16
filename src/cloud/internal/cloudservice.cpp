/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore BVBA and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "cloudservice.h"
#include "config.h"

#include <QOAuth2AuthorizationCodeFlow>
#include <QOAuthHttpServerReplyHandler>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>
#include <QBuffer>
#include <QHttpMultiPart>
#include <QRandomGenerator>

#include "network/networkerrors.h"
#include "multiinstances/resourcelockguard.h"
#include "global/async/async.h"

#include "clouderrors.h"

#include "log.h"

using namespace mu::cloud;
using namespace mu::network;
using namespace mu::framework;

static const QString ACCESS_TOKEN_KEY("access_token");
static const QString REFRESH_TOKEN_KEY("refresh_token");
static const QString DEVICE_ID_KEY("device_id");
static const QString SCORE_ID_KEY("score_id");
static const QString EDITOR_SOURCE_KEY("editor_source");
static const QString EDITOR_SOURCE_VALUE(QString("Musescore Editor %1").arg(VERSION));
static const QString PLATFORM_KEY("platform");

static const std::string CLOUD_ACCESS_TOKEN_RESOURCE_NAME("CLOUD_ACCESS_TOKEN");

constexpr int USER_UNAUTHORIZED_ERR_CODE = 401;
constexpr int INVALID_SCORE_ID = 0;

static int scoreIdFromSourceUrl(const QUrl& sourceUrl)
{
    QStringList parts = sourceUrl.toString().split("/");
    if (parts.isEmpty()) {
        return INVALID_SCORE_ID;
    }

    return parts.last().toInt();
}

CloudService::CloudService(QObject* parent)
    : QObject(parent)
{
    m_userAuthorized.val = false;
}

void CloudService::init()
{
    TRACEFUNC;

    m_oauth2 = new QOAuth2AuthorizationCodeFlow(this);
    m_replyHandler = new QOAuthHttpServerReplyHandler(this);

    m_oauth2->setAuthorizationUrl(configuration()->authorizationUrl());
    m_oauth2->setAccessTokenUrl(configuration()->accessTokenUrl());
    m_oauth2->setModifyParametersFunction([](QAbstractOAuth::Stage, QVariantMap* parameters) {
        parameters->insert(EDITOR_SOURCE_KEY, EDITOR_SOURCE_VALUE);
        parameters->insert(PLATFORM_KEY, QString("%1 %2 %3")
                           .arg(QSysInfo::productType())
                           .arg(QSysInfo::productVersion())
                           .arg(QSysInfo::currentCpuArchitecture())
                           );
    });
    m_oauth2->setReplyHandler(m_replyHandler);

    connect(m_oauth2, &QOAuth2AuthorizationCodeFlow::authorizeWithBrowser, this, &CloudService::openUrl);
    connect(m_oauth2, &QOAuth2AuthorizationCodeFlow::granted, this, &CloudService::onUserAuthorized);

    connect(m_oauth2, &QOAuth2AuthorizationCodeFlow::error, [](const QString& error, const QString& errorDescription, const QUrl& uri) {
        LOGE() << "Error during authorization: " << error << "\n Description: " << errorDescription << "\n URI: " << uri.toString();
    });

    multiInstancesProvider()->resourceChanged().onReceive(this, [this](const std::string& resourceName) {
        if (resourceName == CLOUD_ACCESS_TOKEN_RESOURCE_NAME) {
            readTokens();
        }
    });

    if (readTokens()) {
        executeRequest([this]() { return downloadUserInfo(); });
    }
}

bool CloudService::readTokens()
{
    TRACEFUNC;

    mi::ReadResourceLockGuard resource_guard(multiInstancesProvider(), CLOUD_ACCESS_TOKEN_RESOURCE_NAME);

    io::path_t tokensPath = configuration()->tokensFilePath();
    if (!fileSystem()->exists(tokensPath)) {
        return false;
    }

    RetVal<ByteArray> tokensData = fileSystem()->readFile(tokensPath);
    if (!tokensData.ret) {
        LOGE() << tokensData.ret.toString();
        return false;
    }

    QJsonDocument tokensDoc = QJsonDocument::fromJson(tokensData.val.toQByteArrayNoCopy());
    QJsonObject saveObject = tokensDoc.object();

    m_accessToken = saveObject[ACCESS_TOKEN_KEY].toString();
    m_refreshToken = saveObject[REFRESH_TOKEN_KEY].toString();

    return true;
}

bool CloudService::saveTokens()
{
    TRACEFUNC;

    mi::WriteResourceLockGuard resource_guard(multiInstancesProvider(), CLOUD_ACCESS_TOKEN_RESOURCE_NAME);

    QJsonObject tokensObject;
    tokensObject[ACCESS_TOKEN_KEY] = m_accessToken;
    tokensObject[REFRESH_TOKEN_KEY] = m_refreshToken;
    QJsonDocument tokensDoc(tokensObject);

    QByteArray json = tokensDoc.toJson();
    Ret ret = fileSystem()->writeFile(configuration()->tokensFilePath(), ByteArray::fromQByteArrayNoCopy(json));
    if (!ret) {
        LOGE() << ret.toString();
    }

    return ret;
}

bool CloudService::updateTokens()
{
    TRACEFUNC;

    QUrlQuery query;
    query.addQueryItem(REFRESH_TOKEN_KEY, m_refreshToken);
    query.addQueryItem(DEVICE_ID_KEY, configuration()->clientId());

    QUrl refreshApiUrl = configuration()->refreshApiUrl();
    refreshApiUrl.setQuery(query);

    QBuffer receivedData;
    INetworkManagerPtr manager = networkManagerCreator()->makeNetworkManager();
    Ret ret = manager->post(refreshApiUrl, nullptr, &receivedData, headers());

    if (!ret) {
        LOGE() << ret.toString();
        clearTokens();
        return false;
    }

    QJsonDocument document = QJsonDocument::fromJson(receivedData.data());
    QJsonObject tokens = document.object();

    m_accessToken = tokens.value(ACCESS_TOKEN_KEY).toString();
    m_refreshToken = tokens.value(REFRESH_TOKEN_KEY).toString();

    return saveTokens();
}

void CloudService::clearTokens()
{
    m_accessToken.clear();
    m_refreshToken.clear();
    setAccountInfo(AccountInfo());
}

void CloudService::onUserAuthorized()
{
    TRACEFUNC;

    m_accessToken = m_oauth2->token();
    m_refreshToken = m_oauth2->refreshToken();

    saveTokens();

    Ret ret = downloadUserInfo();
    if (!ret) {
        LOGE() << ret.toString();
        return;
    }

    if (m_onUserAuthorizedCallback) {
        m_onUserAuthorizedCallback();
        m_onUserAuthorizedCallback = OnUserAuthorizedCallback();
    }
}

void CloudService::authorize(const OnUserAuthorizedCallback& onUserAuthorizedCallback)
{
    if (m_userAuthorized.val) {
        return;
    }

    m_onUserAuthorizedCallback = onUserAuthorizedCallback;
    m_oauth2->setAuthorizationUrl(configuration()->authorizationUrl());
    m_oauth2->grant();
}

mu::RetVal<QUrl> CloudService::prepareUrlForRequest(QUrl apiUrl, const QVariantMap& params) const
{
    if (m_accessToken.isEmpty()) {
        return make_ret(cloud::Err::AccessTokenIsEmpty);
    }

    QUrlQuery query;
    query.addQueryItem(ACCESS_TOKEN_KEY, m_accessToken);

    for (auto it = params.cbegin(); it != params.cend(); ++it) {
        query.addQueryItem(it.key(), it.value().toString());
    }

    apiUrl.setQuery(query);

    return RetVal<QUrl>::make_ok(apiUrl);
}

RequestHeaders CloudService::headers() const
{
    return configuration()->headers();
}

mu::Ret CloudService::downloadUserInfo()
{
    TRACEFUNC;

    RetVal<QUrl> userInfoUrl = prepareUrlForRequest(configuration()->userInfoApiUrl());
    if (!userInfoUrl.ret) {
        return userInfoUrl.ret;
    }

    QBuffer receivedData;
    INetworkManagerPtr manager = networkManagerCreator()->makeNetworkManager();
    Ret ret = manager->get(userInfoUrl.val, &receivedData, headers());

    if (ret.code() == USER_UNAUTHORIZED_ERR_CODE) {
        return make_ret(cloud::Err::UserIsNotAuthorized);
    }

    if (!ret) {
        return ret;
    }

    QJsonDocument document = QJsonDocument::fromJson(receivedData.data());
    QJsonObject user = document.object();

    AccountInfo info;
    info.id = user.value("id").toInt();
    info.userName = user.value("name").toString();
    QString profileUrl = user.value("profile_url").toString();
    info.profileUrl = QUrl(profileUrl);
    info.avatarUrl = QUrl(user.value("avatar_url").toString());
    info.sheetmusicUrl = QUrl(profileUrl + "/sheetmusic");

    setAccountInfo(info);

    return make_ok();
}

mu::RetVal<ScoreInfo> CloudService::downloadScoreInfo(int scoreId)
{
    TRACEFUNC;

    RetVal<ScoreInfo> result = RetVal<ScoreInfo>::make_ok(ScoreInfo());

    QVariantMap params;
    params[SCORE_ID_KEY] = scoreId;

    RetVal<QUrl> scoreInfoUrl = prepareUrlForRequest(configuration()->scoreInfoApiUrl(), params);
    if (!scoreInfoUrl.ret) {
        result.ret = scoreInfoUrl.ret;
        return result;
    }

    QBuffer receivedData;
    INetworkManagerPtr manager = networkManagerCreator()->makeNetworkManager();
    Ret ret = manager->get(scoreInfoUrl.val, &receivedData, headers());

    if (!ret) {
        result.ret = ret;
        return result;
    }

    QJsonDocument document = QJsonDocument::fromJson(receivedData.data());
    QJsonObject scoreInfo = document.object();

    result.val.id = scoreInfo.value("id").toInt();
    result.val.title = scoreInfo.value("title").toString();
    result.val.description = scoreInfo.value("description").toString();
    result.val.license = scoreInfo.value("license").toString();
    result.val.tags = scoreInfo.value("tags").toString().split(',');
    result.val.isPrivate = scoreInfo.value("sharing").toString() == "private";
    result.val.url = scoreInfo.value("custom_url").toString();

    QJsonObject owner = scoreInfo.value("user").toObject();

    result.val.owner.id = owner.value("uid").toInt();
    result.val.owner.userName = owner.value("username").toString();
    result.val.owner.profileUrl = owner.value("custom_url").toString();

    return result;
}

void CloudService::signIn()
{
    authorize();
}

void CloudService::signUp()
{
    if (m_userAuthorized.val) {
        return;
    }
    m_oauth2->setAuthorizationUrl(configuration()->signUpUrl());
    m_oauth2->grant();
}

void CloudService::signOut()
{
    if (!m_userAuthorized.val) {
        return;
    }

    TRACEFUNC;

    QVariantMap params;
    params[REFRESH_TOKEN_KEY] = m_refreshToken;

    RetVal<QUrl> signOutUrl = prepareUrlForRequest(configuration()->logoutApiUrl(), params);
    if (!signOutUrl.ret) {
        LOGE() << signOutUrl.ret.toString();
        return;
    }

    QBuffer receivedData;
    INetworkManagerPtr manager = networkManagerCreator()->makeNetworkManager();
    Ret ret = manager->get(signOutUrl.val, &receivedData, headers());
    if (!ret) {
        LOGE() << ret.toString();
    }

    mi::WriteResourceLockGuard resource_guard(multiInstancesProvider(), CLOUD_ACCESS_TOKEN_RESOURCE_NAME);

    ret = fileSystem()->remove(configuration()->tokensFilePath());
    if (!ret) {
        LOGE() << ret.toString();
    }

    clearTokens();
}

mu::Ret CloudService::requireAuthorization(const std::string& text)
{
    UriQuery query("musescore://cloud/requireauthorization");
    query.addParam("text", Val(text));
    return interactive()->open(query).ret;
}

mu::ValCh<bool> CloudService::userAuthorized() const
{
    return m_userAuthorized;
}

mu::ValCh<AccountInfo> CloudService::accountInfo() const
{
    return m_accountInfo;
}

void CloudService::setAccountInfo(const AccountInfo& info)
{
    if (m_accountInfo.val == info) {
        return;
    }

    m_accountInfo.set(info);
    m_userAuthorized.set(info.isValid());
}

ProgressPtr CloudService::uploadScore(QIODevice& scoreSourceDevice, const QString& title, const QUrl& sourceUrl)
{
    ProgressPtr progress = std::make_shared<Progress>();

    auto uploadCallback = [this, progress, &scoreSourceDevice, title, sourceUrl]() {
        progress->started.notify();

        INetworkManagerPtr manager = networkManagerCreator()->makeNetworkManager();
        manager->progress().progressChanged.onReceive(this, [progress](int64_t current, int64_t total, const std::string& message) {
            progress->progressChanged.send(current, total, message);
        });

        RetVal<QUrl> newSourceUrl = doUploadScore(manager, scoreSourceDevice, title, sourceUrl);

        ProgressResult result;
        result.ret = newSourceUrl.ret;
        result.val = Val(newSourceUrl.val.toString());
        progress->finished.send(result);

        return result.ret;
    };

    async::Async::call(this, [this, uploadCallback]() {
        if (!m_userAuthorized.val) {
            authorize(uploadCallback);
            return;
        }

        executeRequest(uploadCallback);
    });

    return progress;
}

mu::RetVal<QUrl> CloudService::doUploadScore(INetworkManagerPtr uploadManager, QIODevice& scoreSourceDevice, const QString& title,
                                             const QUrl& sourceUrl)
{
    TRACEFUNC;

    RetVal<QUrl> result = RetVal<QUrl>::make_ok(QUrl());

    RetVal<QUrl> uploadUrl = prepareUrlForRequest(configuration()->uploadingApiUrl());
    if (!uploadUrl.ret) {
        result.ret = uploadUrl.ret;
        return result;
    }

    int scoreId = scoreIdFromSourceUrl(sourceUrl);
    bool isScoreAlreadyUploaded = scoreId != INVALID_SCORE_ID;

    if (isScoreAlreadyUploaded) {
        RetVal<ScoreInfo> scoreInfo = downloadScoreInfo(scoreId);
        if (!scoreInfo.ret) {
            LOGW() << scoreInfo.ret.toString();

            if (scoreInfo.ret.code() == static_cast<int>(network::Err::ResourceNotFound)) {
                isScoreAlreadyUploaded = false;
            } else {
                result.ret = scoreInfo.ret;
                return result;
            }
        }

        if (scoreInfo.val.owner.id != m_accountInfo.val.id) {
            isScoreAlreadyUploaded = false;
        }
    }

    QHttpMultiPart multiPart(QHttpMultiPart::FormDataType);

    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("application/octet-stream"));
    int fileNameNumber = QRandomGenerator::global()->generate() % 100000;
    QString contentDisposition = QString("form-data; name=\"score_data\"; filename=\"temp_%1.mscz\"").arg(fileNameNumber);
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant(contentDisposition));

    filePart.setBodyDevice(&scoreSourceDevice);
    multiPart.append(filePart);

    if (isScoreAlreadyUploaded) {
        QHttpPart scoreIdPart;
        scoreIdPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"score_id\""));
        scoreIdPart.setBody(QString::number(scoreId).toLatin1());
        multiPart.append(scoreIdPart);
    }

    QHttpPart titlePart;
    titlePart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"title\""));
    titlePart.setBody(title.toUtf8());
    multiPart.append(titlePart);

    QHttpPart licensePart;
    licensePart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"license\""));
    licensePart.setBody(configuration()->uploadingLicense());
    multiPart.append(licensePart);

    Ret ret(true);
    QBuffer receivedData;
    OutgoingDevice device(&multiPart);

    if (isScoreAlreadyUploaded) { // score exists, update
        ret = uploadManager->put(uploadUrl.val, &device, &receivedData, headers());
    } else { // score doesn't exist, post a new score
        ret = uploadManager->post(uploadUrl.val, &device, &receivedData, headers());
    }

    if (ret.code() == USER_UNAUTHORIZED_ERR_CODE) {
        return make_ret(cloud::Err::UserIsNotAuthorized);
    }

    if (!ret) {
        result.ret = ret;
        return result;
    }

    QJsonObject scoreInfo = QJsonDocument::fromJson(receivedData.data()).object();
    QUrl newSourceUrl = QUrl(scoreInfo.value("permalink").toString());
    QUrl editUrl = QUrl(scoreInfo.value("edit_url").toString());

    if (!newSourceUrl.isValid()) {
        result.ret = make_ret(cloud::Err::CouldNotReceiveSourceUrl);
        return result;
    }

    result.val = newSourceUrl;
    openUrl(editUrl);

    return result;
}

void CloudService::executeRequest(const RequestCallback& requestCallback)
{
    Ret ret = requestCallback();

    if (ret.code() == static_cast<int>(cloud::Err::UserIsNotAuthorized)) {
        if (updateTokens()) {
            ret = requestCallback();
        }
    }

    if (!ret) {
        LOGE() << ret.toString();
    }
}

void CloudService::openUrl(const QUrl& url)
{
    Ret ret = interactive()->openUrl(url);
    if (!ret) {
        LOGE() << ret.toString();
    }
}
