#include "clientaggregator.h"
#include "connectionlesssocket.h"
#include "util.h"
#include <QDebug>
#include <QCryptographicHash>
#include <algorithm> // Required for std::sort

namespace
{
    QString skillLevelToString ( ESkillLevel level )
    {
        switch ( level )
        {
        case SL_BEGINNER:
            return QStringLiteral ( "Beginner" );
        case SL_INTERMEDIATE:
            return QStringLiteral ( "Intermediate" );
        case SL_PROFESSIONAL:
            return QStringLiteral ( "Professional" );
        case SL_NOT_SET:
        default:
            return QStringLiteral ( "Not Set" );
        }
    }

    QString countryToCode(QLocale::Country country)
    {
        if (country == QLocale::AnyCountry)
        {
            return QString();
        }
        const QList<QLocale> locales = QLocale::matchingLocales(QLocale::AnyLanguage, QLocale::AnyScript, country);
        if (!locales.isEmpty())
        {
            const QStringList parts = locales.first().name().split('_');
            if (parts.size() > 1)
            {
                return parts.last();
            }
        }
        return QString();
    }
}

QJsonObject Client::toJson(bool detailed) const
{
    QJsonObject obj;
    obj["chanid"]       = chanid;
    obj["country"]      = country;
    if (detailed) {
        obj["countryid"]    = countryid;
        obj["skillid"]      = skillid;
        obj["skill"]        = skill;
    }
    obj["instrumentid"] = instrumentid;
    obj["country_code"] = country_code;
    obj["instrument"]   = instrument;
    obj["name"]         = name;
    obj["city"]         = city;
    return obj;
}

QJsonObject Server::toJson(bool detailed) const
{
    QJsonObject obj;
    obj["port"]        = static_cast<qint64> ( port );
    obj["maxclients"]  = maxclients;
    obj["name"]        = name;
    obj["country_code"] = country_code;
    obj["country"]     = country;
    obj["city"]        = city;
    obj["ip"]          = ip;
    obj["nclients"]    = nclients;
    obj["lastClientListReceived"] = lastClientListReceived.isValid() ? lastClientListReceived.toString ( Qt::ISODate ) : QString();
    if (detailed) {
        obj["numip"]       = static_cast<qint64> ( numip );
        obj["countryid"]   = countryid;
        obj["perm"]        = perm;
        obj["ipaddrs"]     = ipaddrs;
        obj["ping"]        = ping;
        obj["os"]          = os;
        obj["version"]     = version;
        obj["versionsort"] = versionsort;
        
    }

    QJsonArray clientsArray;
    for ( const auto& client : clients )
    {
        clientsArray.append ( client.toJson(detailed) );
    }
    if ( !clientsArray.isEmpty() )
    {
        obj["clients"] = clientsArray;
    }

    return obj;
}

CClientAggregator::CClientAggregator ( const QStringList& directoryAddresses, bool detailedJson, QObject* parent )
    : QObject ( parent ), m_protocol(), m_pSocket( new CConnectionlessSocket( this ) ), m_httpServer ( nullptr ), m_httpServerThread ( nullptr ), m_detailedJson(detailedJson)
{
    for ( const auto& address : directoryAddresses )
    {
        addDirectory ( address );
    }
    qInfo() << "ClientAggregator: Initialized with directories:" << directoryAddresses;

    connect ( &m_updateTimer, &QTimer::timeout, this, &CClientAggregator::onUpdateTimer );

    // Protocol -> this
    connect ( &m_protocol, &CProtocol::CLServerListReceived, this, &CClientAggregator::onServerListReceived );
    connect ( &m_protocol, &CProtocol::CLPingWithNumClientsReceived, this, &CClientAggregator::onPingTimeReceived );
    connect ( &m_protocol, &CProtocol::CLVersionAndOSReceived, this, &CClientAggregator::onVersionReceived );
    connect ( &m_protocol, &CProtocol::CLConnClientsListMesReceived, this, &CClientAggregator::onConnClientsListMesReceived );

    // Protocol -> Socket
    connect ( &m_protocol, &CProtocol::CLMessReadyForSending, m_pSocket, &CConnectionlessSocket::OnCLMessReadyForSending );

    // Socket -> Protocol
    connect ( m_pSocket, &CConnectionlessSocket::CLMessageReceived, &m_protocol, &CProtocol::ParseConnectionLessMessageBody );
}

