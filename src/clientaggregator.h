/******************************************************************************\
 * Copyright (c) 2025
 *
 * Author(s):
 *
 ******************************************************************************
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 *
\******************************************************************************/

#pragma once

#include <QTimer>
#include <QLocale>
#include <QtConcurrent>
#include <QRegularExpression>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QDateTime>
#include <QMutex>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include "global.h"
#include "protocol.h"
#include "util.h"

class CConnectionlessSocket;

/* Definitions ****************************************************************/
#define AGGREGATOR_UPDATE_INTERVAL_MS           15000  // Interval to re-query directories and servers
#define AGGREGATOR_UPDATE_CLIENTS_INTERVAL_MS   30000 // Interval to force re-fetching client list
#define AGGREGATOR_SERVER_TIMEOUT_MS            30000 // Time after which a non-responding server is considered offline
#define AGGREGATOR_DIRECTORY_TIMEOUT_MS         60000 // Time after which a non-responding directory is considered offline

/* Data Structures ************************************************************/
struct Client
{
    int       chanid{};
    int       countryid{};
    int       instrumentid{};
    int       skillid{};
    QString   country;
    QString   country_code;
    QString   instrument;
    QString   skill;
    QString   name;
    QString   city;
    QDateTime lastModified;

    QJsonObject toJson(bool detailed) const;
};

struct Server
{
    quint32       numip{};
    quint16       port{};
    int           countryid{};
    int           maxclients{};
    int           perm{};
    QString       country;
    QString       country_code;
    QString       name;
    QString       ipaddrs;
    QString       city;
    QString       ip;
    int           ping{};
    QString       os;
    QString       version;
    QString       versionsort;
    int           nclients{}; // Added to store the actual number of clients from ping
    QList<Client> clients;
    QDateTime     lastModified;
    QDateTime     lastPingResponse;
    int           lastNumClients{};        // Store the last reported number of clients
    QDateTime     lastClientListFetch; // Timestamp of the last client list fetch
    QDateTime     lastClientListReceived; // Timestamp when the client list was last received

    QJsonObject toJson(bool detailed) const;
};

struct Directory
{
    QString       address;
    QList<Server> servers;
    QDateTime     lastModified;
    QDateTime     lastSuccessfulUpdate;
};

struct QueryParameters
{
    bool hideEmpty = false; // Filter servers with 0 clients

    QString getKey() const
    {
        QString key;
        if ( hideEmpty )
            key += "hideEmpty=1";
        return key.isEmpty() ? "default" : key;
    }

    bool isValid() const
    {
        // All parameters in this struct are valid - add checks here if needed
        return true;
    }
};

struct JsonCacheData
{
    QByteArray jsonCache;
    QDateTime  cacheTimestamp;
    QByteArray etag;
};

class CClientAggregator;

class CJamStatusServer : public QObject
{
    Q_OBJECT
public:
    CJamStatusServer(CClientAggregator* aggregator, quint16 port, QObject* parent = nullptr);
    ~CJamStatusServer();

public slots:
    void start();
    void stop();

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();

private:
    CClientAggregator* m_aggregator;
    QTcpServer* m_server;
    quint16 m_port;
    QList<QTcpSocket*> m_clients;
};

/* Classes ********************************************************************/
class CClientAggregator : public QObject
{
    Q_OBJECT

public:
    explicit CClientAggregator ( const QStringList& directoryAddresses, bool detailedJson = false, QObject* parent = nullptr );
    ~CClientAggregator();

    // CRUD for Directories
    void       addDirectory ( const QString& address );
    void       removeDirectory ( const QString& address );
    Directory* getDirectory ( const QString& address );

    // CRUD for Servers
    void    updateOrAddServer ( const QString& directoryAddress, const Server& server );
    void    updateOrAddServerLocked ( const QString& directoryAddress, const Server& server ); // Internal: called with mutex locked
    bool    removeServer ( const QString& directoryAddress, const QString& serverIp, quint16 port );
    Server* getServer ( const QString& directoryAddress, const QString& serverIp, quint16 port );
    Server* getServerFromAnyDirectory(const QString& serverIp, quint16 port);

    // CRUD for Clients
    void    updateOrAddClient ( const QString& directoryAddress, const QString& serverIp, quint16 port, const Client& client );
    bool    removeClient ( const QString& directoryAddress, const QString& serverIp, quint16 port, int chanId );
    Client* getClient ( const QString& directoryAddress, const QString& serverIp, quint16 port, int chanId );

    // Serialization
    QByteArray getJson(const QueryParameters& params = QueryParameters()) const;
    QByteArray getJsonLocked(const QueryParameters& params) const;  // Internal: called with both mutexes already locked
    QByteArray getCompressedJson(QByteArray& etag, const QueryParameters& params = QueryParameters());


    void start();
    void stop();
    void startHttpServer(quint16 port);


private slots:
    void onUpdateTimer();
    void onServerListReceived ( const CHostAddress& inetAddr, const CVector<CServerInfo>& vecServerInfo );
    void onPingTimeReceived ( const CHostAddress& inetAddr, int iPingTime, int iNumClients );
    void onVersionReceived ( const CHostAddress& inetAddr, COSUtil::EOpSystemType eOSType, const QString& version );
    void onConnClientsListMesReceived ( const CHostAddress& inetAddr, const CVector<CChannelInfo>& vecChanInfo );

private:
    void queryDirectories();
    void queryServers();
    void cleanupExpiredServers();

    QMap<QString, Directory> m_directories;
    CProtocol                m_protocol;
    CConnectionlessSocket*   m_pSocket;
    QTimer                   m_updateTimer;
    CJamStatusServer*        m_httpServer{};
    QThread*                 m_httpServerThread{};
    bool                     m_detailedJson;

    mutable QRecursiveMutex m_dataMutex;      // Protects m_directories
    mutable QRecursiveMutex m_cacheMutex;
    mutable QMap<QString, JsonCacheData> m_jsonCaches;             // Cache per parameter set
    
    static QString normalizedIpString(const QHostAddress& qhAddr);
};
