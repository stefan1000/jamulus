#pragma once

#include <QObject>
#include <QUdpSocket>
#include "protocol.h"
#include "util.h"
#include "socket.h"

class CConnectionlessSocket : public QObject
{
    Q_OBJECT

public:
    CConnectionlessSocket(QObject* parent = nullptr);
    ~CConnectionlessSocket();

    bool Start(quint16 port, bool bEnableIPv6);
    void Stop();
    quint16 GetPort() const;

public slots:
    void OnCLMessReadyForSending(const CHostAddress& InetAddr, const CVector<uint8_t>& vecMessage);

signals:
    void CLMessageReceived(const CVector<uint8_t>& vecbyMesBodyData, int iRecID, const CHostAddress& InetAddr);

private slots:
    void onReadyRead();

private:
    QUdpSocket* m_pUdpSocket;
};
