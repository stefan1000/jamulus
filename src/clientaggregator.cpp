#include "clientaggregator.h"
#include "connectionlesssocket.h"
#include "util.h"
#include <QDebug>
#include <QCryptographicHash>
#include <algorithm> // Required for std::sort

// Forward declarations for utility functions (defined in clientaggregator_util.cpp)
extern void    setupLogging ( const QString& strLoggingFileName, QtMsgType logLevelMask );
extern void    shutdownLogging();
extern QString skillLevelToString ( ESkillLevel level );
extern QString countryToCode ( QLocale::Country country );

CClientAggregator::CClientAggregator ( const QStringList& directoryAddresses,
                                       bool detailedJson,
                                       const QString& strLoggingFileName,
                                       QtMsgType logLevelMask,
                                       QObject* parent )
    : QObject ( parent ), m_protocol(), m_pSocket( new CConnectionlessSocket( this ) ), m_httpServer ( nullptr ), m_httpServerThread ( nullptr ), m_detailedJson(detailedJson)
{
    // Setup logging if requested
    if ( !strLoggingFileName.isEmpty() )
    {
        setupLogging ( strLoggingFileName, logLevelMask );
    }

    for ( const auto& address : directoryAddresses )
    {
        addDirectory ( address );
    }

    qInfo() << "ClientAggregator: Initialized with directories:" << directoryAddresses;

    // Initialize geolocation service
    m_geoLocation = std::make_unique<CGeoLocation>(this);
    m_geoLocation->setRefreshIntervalMinutes(60*24);     // Refresh cache every day
    m_geoLocation->setCacheJitterSeconds ( 60 * 60 * 4);
    m_geoLocation->setMaxBatchSize(20);              // Max 20 IPs per batch
    m_geoLocation->setRequestTimeoutMs(10000);        // 10 second timeout
    m_geoLocation->setEnableGeoLocation(true);       // Enabled by default

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

    // Geolocation -> this
    connect ( m_geoLocation.get(), &CGeoLocation::geoLocationReady, this, &CClientAggregator::onGeoLocationReady );
}

CClientAggregator::~CClientAggregator()
{
    // Cleanup logging
    shutdownLogging();

    if ( m_httpServerThread )
    {
        m_httpServerThread->quit();
        m_httpServerThread->wait();
    }
    stop();
}