CClientAggregator::~CClientAggregator()
{
    if ( m_httpServerThread )
    {
        m_httpServerThread->quit();
        m_httpServerThread->wait();
    }
    stop();
}

void CClientAggregator::start()
{
    // Start protocol with random port
    if ( !m_pSocket->Start ( 0, true ) // use port 0 for random, enable IPv6
    )
    {
        qCritical() << "ClientAggregator: Socket could not start.";
        return;
    }
    qInfo() << "ClientAggregator: Socket started on port" << m_pSocket->GetPort();

    if ( !m_updateTimer.isActive() )
    {
        qInfo() << "ClientAggregator: Starting update timer.";
        onUpdateTimer(); // immediate first update
        m_updateTimer.start ( AGGREGATOR_UPDATE_INTERVAL_MS );
    }
}

void CClientAggregator::stop()
{
    if ( m_updateTimer.isActive() )
    {
        m_updateTimer.stop();
        m_pSocket->Stop();
        qInfo() << "ClientAggregator: Stopped.";
    }
}

void CClientAggregator::onUpdateTimer()
{
    queryDirectories();
    queryServers();
    cleanupExpiredServers();
}

void CClientAggregator::queryDirectories()
{
    QMutexLocker locker ( &m_dataMutex );
    // qDebug() << "ClientAggregator: Querying directories...";
    for ( auto it = m_directories.begin(); it != m_directories.end(); ++it )
    {
        CHostAddress haDirectoryAddress;
        if ( NetworkUtil().ParseNetworkAddress ( it.key(), haDirectoryAddress, false ) )
        {
            // qDebug() << "ClientAggregator: Sending server list request to" << it.key();
            m_protocol.CreateCLReqServerListMes ( haDirectoryAddress );
        }
        else
        {
            qWarning() << "ClientAggregator: Could not parse directory address:" << it.key();
        }
    }
}

void CClientAggregator::queryServers()
{
    QMutexLocker locker ( &m_dataMutex );
    for ( auto& directory : m_directories )
    {
        for ( auto& server : directory.servers )
        {
            CHostAddress haServerAddress;
            if ( NetworkUtil().ParseNetworkAddress ( server.ip + ":" + QString::number ( server.port ), haServerAddress, true ) )
            {
                m_protocol.CreateCLPingWithNumClientsMes ( haServerAddress, QDateTime::currentMSecsSinceEpoch(), 0 /* dummy iNumClients for request */ );
                if ( server.version.isEmpty() )
                {
                    m_protocol.CreateCLReqVersionAndOSMes ( haServerAddress );
                }
            }
        }
    }
}

void CClientAggregator::cleanupExpiredServers()
{
    QMutexLocker locker ( &m_dataMutex );
    const QDateTime now = QDateTime::currentDateTime();
    for ( auto& directory : m_directories )
    {
        directory.servers.erase ( std::remove_if ( directory.servers.begin(),
                                                   directory.servers.end(),
                                                   [&] ( const Server& server ) {
                                                       return server.lastPingResponse.isValid() &&
                                                              server.lastPingResponse.msecsTo ( now ) > AGGREGATOR_SERVER_TIMEOUT_MS;
                                                   } ),
                                  directory.servers.end() );
        directory.lastModified = now;
    }
}

