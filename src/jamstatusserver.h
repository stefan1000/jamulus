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

#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QMap>
#include <QByteArray>

class CClientAggregator;

class CJamStatusServer : public QObject
{
    Q_OBJECT

public:
    explicit CJamStatusServer ( CClientAggregator* aggregator, quint16 port, QObject* parent = nullptr );
    ~CJamStatusServer();

public slots:
    void start();
    void stop();

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();

private:
    static int findEndOfHeaders ( const QByteArray& data );
    void       processCompleteRequest ( QTcpSocket* socket, const QByteArray& completeRequest );

    CClientAggregator* m_aggregator;
    QTcpServer*        m_server;
    quint16            m_port;
    QList<QTcpSocket*> m_clients;
    QMap<QTcpSocket*, QByteArray> m_socketBuffers;

    static constexpr int MAX_REQUEST_SIZE = 65536;
};
