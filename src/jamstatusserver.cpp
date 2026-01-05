/******************************************************************************\
 * Copyright (c) 2004-2025
 *
 * Author(s):
 *  Volker Fischer
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

#include "jamstatusserver.h"
#include "clientaggregator.h"
#include <QDebug>

CJamStatusServer::CJamStatusServer ( CClientAggregator* aggregator, quint16 port, QObject* parent )
    : QObject ( parent ), m_aggregator ( aggregator ), m_server ( new QTcpServer ( this ) ), m_port ( port )
{
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
    int             questionMarkPos = path.indexOf ( '?' );
    QByteArray      pathOnly         = path;

    if ( questionMarkPos != -1 )
    {
        pathOnly = path.left ( questionMarkPos );
        QByteArray queryString = path.mid ( questionMarkPos + 1 );

        // Parse query parameters - only accept known parameters
        QList<QByteArray> queryParts = queryString.split ( '&' );
        for ( const QByteArray& part : queryParts )
        {
            int        eqPos = part.indexOf ( '=' );
            QByteArray key   = ( eqPos != -1 ) ? part.left ( eqPos ) : part;
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
        QByteArray raw = m_aggregator->getJson ( params );
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