void CClientAggregator::onServerListReceived ( const CHostAddress& inetAddr, const CVector<CServerInfo>& vecServerInfo )
{
    QMutexLocker locker ( &m_dataMutex );
    QString    directoryAddressStr = inetAddr.toString ( CHostAddress::SM_IP_PORT );
    // qDebug() << "ClientAggregator: Received server list from" << directoryAddressStr << "with" << vecServerInfo.Size() << "servers.";
    // Find directory by IP AND PORT, not by IP alone (multiple directories can share same IP)
    Directory* dir = nullptr;
    for ( auto it = m_directories.begin(); it != m_directories.end(); ++it )
    {
        CHostAddress haDirAddr;
        if ( NetworkUtil().ParseNetworkAddress ( it.key(), haDirAddr, false ) )
        {
            // Match both IP and port to handle multiple directories on same IP with different ports
            if ( haDirAddr.InetAddr.isEqual(inetAddr.InetAddr) && haDirAddr.iPort == inetAddr.iPort )
            {
                dir = &it.value();
                break;
            }
        }
    }

    if ( !dir )
    {
        qWarning() << "ClientAggregator: Received server list from unknown directory:" << directoryAddressStr;
        return;
    }

    const QDateTime now       = QDateTime::currentDateTime();
    dir->lastSuccessfulUpdate = now;
    dir->lastModified         = now;

    QSet<QString> receivedServers;

    for ( const auto& serverInfo : vecServerInfo )
    {
        CHostAddress serverAddr;
        if ( serverInfo.HostAddr.InetAddr.toIPv4Address() == 0 && serverInfo.HostAddr.iPort == 0 )
        {
            serverAddr = inetAddr;
        }
        else
        {
            serverAddr = serverInfo.HostAddr;
        }
        QString serverIp = normalizedIpString(serverAddr.InetAddr);
        quint16      serverPort = serverAddr.iPort;
        QString      serverId   = serverIp + ":" + QString::number ( serverPort );
        receivedServers.insert ( serverId );

        Server* existingServer = getServer ( dir->address, serverIp, serverPort );
        if ( !existingServer )
        {
            Server newServer;
            newServer.ip           = serverIp;
            newServer.port         = serverPort;
            newServer.numip        = serverAddr.InetAddr.toIPv4Address();
            newServer.ipaddrs      = serverInfo.LHostAddr.toString();
            newServer.name         = serverInfo.strName;
            newServer.city         = serverInfo.strCity;
            newServer.country      = QLocale::countryToString ( serverInfo.eCountry );
            newServer.country_code = countryToCode(serverInfo.eCountry);
            newServer.countryid    = static_cast<int> ( serverInfo.eCountry );
            newServer.maxclients   = serverInfo.iMaxNumClients;
            newServer.perm         = serverInfo.bPermanentOnline;
            newServer.lastModified = now;
            //qDebug() << "ClientAggregator: Adding new server:" << newServer.name << "@" << serverId;
            updateOrAddServerLocked ( dir->address, newServer );
        }
        else
        {
            // Update existing server info if needed
            existingServer->name         = serverInfo.strName;
            existingServer->city         = serverInfo.strCity;
            existingServer->country      = QLocale::countryToString ( serverInfo.eCountry );
            existingServer->country_code = countryToCode(serverInfo.eCountry);
            existingServer->countryid    = static_cast<int> ( serverInfo.eCountry );
            existingServer->maxclients   = serverInfo.iMaxNumClients;
            existingServer->perm         = serverInfo.bPermanentOnline;
            existingServer->lastModified = now;
        }
    }

    dir->servers.erase(
        std::remove_if(dir->servers.begin(), dir->servers.end(),
            [&](const Server& server) {
                QString serverId = server.ip + ":" + QString::number(server.port);
                return !receivedServers.contains(serverId);
            }),
        dir->servers.end());
    dir->lastModified = now;
}

void CClientAggregator::onPingTimeReceived ( const CHostAddress& inetAddr, int iPingTime, int iNumClients )
{
    QMutexLocker locker ( &m_dataMutex );
    // qDebug() << "ClientAggregator: Received ping from" << inetAddr.toString() << "- Ping:" << iPingTime << "Clients:" << iNumClients;
    // Find which directory this server belongs to
   
    if ( Server* server = getServerFromAnyDirectory(normalizedIpString ( inetAddr.InetAddr ), inetAddr.iPort ) )
    {
        const QDateTime now = QDateTime::currentDateTime();
        server->ping        = iPingTime;
        server->nclients    = iNumClients; // Update nclients directly from ping response

        bool shouldRequestClientList = false;

        if ( iNumClients > 0 )
        {
            // Condition 1: Number of clients changed
            if ( iNumClients != server->lastNumClients )
            {
                shouldRequestClientList = true;
            }
            // Condition 2: Last fetch was too long ago
            if ( !shouldRequestClientList && server->lastClientListFetch.isValid() &&
                 server->lastClientListFetch.msecsTo ( now ) > AGGREGATOR_UPDATE_CLIENTS_INTERVAL_MS )
            {
                shouldRequestClientList = true;
            }
            // Condition 3: No valid last fetch time, but clients reported
            if ( !shouldRequestClientList && !server->lastClientListFetch.isValid() )
            {
                shouldRequestClientList = true;
            }

            if ( shouldRequestClientList )
            {
                // qDebug() << "ClientAggregator: Requesting connected clients list for" << normalizedIpString(inetAddr.InetAddr) << "due to
                // condition change/timeout.";
                m_protocol.CreateCLReqConnClientsListMes ( inetAddr );
                server->lastClientListFetch = now; // Update fetch timestamp
            }
        }
        else
        { // iNumClients == 0
            // If ping reports 0 clients, clear the client list if it's not already empty
            if ( !server->clients.isEmpty() )
            {
                // qDebug() << "ClientAggregator: Clearing client list for" << normalizedIpString(inetAddr.InetAddr) << "as ping reports 0
                // clients.";
                server->clients.clear();
            }
            server->lastClientListFetch = QDateTime(); // Reset last fetch time
        }

        server->lastNumClients   = iNumClients; // Update last reported client count
        server->lastPingResponse = now;
        server->lastModified     = now;
        // directory.lastModified = now; // Update removed from here as server is now retrieved from helper function.

    }        
    
}