void CClientAggregator::start()
{
    // 
    if ( !m_pSocket->Start ( 22131, true ) // use port 0 for random, enable IPv6
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
    cleanupClientLocationCache();
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
        // First pass: hard-remove hard-expired servers (no ping for AGGREGATOR_SERVER_TIMEOUT_MS)
        directory.servers.erase ( std::remove_if ( directory.servers.begin(),
                                                   directory.servers.end(),
                                                   [&] ( const Server& server ) {
                                                       return server.lastPingResponse.isValid() &&
                                                              server.lastPingResponse.msecsTo ( now ) > AGGREGATOR_SERVER_TIMEOUT_MS;
                                                   } ),
                                  directory.servers.end() );
        
        // Second pass: hard-remove soft-removed servers that have accumulated enough missed pings
        directory.servers.erase ( std::remove_if ( directory.servers.begin(),
                                                   directory.servers.end(),
                                                   [&] ( Server& server ) {
                                                       // If server is soft-removed and hasn't received a ping recently
                                                       if ( !server.inDirectoryList )
                                                       {
                                                           // Check if we should increment missed ping counter
                                                           // This happens on cleanup interval (every 15 seconds)
                                                           // We're being soft about removal - servers can come back
                                                           server.missedPingCount++;
                                                           
                                                           qDebug() << "ClientAggregator: Soft-removed server has been offline for"
                                                                    << (server.missedPingCount * 15) << "seconds:" 
                                                                    << server.ip << ":" << server.port
                                                                    << "(" << server.name << ")"
                                                                    << "- missed" << server.missedPingCount << "pings";
                                                           
                                                           // Hard-remove after N missed pings
                                                           if ( server.missedPingCount >= static_cast<int>(AGGREGATOR_SERVER_SOFT_REMOVAL_PINGS) )
                                                           {
                                                               qInfo() << "ClientAggregator: Hard-removing server after"
                                                                       << AGGREGATOR_SERVER_SOFT_REMOVAL_PINGS << "missed pings:"
                                                                       << server.ip << ":" << server.port
                                                                       << "(" << server.name << ")";
                                                               return true;  // Remove this server
                                                           }
                                                       }
                                                       return false;  // Keep server
                                                   } ),
                                  directory.servers.end() );
        
        // Only update lastModified if we actually removed servers
        // (the erase operation modifies the list)
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
    
    // Track whether any actual changes occurred
    bool hasChanges = false;

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
            // New server added
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

            // Detect and set IP stack type
            updateServerIPStackType(newServer, serverAddr.InetAddr);

            //qDebug() << "ClientAggregator: Adding new server:" << newServer.name << "@" << serverId;
            updateOrAddServerLocked ( dir->address, newServer );
            hasChanges = true;
        }
        else
        {
            // Check if any fields actually changed for existing server
            bool fieldsChanged = false;
            if ( existingServer->name != serverInfo.strName ||
                 existingServer->city != serverInfo.strCity ||
                 existingServer->country != QLocale::countryToString ( serverInfo.eCountry ) ||
                 existingServer->countryid != static_cast<int> ( serverInfo.eCountry ) ||
                 existingServer->maxclients != serverInfo.iMaxNumClients ||
                 existingServer->perm != serverInfo.bPermanentOnline )
            {
                fieldsChanged = true;
            }

            if ( fieldsChanged )
            {
                // Update existing server info
                existingServer->name         = serverInfo.strName;
                existingServer->city         = serverInfo.strCity;
                existingServer->country      = QLocale::countryToString ( serverInfo.eCountry );
                existingServer->country_code = countryToCode(serverInfo.eCountry);
                existingServer->countryid    = static_cast<int> ( serverInfo.eCountry );
                existingServer->maxclients   = serverInfo.iMaxNumClients;
                existingServer->perm         = serverInfo.bPermanentOnline;
                existingServer->lastModified = now;
                hasChanges = true;
            }
        }
    }

    // Check if any servers were removed
    size_t oldServerCount = dir->servers.size();
    dir->servers.erase(
        std::remove_if(dir->servers.begin(), dir->servers.end(),
            [&](const Server& server) {
                QString serverId = server.ip + ":" + QString::number(server.port);
                return !receivedServers.contains(serverId);
            }),
        dir->servers.end());
    
    if ( dir->servers.size() != oldServerCount )
    {
        hasChanges = true;
    }

    // Only update directory lastModified if there were actual changes
    if ( hasChanges )
    {
        dir->lastModified = now;
    }

    // Mark servers not in received list as soft-removed (but keep them)
    // They'll be hard-removed after N missed pings
    for (auto& server : dir->servers)
    {
        QString serverId = server.ip + ":" + QString::number(server.port);
        if (!receivedServers.contains(serverId))
        {
            // Server no longer in directory list, mark as soft-removed
            if (server.inDirectoryList)
            {
                server.inDirectoryList = false;
                server.missedPingCount = 0;  // Reset counter
                qDebug() << "ClientAggregator: Server soft-removed from directory:" << server.ip << ":" << server.port
                         << "(" << server.name << ") - will wait for" << AGGREGATOR_SERVER_SOFT_REMOVAL_PINGS << "failed pings";
            }
        }
        else
        {
            // Server is back in directory list, restore it
            if (!server.inDirectoryList)
            {
                server.inDirectoryList = true;
                server.missedPingCount = 0;
                qDebug() << "ClientAggregator: Server restored to directory:" << server.ip << ":" << server.port
                         << "(" << server.name << ")";
            }
        }
    }

    // Collect all server IPs that need geolocation
    // Also apply cached geolocation data if available
    QStringList ipsToQuery;
    const QDateTime now2 = QDateTime::currentDateTime();
    for (auto& server : dir->servers)
    {
        if (!server.geo_valid)
        {
            // Try to get cached geolocation data first
            GeoLocationData cachedGeo = m_geoLocation->getGeoLocation(server.ip);
            if ( cachedGeo.is_valid )
            {
                // Apply cached geolocation data
                server.geo_latitude  = cachedGeo.latitude;
                server.geo_longitude = cachedGeo.longitude;
                server.geo_valid     = true;
                server.lastModified  = now2;
                hasChanges           = true; // Mark directory as changed
                //qDebug() << "ClientAggregator: Applied cached geolocation for" << server.ip
                //         << "lat:" << cachedGeo.latitude << "lon:" << cachedGeo.longitude;
            }
            else
            {
                // Only query if we don't have valid data from cache
                ipsToQuery.append(server.ip);
            }
        }
    }

    // Release the mutex before making async network requests
    locker.unlock();

    // Query geolocation for servers without valid geo data
    if (!ipsToQuery.isEmpty())
    {
        //qDebug() << "ClientAggregator: Querying geolocation for" << ipsToQuery.size() << "servers";
        m_geoLocation->getGeoLocations(ipsToQuery);
    }
}

