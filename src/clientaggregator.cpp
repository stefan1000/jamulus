#include "clientaggregator.h"
#include "connectionlesssocket.h"
#include "util.h"
#include <QDebug>
#include <QCryptographicHash>

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
    }
    obj["instrumentid"] = instrumentid;
    obj["skillid"]      = skillid;
    obj["country_code"] = country_code;
    obj["instrument"]   = instrument;
    obj["skill"]        = skill;
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
    if ( !m_pSocket->Start ( 0, true ) ) // use port 0 for random, enable IPv6
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
    qDebug() << "ClientAggregator: Querying directories...";
    for ( auto it = m_directories.begin(); it != m_directories.end(); ++it )
    {
        CHostAddress haDirectoryAddress;
        if ( NetworkUtil().ParseNetworkAddress ( it.key(), haDirectoryAddress, false ) )
        {
            qDebug() << "ClientAggregator: Sending server list request to" << it.key();
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
    QString    directoryAddressStr = inetAddr.toString ( CHostAddress::SM_IP_PORT );
    qDebug() << "ClientAggregator: Received server list from" << directoryAddressStr << "with" << vecServerInfo.Size() << "servers.";
    // Find directory by IP, not by full address, as port can differ
    Directory* dir = nullptr;
    for ( auto it = m_directories.begin(); it != m_directories.end(); ++it )
    {
        CHostAddress haDirAddr;
        if ( NetworkUtil().ParseNetworkAddress ( it.key(), haDirAddr, false ) )
        {
            if ( haDirAddr.InetAddr.isEqual(inetAddr.InetAddr) )
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
                QString serverIp;
                if (serverAddr.InetAddr.protocol() == QAbstractSocket::IPv6Protocol && serverAddr.InetAddr.toIPv4Address() != 0) {
                    serverIp = QHostAddress(serverAddr.InetAddr.toIPv4Address()).toString();
                } else {
                    serverIp = serverAddr.InetAddr.toString();
                }
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
            newServer.ipaddrs      = serverInfo.LHostAddr.toString(); // Add this line
            newServer.name         = serverInfo.strName;
            newServer.city         = serverInfo.strCity;
            newServer.country      = QLocale::countryToString ( serverInfo.eCountry );
            newServer.country_code = countryToCode(serverInfo.eCountry);
            newServer.countryid    = static_cast<int> ( serverInfo.eCountry );
            newServer.maxclients   = serverInfo.iMaxNumClients;
            newServer.perm         = serverInfo.bPermanentOnline;
            newServer.lastModified = now;
            qDebug() << "ClientAggregator: Adding new server:" << newServer.name << "@" << serverId;
            updateOrAddServer ( dir->address, newServer );
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
    qDebug() << "ClientAggregator: Received ping from" << inetAddr.toString() << "- Ping:" << iPingTime << "Clients:" << iNumClients;
    // Find which directory this server belongs to
    for ( auto& directory : m_directories )
    {
        if ( Server* server = getServer ( directory.address, normalizedIpString(inetAddr.InetAddr), inetAddr.iPort ) )
        {
            const QDateTime now = QDateTime::currentDateTime();
            server->ping        = iPingTime;
            server->nclients    = iNumClients; // Update nclients directly from ping response
            if ( iNumClients > 0 )
            {
                m_protocol.CreateCLReqConnClientsListMes ( inetAddr );
            }
            server->lastPingResponse = now;
            server->lastModified     = now;
            directory.lastModified   = now;
            break; // Assume server is in one directory only
        }
    }
}

void CClientAggregator::onVersionReceived ( const CHostAddress& inetAddr, COSUtil::EOpSystemType eOSType, const QString& version )
{
    // qDebug() << "ClientAggregator: Received version from" << inetAddr.toString() << "- Version:" << version;
    for ( auto& directory : m_directories )
    {
        if ( Server* server = getServer ( directory.address, inetAddr.InetAddr.toString(), inetAddr.iPort ) )
        {
            const QDateTime now = QDateTime::currentDateTime();
            server->version     = version;
            server->os          = COSUtil::GetOperatingSystemString ( eOSType );
            server->lastModified   = now;
            directory.lastModified = now;
            break;
        }
    }
}

void CClientAggregator::onConnClientsListMesReceived ( const CHostAddress& inetAddr, const CVector<CChannelInfo>& vecChanInfo )
{
    qDebug() << "ClientAggregator: Received client list from" << inetAddr.toString() << "with" << vecChanInfo.Size() << "clients.";

    for ( auto& directory : m_directories )
    {
        if ( Server* server = getServer ( directory.address, inetAddr.InetAddr.toString(), inetAddr.iPort ) )
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

            server->lastModified   = now;
            directory.lastModified = now;
            break; // Assume server is in one directory only
        }
    }
}

void CClientAggregator::addDirectory ( const QString& address )
{
    if ( !m_directories.contains ( address ) )
    {
        m_directories.insert ( address, { address, {}, QDateTime::currentDateTime(), QDateTime() } );
    }
}

void CClientAggregator::removeDirectory ( const QString& address ) { m_directories.remove ( address ); }

Directory* CClientAggregator::getDirectory ( const QString& address )
{
    auto it = m_directories.find ( address );
    return ( it != m_directories.end() ) ? &it.value() : nullptr;
}

void CClientAggregator::updateOrAddServer ( const QString& directoryAddress, const Server& server )
{
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

void CClientAggregator::updateOrAddClient ( const QString& directoryAddress, const QString& serverIp, quint16 port, const Client& client )
{
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

QByteArray CClientAggregator::getJson() const
{
    QMutexLocker locker ( &m_cacheMutex );
    QJsonArray serverList;
    int        index = 0;
    for ( const auto& directory : m_directories )
    {
        for ( const auto& server : directory.servers )
        {
            QJsonObject serverObj = server.toJson(m_detailedJson);
            serverObj["index"]    = index++;
            serverList.append ( serverObj );
        }
    }
    QByteArray d = QJsonDocument ( serverList ).toJson ( QJsonDocument::Indented );

    return d;
}

QByteArray CClientAggregator::getCompressedJson ( QByteArray& etag )
{
    QMutexLocker locker ( &m_cacheMutex );

    QDateTime mostRecentChange;
    for ( const auto& directory : m_directories )
    {
        if ( directory.lastModified > mostRecentChange )
        {
            mostRecentChange = directory.lastModified;
        }
    }

    if ( mostRecentChange > m_cacheTimestamp )
    {
        QByteArray raw = getJson();
        m_jsonCache      = qCompress ( raw, 9 );
        m_cacheTimestamp = mostRecentChange;
        m_etag           = QCryptographicHash::hash ( raw, QCryptographicHash::Sha1 ).toHex();
    }

    etag = m_etag;
    return m_jsonCache;
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
        socket->close();
    }
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
        socket->deleteLater();
    }
}

void CJamStatusServer::onReadyRead()
{
    QTcpSocket* socket = qobject_cast<QTcpSocket*> ( sender() );
    if ( !socket )
        return;

    QByteArray      requestData  = socket->readAll();
    QList<QByteArray> requestLines = requestData.split ( '\n' );
    if ( requestLines.isEmpty() )
    {
        socket->close();
        return;
    }

    QList<QByteArray> requestLine = requestLines.first().trimmed().split ( ' ' );
    if ( requestLine.size() < 2 )
    {
        socket->close();
        return;
    }

    QByteArray method = requestLine.at ( 0 ).toUpper();
    QByteArray path   = requestLine.at ( 1 );

    if ( path != "/jamstatus" )
    {
        socket->write ( "HTTP/1.1 404 Not Found\r\n\r\n" );
        socket->close();
        return;
    }

    QByteArray ifNoneMatch;
    bool acceptDeflate = false;
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
    QByteArray compressedJson = m_aggregator->getCompressedJson ( etag );

    if ( ifNoneMatch == etag )
    {
        socket->write ( "HTTP/1.1 304 Not Modified\r\n" );
        socket->write ( "ETag: " + etag + "\r\n" );
        socket->write ( "\r\n" );
        socket->close();
        return;
    }

    QByteArray responseHeader;
    QByteArray body;

    if ( acceptDeflate )
    {
        // Client accepts deflate: qCompress output is zlib with 4-byte header; remove first 4 bytes to produce raw deflate
        QByteArray deflateData = compressedJson;
        if ( deflateData.size() > 6 ) // Ensure there's enough data to strip header (2 bytes) and trailer (4 bytes)
        {
            // Strip 2-byte zlib header and 4-byte zlib checksum to get raw deflate
            deflateData = deflateData.mid(2, deflateData.size() - 6);
        }
        else // Handle cases where data is too small to be valid zlib/deflate
        {
            deflateData.clear(); // Send empty or error if data is malformed
        }

        responseHeader.append ( "HTTP/1.1 200 OK\r\n" );
        responseHeader.append ( "Content-Type: application/json\r\n" );
        responseHeader.append ( "Content-Encoding: deflate\r\n" );
        responseHeader.append ( "ETag: " + etag + "\r\n" );
        responseHeader.append ( "Content-Length: " + QByteArray::number ( deflateData.size() ) + "\r\n" );
        responseHeader.append ( "\r\n" );

        body = deflateData;
    }
    else
    {
        // Send uncompressed JSON (client likely doesn't accept compression)
        QByteArray raw = m_aggregator->getJson();
        responseHeader.append ( "HTTP/1.1 200 OK\r\n" );
        responseHeader.append ( "Content-Type: application/json\r\n" );
        responseHeader.append ( "ETag: " + etag + "\r\n" );
        responseHeader.append ( "Content-Length: " + QByteArray::number ( raw.size() ) + "\r\n" );
        responseHeader.append ( "\r\n" );

        body = raw;
    }

    if ( method == "GET" )
    {
        socket->write ( responseHeader );
        socket->write ( body );
    }

    socket->close();
}