void CClientAggregator::onVersionReceived ( const CHostAddress& inetAddr, COSUtil::EOpSystemType eOSType, const QString& version )
{
    QMutexLocker locker ( &m_dataMutex );
    // qDebug() << "ClientAggregator: Received version from" << inetAddr.toString() << "- Version:" << version;
    if ( Server* server = getServerFromAnyDirectory( normalizedIpString(inetAddr.InetAddr), inetAddr.iPort ) )
    {
        const QDateTime now = QDateTime::currentDateTime();
        server->version     = version;
        server->os          = COSUtil::GetOperatingSystemString ( eOSType );
        server->lastModified   = now;
        // directory.lastModified = now; // Update removed from here as server is now retrieved from helper function.
    }
}

void CClientAggregator::onConnClientsListMesReceived ( const CHostAddress& inetAddr, const CVector<CChannelInfo>& vecChanInfo )
{
    QMutexLocker locker ( &m_dataMutex );
    // qDebug() << "ClientAggregator: Received client list from" << inetAddr.toString() << "with" << vecChanInfo.Size() << "clients.";

    if ( Server* server = getServerFromAnyDirectory( normalizedIpString(inetAddr.InetAddr), inetAddr.iPort ) )
    {
        const QDateTime now = QDateTime::currentDateTime();
        server->clients.clear();

        for ( const auto& chanInfo : vecChanInfo )
        {
            Client newClient;
            newClient.chanid       = chanInfo.iChanID;
            newClient.country      = QLocale::countryToString ( chanInfo.eCountry );
            newClient.country_code = countryToCode(chanInfo.eCountry);
            newClient.instrument   = CInstPictures::GetName ( chanInfo.iInstrument );
            newClient.skill        = skillLevelToString ( chanInfo.eSkillLevel );
            newClient.skillid      = static_cast<int> ( chanInfo.eSkillLevel );
            newClient.countryid    = static_cast<int> ( chanInfo.eCountry );
            newClient.instrumentid = chanInfo.iInstrument;
            newClient.name         = chanInfo.strName;
            newClient.city         = chanInfo.strCity;
            newClient.lastModified = now;
            server->clients.append ( newClient );
        }

        // Sort clients by chanid
        std::sort(server->clients.begin(), server->clients.end(), [](const Client& a, const Client& b) {
            return a.chanid < b.chanid;
        });

        server->lastClientListReceived = now; // Update the timestamp when client list was received
        server->lastModified   = now;
        // directory.lastModified = now; // Update removed from here as server is now retrieved from helper function.
    }
    else
    {
        qWarning() << "ClientAggregator: Received client list from unknown server" << inetAddr.toString();
    }
}

void CClientAggregator::addDirectory ( const QString& address )
{
    QMutexLocker locker ( &m_dataMutex );
    if ( !m_directories.contains ( address ) )
    {
        m_directories.insert ( address, { address, {}, QDateTime::currentDateTime(), QDateTime() } );
    }
}

void CClientAggregator::removeDirectory ( const QString& address )
{
    QMutexLocker locker ( &m_dataMutex );
    m_directories.remove ( address );
}

Directory* CClientAggregator::getDirectory ( const QString& address )
{
    auto it = m_directories.find ( address );
    return ( it != m_directories.end() ) ? &it.value() : nullptr;
}

void CClientAggregator::updateOrAddServer ( const QString& directoryAddress, const Server& server )
{
    QMutexLocker locker ( &m_dataMutex );
    updateOrAddServerLocked( directoryAddress, server );
}