void CClientAggregator::onPingTimeReceived ( const CHostAddress& inetAddr, int iPingTime, int iNumClients )
{
    QMutexLocker locker ( &m_dataMutex );
    // qDebug() << "ClientAggregator: Received ping from" << inetAddr.toString() << "- Ping:" << iPingTime << "Clients:" << iNumClients;

    // Find which directory this server belongs to
    const ServerAndDirectory sad = getServerAndDirectory ( normalizedIpString ( inetAddr.InetAddr ), inetAddr.iPort );
    if ( sad )
    {
        Server*    server    = sad.server;
        Directory* directory = sad.directory;
        const QDateTime now = QDateTime::currentDateTime();

        // Store old values to check for meaningful changes that affect the JSON output
        const int          oldNumClients       = server->nclients;
        const EIPStackType oldIpStackType      = server->ipStackType;
        const bool         oldReachableViaIPv4 = server->reachableViaIPv4;
        const bool         oldReachableViaIPv6 = server->reachableViaIPv6;

        // Update server properties from ping
        server->ping     = iPingTime; // This change should not trigger an E-Tag update
        server->nclients = iNumClients;
        updateServerIPStackType ( *server, inetAddr.InetAddr );
        server->missedPingCount = 0;

        bool shouldRequestClientList = false;

        if ( iNumClients > 0 )
        {
            if ( iNumClients != server->lastNumClients )
            {
                // Condition 1: Number of clients changed, so we need an immediate update.
                shouldRequestClientList = true;
            }
            else
            {
                // Condition 2: Number of clients is constant.
                // We periodically query every AGGREGATOR_UPDATE_CLIENTS_INTERVAL_MS
                // to get updated client details (name, instrument, etc.),
                // or if we have never fetched the list before.
                if ( !server->lastClientListFetch.isValid() || server->lastClientListFetch.msecsTo ( now ) > AGGREGATOR_UPDATE_CLIENTS_INTERVAL_MS )
                {
                    shouldRequestClientList = true;
                }
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

        // Update timestamps
        server->lastNumClients   = iNumClients;
        server->lastPingResponse = now;
        server->lastModified     = now; // Server's own timestamp always updated

        // Only update the directory's timestamp if data visible in the JSON has changed
        const bool numClientsChanged = ( oldNumClients != server->nclients );
        const bool ipStackChanged =
            ( oldIpStackType != server->ipStackType || oldReachableViaIPv4 != server->reachableViaIPv4 || oldReachableViaIPv6 != server->reachableViaIPv6 );

        if ( numClientsChanged || ipStackChanged )
        {
            directory->lastModified = now;
        }
    }
    else
    {
        // Server not found - this is now less of an issue since we soft-remove servers
        // But still log it as it helps track directory consistency
        QString serverKey = normalizedIpString ( inetAddr.InetAddr ) + ":" + QString::number ( inetAddr.iPort );
        qDebug() << "ClientAggregator: Ping received from unknown server" << serverKey << "(may be pending soft removal after missed pings)";
    }
}

void CClientAggregator::onVersionReceived ( const CHostAddress& inetAddr, COSUtil::EOpSystemType eOSType, const QString& version )
{
    QMutexLocker locker ( &m_dataMutex );
    // qDebug() << "ClientAggregator: Received version from" << inetAddr.toString() << "- Version:" << version;
    const ServerAndDirectory sad = getServerAndDirectory ( normalizedIpString ( inetAddr.InetAddr ), inetAddr.iPort );
    if ( sad )
    {
        Server*    server    = sad.server;
        Directory* directory = sad.directory;
        const QDateTime now = QDateTime::currentDateTime();
        server->version     = version;
        server->os          = COSUtil::GetOperatingSystemString ( eOSType );
        server->lastModified    = now;
        directory->lastModified = now;
    }
}

void CClientAggregator::onConnClientsListMesReceived ( const CHostAddress& inetAddr, const CVector<CChannelInfo>& vecChanInfo )
{
    QMutexLocker locker ( &m_dataMutex );
    // qDebug() << "ClientAggregator: Received client list from" << inetAddr.toString() << "with" << vecChanInfo.Size() << "clients.";

    const ServerAndDirectory sad = getServerAndDirectory ( normalizedIpString ( inetAddr.InetAddr ), inetAddr.iPort );
    if ( sad )
    {
        Server*    server    = sad.server;
        Directory* directory = sad.directory;
        const QDateTime now = QDateTime::currentDateTime();

        // Build list of new clients before clearing
        QList<Client> newClients;
        for ( const auto& chanInfo : vecChanInfo )
        {
            Client newClient;
            newClient.chanid       = chanInfo.iChanID;
            newClient.country      = QLocale::countryToString ( chanInfo.eCountry );
            newClient.country_code = countryToCode ( chanInfo.eCountry );
            newClient.instrument   = CInstPictures::GetName ( chanInfo.iInstrument );
            newClient.skill        = skillLevelToString ( chanInfo.eSkillLevel );
            newClient.skillid      = static_cast<int> ( chanInfo.eSkillLevel );
            newClient.countryid    = static_cast<int> ( chanInfo.eCountry );
            newClient.instrumentid = chanInfo.iInstrument;
            newClient.name         = chanInfo.strName;
            newClient.city         = chanInfo.strCity;
            newClient.lastModified = now;
            newClients.append ( newClient );
        }

        // Check for clients that moved from other servers and trigger refetch
        QString serverIp   = normalizedIpString ( inetAddr.InetAddr );
        quint16 serverPort = inetAddr.iPort;
        checkAndRequestRefetchForMovedClients ( serverIp, serverPort, newClients );

        server->clients.clear();
        for ( const auto& client : newClients )
        {
            server->clients.append ( client );
        }

        // Sort clients by chanid
        std::sort ( server->clients.begin(), server->clients.end(), [] ( const Client& a, const Client& b ) { return a.chanid < b.chanid; } );

        server->lastClientListReceived = now; // Update the timestamp when client list was received
        server->lastModified           = now;
        directory->lastModified        = now;
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

ServerAndDirectory CClientAggregator::getServerAndDirectory ( const QString& serverIp, quint16 port )
{
    for ( auto& directory : m_directories )
    {
        auto it = std::find_if ( directory.servers.begin(), directory.servers.end(), [&] ( const Server& s ) { return s.ip == serverIp && s.port == port; } );
        if ( it != directory.servers.end() )
        {
            return ServerAndDirectory{ & ( *it ), &directory };
        }
    }
    return ServerAndDirectory{ nullptr, nullptr };
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

void CClientAggregator::onGeoLocationReady(const QString& ip, const GeoLocationData& geoData)
{
    QMutexLocker locker(&m_dataMutex);
    
    if (!geoData.is_valid)
    {
        qDebug() << "ClientAggregator: Geolocation query failed for" << ip;
        return;
    }

    // Epsilon value for comparing floating-point coordinates
    // 0.0001 degrees ≈ 11 meters at the equator (sufficient precision)
    static constexpr double COORD_EPSILON = 0.0001;

    // Find and update all servers with this IP
    for (auto& directory : m_directories)
    {
        bool anyServerUpdated = false;

        for (auto& server : directory.servers)
        {
            if (server.ip == ip)
            {
                // Only update if coordinates actually changed (using epsilon comparison)
                bool latChanged = std::abs(server.geo_latitude - geoData.latitude) > COORD_EPSILON;
                bool lonChanged = std::abs(server.geo_longitude - geoData.longitude) > COORD_EPSILON;
                bool validityChanged = (server.geo_valid != true);

                if (latChanged || lonChanged || validityChanged)
                {
                    server.geo_latitude = geoData.latitude;
                    server.geo_longitude = geoData.longitude;
                    server.geo_valid = true;
                    server.lastModified = QDateTime::currentDateTime();
                    anyServerUpdated = true;

                    //qDebug() << "ClientAggregator: Updated geolocation for" << ip
                    //         << "lat:" << geoData.latitude << "lon:" << geoData.longitude;
                }
            }
        }

        // Only update directory timestamp if we actually changed server data
        if (anyServerUpdated)
        {
            directory.lastModified = QDateTime::currentDateTime();
        }
    }
}

// Helper function to detect IPv4/IPv6 stack type from an address
EIPStackType CClientAggregator::detectIPStackType(const QHostAddress& addr)
{
    if (addr.protocol() == QAbstractSocket::IPv6Protocol)
    {
        // Check if it's an IPv4-mapped IPv6 address
        quint32 ipv4 = addr.toIPv4Address();
        if (ipv4 != 0)
        {
            // It's an IPv4-mapped IPv6 address, treat as IPv4
            return EIPStackType::IPV4_ONLY;
        }
        else
        {
            // Pure IPv6 address
            return EIPStackType::IPV6_ONLY;
        }
    }
    else if (addr.protocol() == QAbstractSocket::IPv4Protocol)
    {
        return EIPStackType::IPV4_ONLY;
    }
    
    return EIPStackType::UNKNOWN;
}

// Update server's IP stack type based on newly received address
void CClientAggregator::updateServerIPStackType(Server& server, const QHostAddress& addr)
{
    EIPStackType newType = detectIPStackType(addr);
    
    if (newType == EIPStackType::IPV4_ONLY)
    {
        server.reachableViaIPv4 = true;
    }
    else if (newType == EIPStackType::IPV6_ONLY)
    {
        server.reachableViaIPv6 = true;
        // Store IPv6 address if different from main IP
        QString ipv6Str = normalizedIpString(addr);
        if (ipv6Str != server.ip)
        {
            server.ipv6Address = ipv6Str;
        }
    }
    
    // Update the overall stack type based on what we've detected so far
    if (server.reachableViaIPv4 && server.reachableViaIPv6)
    {
        server.ipStackType = EIPStackType::DUAL_STACK;
    }
    else if (server.reachableViaIPv4)
    {
        server.ipStackType = EIPStackType::IPV4_ONLY;
    }
    else if (server.reachableViaIPv6)
    {
        server.ipStackType = EIPStackType::IPV6_ONLY;
    }
    
    //qDebug() << "ClientAggregator: Updated IP stack type for" << server.ip 
    //         << "- Type:" << static_cast<int>(server.ipStackType)
    //         << "IPv4:" << server.reachableViaIPv4 << "IPv6:" << server.reachableViaIPv6;
}

QString CClientAggregator::generateClientKey(const Client& client) const
{
    // Generate a unique key for a client based on name, countryid, and skillid
    return client.name + ":" + QString::number(client.countryid) + ":" + QString::number(client.skillid);
}

void CClientAggregator::checkAndRequestRefetchForMovedClients(const QString& serverIp, quint16 serverPort, const QList<Client>& newClients)
{
    // Called with m_dataMutex already locked
    // This method:
    // 1. Detects clients that moved from other servers
    // 2. Collects servers that need refetch (deduped)
    // 3. Updates the active server map for all clients
    // 4. Requests refetch for moved servers (once per server)
    
    QSet<QString> serversNeedingRefetch;  // Deduplicated set of servers to refetch: "ip:port"
    
    // First pass: Check for moved clients and collect servers to refetch
    for (const auto& client : newClients)
    {
        QString clientKey = generateClientKey(client);
        auto it = m_clientActiveOnServerMap.find(clientKey);
        
        if (it != m_clientActiveOnServerMap.end())
        {
            const ClientLocation& previousLocation = it.value();
            // Check if client moved from a different server
            if (previousLocation.serverIp != serverIp || previousLocation.serverPort != serverPort)
            {
                qInfo() << "ClientAggregator: Client moved from" << previousLocation.serverIp << ":" << previousLocation.serverPort
                        << "to" << serverIp << ":" << serverPort << "- Client:" << client.name;
                
                // Mark the previous server for refetch
                QString prevServerId = previousLocation.serverIp + ":" + QString::number(previousLocation.serverPort);
                serversNeedingRefetch.insert(prevServerId);
            }
        }
        
        // Update the active location for this client
        ClientLocation newLocation;
        newLocation.serverIp = serverIp;
        newLocation.serverPort = serverPort;
        newLocation.lastSeen = QDateTime::currentDateTime();
        m_clientActiveOnServerMap[clientKey] = newLocation;
    }
    
    // Second pass: Request refetch for servers with moved clients (each server once)
    for (const QString& serverId : serversNeedingRefetch)
    {
        // Parse serverId back to IP and port
        int colonPos = serverId.lastIndexOf(":");
        if (colonPos > 0)
        {
            QString previousServerIp   = serverId.left ( colonPos );
                        quint16 previousServerPort = serverId.mid ( colonPos + 1 ).toUShort();
            
                        const ServerAndDirectory sad = getServerAndDirectory ( previousServerIp, previousServerPort );
                        if ( sad )
                        {
                            Server* previousServer = sad.server;
                            // Request a refetch by requesting the connected clients list
                            CHostAddress previousServerAddr;
                            if ( NetworkUtil().ParseNetworkAddress ( serverId, previousServerAddr, true ) )
                            {
                                qDebug() << "ClientAggregator: Requesting client list refetch for server" << serverId << "to verify client removal";
                                m_protocol.CreateCLReqConnClientsListMes ( previousServerAddr );
                                previousServer->lastClientListFetch = QDateTime::currentDateTime();
                            }
                        }
        }
    }
}

void CClientAggregator::cleanupClientLocationCache()
{
    QMutexLocker locker(&m_dataMutex);
    
    const QDateTime now = QDateTime::currentDateTime();
    
    // Only run cleanup every AGGREGATOR_CLIENT_LOCATION_CLEANUP_MS milliseconds
    if (m_lastClientLocationCleanup.isValid() &&
        m_lastClientLocationCleanup.msecsTo(now) < AGGREGATOR_CLIENT_LOCATION_CLEANUP_MS)
    {
        return;
    }
    
    m_lastClientLocationCleanup = now;
    
    // Verify all entries actually exist on their tracked servers
    // Remove stale entries (clients no longer on any server)
    auto it = m_clientActiveOnServerMap.begin();
    int removedMissing = 0;
    while (it != m_clientActiveOnServerMap.end())
    {
        const ClientLocation& location = it.value();
        bool clientFound = false;
        
        // Search for this client on the tracked server
        const ServerAndDirectory sad = getServerAndDirectory ( location.serverIp, location.serverPort );
        if ( sad )
        {
            Server* server = sad.server;
            // Parse the client key to get the identifying info
            QStringList parts = it.key().split ( ":" );
            if ( parts.size() == 3 )
            {
                QString clientName = parts[0];
                int countryid = parts[1].toInt();
                int skillid = parts[2].toInt();
                
                // Check if this client exists on the server
                for (const auto& serverClient : server->clients)
                {
                    if (serverClient.name == clientName &&
                        serverClient.countryid == countryid &&
                        serverClient.skillid == skillid)
                    {
                        clientFound = true;
                        // Update lastSeen to keep the entry fresh
                        it.value().lastSeen = now;
                        break;
                    }
                }
            }
        }
        
        if (!clientFound)
        {
            // Client no longer on tracked server - check if expired
            if (it.value().lastSeen.msecsTo(now) > AGGREGATOR_CLIENT_LOCATION_TTL_MS)
            {
                qDebug() << "ClientAggregator: Removing expired stale client location entry:" << it.key()
                         << "from" << location.serverIp << ":" << location.serverPort;
                it = m_clientActiveOnServerMap.erase(it);
                removedMissing++;
            }
            else
            {
                // Not yet expired, keep in case client returns soon
                qDebug() << "ClientAggregator: Client" << it.key() << "removed from server but kept in cache"
                         << "for" << (AGGREGATOR_CLIENT_LOCATION_TTL_MS - it.value().lastSeen.msecsTo(now)) / 1000 << "more seconds";
                ++it;
            }
        }
        else
        {
            ++it;
        }
    }
    
    if (removedMissing > 0)
    {
        qDebug() << "ClientAggregator: Client location cache cleanup - Removed" << removedMissing
                 << "expired stale entries. Cache size:" << m_clientActiveOnServerMap.size();
    }
}