void CClientAggregator::updateOrAddServerLocked ( const QString& directoryAddress, const Server& server )
{
    // Called with m_dataMutex already locked
    if ( Directory* dir = getDirectory ( directoryAddress ) )
    {
        auto it =
            std::find_if ( dir->servers.begin(), dir->servers.end(), [&] ( const Server& s ) { return s.ip == server.ip && s.port == server.port; } );

        if ( it != dir->servers.end() )
        {
            // Preserve client list, ping response time, and client count
            QList<Client> clients  = it->clients;
            QDateTime     lastPing = it->lastPingResponse;
            int           numClients = it->nclients;
            QDateTime     lastClientListReceived = it->lastClientListReceived; // Preserve this field

            // Update specific fields that come from the directory server list
            it->ip           = server.ip;
            it->port         = server.port;
            it->numip        = server.numip;
            it->name         = server.name;
            it->city         = server.city;
            it->country      = server.country;
            it->country_code = server.country_code;
            it->countryid    = server.countryid;
            it->maxclients   = server.maxclients;
            it->perm         = server.perm;

            // Restore preserved fields
            it->clients            = clients;
            it->lastPingResponse   = lastPing;
            it->nclients           = numClients;
            it->lastClientListReceived = lastClientListReceived; // Restore this field
        }
        else
        {
            dir->servers.append ( server );
        }
        dir->lastModified = QDateTime::currentDateTime();
    }
}

bool CClientAggregator::removeServer ( const QString& directoryAddress, const QString& serverIp, quint16 port )
{
    QMutexLocker locker ( &m_dataMutex );
    if ( Directory* dir = getDirectory ( directoryAddress ) )
    {
        auto it = std::remove_if ( dir->servers.begin(), dir->servers.end(), [&] ( const Server& s ) { return s.ip == serverIp && s.port == port; } );
        if ( it != dir->servers.end() )
        {
            dir->servers.erase ( it, dir->servers.end() );
            dir->lastModified = QDateTime::currentDateTime();
            return true;
        }
    }
    return false;
}

Server* CClientAggregator::getServer ( const QString& directoryAddress, const QString& serverIp, quint16 port )
{
    if ( Directory* dir = getDirectory ( directoryAddress ) )
    {
        auto it = std::find_if ( dir->servers.begin(), dir->servers.end(), [&] ( const Server& s ) { return s.ip == serverIp && s.port == port; } );
        if ( it != dir->servers.end() )
        {
            return &( *it );
        }
    }
    return nullptr;
}

Server* CClientAggregator::getServerFromAnyDirectory(const QString& serverIp, quint16 port)
{
    for ( auto& directory : m_directories )
    {
        if ( Server* server = getServer ( directory.address, serverIp, port ) )
        {
            return server;
        }
    }
    qWarning() << "ClientAggregator: Server not found in any directory for IP:" << serverIp << "Port:" << port;
    return nullptr;
}

void CClientAggregator::updateOrAddClient ( const QString& directoryAddress, const QString& serverIp, quint16 port, const Client& client )
{
    QMutexLocker locker ( &m_dataMutex );
    if ( Server* server = getServer ( directoryAddress, serverIp, port ) )
    {
        auto it = std::find_if ( server->clients.begin(), server->clients.end(), [&] ( const Client& c ) { return c.chanid == client.chanid; } );
        if ( it != server->clients.end() )
        {
            *it = client;
        }
        else
        {
            server->clients.append ( client );
        }
        const QDateTime now  = QDateTime::currentDateTime();
        server->lastModified = now;
        if ( it != server->clients.end() )
        {
            it->lastModified = now;
        }
    }
}

bool CClientAggregator::removeClient ( const QString& directoryAddress, const QString& serverIp, quint16 port, int chanId )
{
    QMutexLocker locker ( &m_dataMutex );
    if ( Server* server = getServer ( directoryAddress, serverIp, port ) )
    {
        auto it = std::remove_if ( server->clients.begin(), server->clients.end(), [=] ( const Client& c ) { return c.chanid == chanId; } );
        if ( it != server->clients.end() )
        {
            server->clients.erase ( it, server->clients.end() );
            server->lastModified = QDateTime::currentDateTime();
            return true;
        }
    }
    return false;
}

Client* CClientAggregator::getClient ( const QString& directoryAddress, const QString& serverIp, quint16 port, int chanId )
{
    QMutexLocker locker ( &m_dataMutex );
    if ( Server* server = getServer ( directoryAddress, serverIp, port ) )
    {
        auto it = std::find_if ( server->clients.begin(), server->clients.end(), [=] ( const Client& c ) { return c.chanid == chanId; } );
        if ( it != server->clients.end() )
        {
            return &( *it );
        }
    }
    return nullptr;
}

QByteArray CClientAggregator::getJson(const QueryParameters& params) const
{
    QMutexLocker lockerData ( &m_dataMutex );
    QMutexLocker lockerCache ( &m_cacheMutex );
    return getJsonLocked(params);
}

QByteArray CClientAggregator::getJsonLocked(const QueryParameters& params) const
{
    // Called with both m_dataMutex and m_cacheMutex already locked
    QJsonObject topLevelObject;
    QJsonArray directoriesArray;
    QJsonArray serversArray;

    QMap<QString, int> directoryIdMap;
    int nextDirectoryId = 0;

    for ( const auto& directory : m_directories )
    {
        // Add directory to directoriesArray
        QJsonObject directoryObj;
        int currentDirectoryId = nextDirectoryId++;
        directoryIdMap.insert(directory.address, currentDirectoryId);

        directoryObj["id"]        = currentDirectoryId;
        directoryObj["address"]   = directory.address;

        // Count servers based on hideEmpty filter
        int serverCount = 0;
        if (params.hideEmpty)
        {
            serverCount = 0;
            for (const auto& server : directory.servers)
            {
                if (server.nclients > 0)
                {
                    serverCount++;
                }
            }
        }
        else
        {
            serverCount = directory.servers.size();
        }

        directoryObj["numservers"] = serverCount;
        directoriesArray.append(directoryObj);

        // Add servers to serversArray, with directory ID
        for ( const auto& server : directory.servers )
        {
            // Skip servers with 0 clients if hideEmpty is true
            if (params.hideEmpty && server.nclients == 0)
            {
                continue;
            }

            QJsonObject serverObj = server.toJson(m_detailedJson);
            serverObj["directory"] = currentDirectoryId;
            serversArray.append ( serverObj );
        }
    }

    topLevelObject["directories"] = directoriesArray;
    topLevelObject["servers"]     = serversArray;

    QByteArray d = QJsonDocument ( topLevelObject ).toJson ( QJsonDocument::Indented );

    return d;
}

QByteArray CClientAggregator::getCompressedJson ( QByteArray& etag, const QueryParameters& params )
{
    QMutexLocker lockerData ( &m_dataMutex );
    QMutexLocker lockerCache ( &m_cacheMutex );

    QString cacheKey = params.getKey();

    // Get or create cache entry for this parameter set
    if (!m_jsonCaches.contains(cacheKey))
    {
        m_jsonCaches[cacheKey] = JsonCacheData();
    }

    JsonCacheData& cacheData = m_jsonCaches[cacheKey];

    QDateTime mostRecentChange;
    for ( const auto& directory : m_directories )
    {
        if ( directory.lastModified > mostRecentChange )
        {
            mostRecentChange = directory.lastModified;
        }
    }

    if ( mostRecentChange > cacheData.cacheTimestamp )
    {
        // Use the internal version that doesn't require additional locks
        QByteArray raw = getJsonLocked(params);
        cacheData.jsonCache      = qCompress ( raw, 9 );
        cacheData.cacheTimestamp = mostRecentChange;
        cacheData.etag           = QCryptographicHash::hash ( raw, QCryptographicHash::Sha1 ).toHex();
    }

    etag = cacheData.etag;
    return cacheData.jsonCache;
}

void CClientAggregator::startHttpServer ( quint16 port )
{
    if ( m_httpServerThread )
        return;

    m_httpServerThread = new QThread();
    m_httpServer       = new CJamStatusServer ( this, port );
    m_httpServer->moveToThread ( m_httpServerThread );

    connect ( m_httpServerThread, &QThread::started, m_httpServer, &CJamStatusServer::start );
    connect ( m_httpServerThread, &QThread::finished, m_httpServer, &QObject::deleteLater );
    m_httpServerThread->start();
}

QString CClientAggregator::normalizedIpString(const QHostAddress& qhAddr) {
    if (qhAddr.protocol() == QAbstractSocket::IPv6Protocol && qhAddr.toIPv4Address() != 0) {
        // It's an IPv4-mapped IPv6 address, convert to IPv4 string
        return QHostAddress(qhAddr.toIPv4Address()).toString();
    } else {
        // It's a pure IPv4 or pure IPv6 address, use its toString() representation
        return qhAddr.toString();
    }
}


CJamStatusServer::CJamStatusServer ( CClientAggregator* aggregator, quint16 port, QObject* parent )
    : QObject ( parent ), m_aggregator ( aggregator ), m_port ( port )
{
    m_server = new QTcpServer ( this );
}

CJamStatusServer::~CJamStatusServer()
{
    stop();
}

void CJamStatusServer::start()
{
    if ( !m_server->listen ( QHostAddress::LocalHost, m_port ) )
    {
        qCritical ( "HTTP server could not start on port %d", m_port );
        return;
    }
    connect ( m_server, &QTcpServer::newConnection, this, &CJamStatusServer::onNewConnection );
    qInfo ( "HTTP server started on http://localhost:%d/jamstatus", m_port );
}

void CJamStatusServer::stop()
{
    for ( QTcpSocket* socket : m_clients )
    {
        m_socketBuffers.remove ( socket );
        socket->close();
    }
    m_clients.clear();
    m_server->close();
}

void CJamStatusServer::onNewConnection()
{
    QTcpSocket* clientSocket = m_server->nextPendingConnection();
    if ( clientSocket )
    {
        m_clients.append ( clientSocket );
        connect ( clientSocket, &QTcpSocket::readyRead, this, &CJamStatusServer::onReadyRead );
        connect ( clientSocket, &QTcpSocket::disconnected, this, &CJamStatusServer::onDisconnected );
    }
}

void CJamStatusServer::onDisconnected()
{
    QTcpSocket* socket = qobject_cast<QTcpSocket*> ( sender() );
    if ( socket )
    {
        m_clients.removeAll ( socket );
        m_socketBuffers.remove ( socket );  // Clean up the buffer for this socket
        socket->deleteLater();
    }
}

void CJamStatusServer::onReadyRead()
{
    QTcpSocket* socket = qobject_cast<QTcpSocket*> ( sender() );
    if ( !socket )
        return;

    // Append new data to existing buffer for this socket
    QByteArray& buffer = m_socketBuffers[socket];
    buffer.append ( socket->readAll() );

    // Try to extract and process complete HTTP requests
    while ( true )
    {
        // Find the end of HTTP headers (double CRLF or double LF)
        int endOfHeaders = findEndOfHeaders ( buffer );
        
        if ( endOfHeaders == -1 )
        {
            // Headers not yet complete
            // Check buffer size to prevent DoS attacks (e.g., max 64KB)
            if ( buffer.size() > MAX_REQUEST_SIZE )
            {
                qWarning() << "CJamStatusServer: HTTP request too large, closing connection";
                socket->close();
                m_socketBuffers.remove ( socket );
            }
            break;
        }

        // Extract the complete request (headers + any body)
        QByteArray completeRequest = buffer.left ( endOfHeaders );
        
        // Remove processed request from buffer
        buffer.remove ( 0, endOfHeaders );

        // Process the complete request
        processCompleteRequest ( socket, completeRequest );

        // If socket was closed during processing, clean up and break
        if ( socket->state() == QTcpSocket::UnconnectedState )
        {
            m_socketBuffers.remove ( socket );
            break;
        }
    }
}

int CJamStatusServer::findEndOfHeaders ( const QByteArray& data )
{
    // Look for double CRLF (HTTP standard)
    int pos = data.indexOf ( "\r\n\r\n" );
    if ( pos != -1 )
    {
        return pos + 4; // Include the double CRLF
    }

    // Also check for double LF (some clients use this)
    pos = data.indexOf ( "\n\n" );
    if ( pos != -1 )
    {
        return pos + 2; // Include the double LF
    }

    return -1; // Headers not yet complete
}

void CJamStatusServer::processCompleteRequest ( QTcpSocket* socket, const QByteArray& completeRequest )
{
    QList<QByteArray> requestLines = completeRequest.split ( '\n' );
    if ( requestLines.isEmpty() )
    {
        socket->close();
        return;
    }

    // Parse the request line
    QList<QByteArray> requestLine = requestLines.first().trimmed().split ( ' ' );
    if ( requestLine.size() < 2 )
    {
        socket->close();
        return;
    }

    QByteArray method = requestLine.at ( 0 ).toUpper();
    QByteArray path   = requestLine.at ( 1 );

    // Verify this is a supported method
    if ( method != "GET" && method != "HEAD" && method != "OPTIONS" )
    {
        socket->write ( "HTTP/1.1 405 Method Not Allowed\r\n" );
        socket->write ( "Allow: GET, HEAD, OPTIONS\r\n" );
        socket->write ( "\r\n" );
        socket->close();
        return;
    }

    // Parse query parameters from path
    QueryParameters params;
    int questionMarkPos = path.indexOf ( '?' );
    QByteArray pathOnly = path;
    
    if ( questionMarkPos != -1 )
    {
        pathOnly = path.left ( questionMarkPos );
        QByteArray queryString = path.mid ( questionMarkPos + 1 );

        // Parse query parameters - only accept known parameters
        QList<QByteArray> queryParts = queryString.split ( '&' );
        for ( const QByteArray& part : queryParts )
        {
            int eqPos = part.indexOf ( '=' );
            QByteArray key = ( eqPos != -1 ) ? part.left ( eqPos ) : part;
            QByteArray value = ( eqPos != -1 ) ? part.mid ( eqPos + 1 ) : QByteArray();

            if ( key == "hideEmpty" )
            {
                params.hideEmpty = ( value == "1" || value == "true" );
            }
            // Add more parameter handling here as needed
        }
    }

    // Check that path is /jamstatus (with or without query parameters)
    if ( pathOnly != "/jamstatus" )
    {
        socket->write ( "HTTP/1.1 404 Not Found\r\n\r\n" );
        socket->close();
        return;
    }

    // Handle OPTIONS method
    if ( method == "OPTIONS" )
    {
        socket->write ( "HTTP/1.1 200 OK\r\n" );
        socket->write ( "Allow: GET, HEAD, OPTIONS\r\n" );
        socket->write ( "Content-Length: 0\r\n" );
        socket->write ( "\r\n" );
        socket->close();
        return;
    }

    QByteArray ifNoneMatch;
    bool       acceptDeflate = false;
    for ( const QByteArray& line : requestLines )
    {
        QByteArray lower = line.toLower();
        if ( lower.startsWith ( "if-none-match:" ) )
        {
            ifNoneMatch = line.mid ( 14 ).trimmed();
        }
        else if ( lower.startsWith ( "accept-encoding:" ) )
        {
            if ( lower.contains ( "deflate" ) )
                acceptDeflate = true;
        }
    }

    QByteArray etag;
    QByteArray compressedJson = m_aggregator->getCompressedJson ( etag, params );

    if ( ifNoneMatch == etag )
    {
        socket->write ( "HTTP/1.1 304 Not Modified\r\n" );
        socket->write ( "ETag: " + etag + "\r\n" );
        socket->write ( "Content-Type: application/json\r\n" );
        socket->write ( "\r\n" );
        socket->close();
        return;
    }

    QByteArray responseHeader;
    QByteArray body;

    if ( acceptDeflate )
    {
        // qCompress format: 4-byte size header + 2-byte zlib header + deflate data + 4-byte Adler-32
        QByteArray qCompressData = compressedJson;

        if ( qCompressData.size() > 10 )
        {
            // Skip the 4-byte qCompress header to get zlib format data
            QByteArray zlibData = qCompressData.mid ( 4 );

            // For "deflate" Content-Encoding, browsers expect zlib format (RFC 1950)
            // which is: 2-byte header + deflate data + 4-byte Adler-32 checksum
            // This is exactly what we have after stripping the qCompress header

            responseHeader.append ( "HTTP/1.1 200 OK\r\n" );
            responseHeader.append ( "Content-Type: application/json\r\n" );
            responseHeader.append ( "Content-Encoding: deflate\r\n" );
            responseHeader.append ( "ETag: " + etag + "\r\n" );
            responseHeader.append ( "Content-Length: " + QByteArray::number ( zlibData.size() ) + "\r\n" );
            responseHeader.append ( "\r\n" );

            body = zlibData;
        }
        else
        {
            // Fallback to uncompressed if compressed data is invalid
            acceptDeflate = false;
        }
    }

    if ( !acceptDeflate )
    {
        // Send uncompressed JSON
        QByteArray raw = m_aggregator->getJson(params);
        responseHeader.append ( "HTTP/1.1 200 OK\r\n" );
        responseHeader.append ( "Content-Type: application/json\r\n" );
        responseHeader.append ( "ETag: " + etag + "\r\n" );
        responseHeader.append ( "Content-Length: " + QByteArray::number ( raw.size() ) + "\r\n" );
        responseHeader.append ( "\r\n" );

        body = raw;
    }

    // For GET, send headers and body. For HEAD, send only headers (no body).
    socket->write ( responseHeader );
    if ( method == "GET" )
    {
        socket->write ( body );
    }
    // HEAD method: headers already sent, no body sent

    socket->close();
}
