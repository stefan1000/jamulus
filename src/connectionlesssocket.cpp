#include "connectionlesssocket.h"
#include <QDebug>

CConnectionlessSocket::CConnectionlessSocket(QObject* parent)
    : QObject(parent), m_pUdpSocket(new QUdpSocket(this))
{
}

CConnectionlessSocket::~CConnectionlessSocket()
{
    Stop();
}

bool CConnectionlessSocket::Start(quint16 port, bool bEnableIPv6)
{
    Q_UNUSED(bEnableIPv6); // IPv6 handling can be improved later
    if (m_pUdpSocket->state() != QAbstractSocket::UnconnectedState) {
        return true;
    }

    connect(m_pUdpSocket, &QUdpSocket::readyRead, this, &CConnectionlessSocket::onReadyRead);

    if (port == 0) {
        if (m_pUdpSocket->bind(QHostAddress::Any)) {
            return true;
        }
    }

    for (int i = 0; i < NUM_SOCKET_PORTS_TO_TRY; ++i) {
        if (m_pUdpSocket->bind(QHostAddress::Any, port + i)) {
            return true;
        }
    }

    return false;
}

void CConnectionlessSocket::Stop()
{
    if (m_pUdpSocket->state() != QAbstractSocket::UnconnectedState) {
        m_pUdpSocket->close();
        disconnect(m_pUdpSocket, &QUdpSocket::readyRead, this, &CConnectionlessSocket::onReadyRead);
    }
}

quint16 CConnectionlessSocket::GetPort() const
{
    return m_pUdpSocket->localPort();
}

void CConnectionlessSocket::OnCLMessReadyForSending(const CHostAddress& InetAddr, const CVector<uint8_t>& vecMessage)
{
    if (m_pUdpSocket->state() == QAbstractSocket::BoundState) {
        m_pUdpSocket->writeDatagram(reinterpret_cast<const char*>(vecMessage.data()),
                                   vecMessage.Size(),
                                   InetAddr.InetAddr,
                                   InetAddr.iPort);
    }
}

void CConnectionlessSocket::onReadyRead()
{
    while (m_pUdpSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_pUdpSocket->pendingDatagramSize());
        QHostAddress sender;
        quint16 senderPort;

        qint64 bytesRead = m_pUdpSocket->readDatagram(datagram.data(), datagram.size(),
                                &sender, &senderPort);

        if (bytesRead <= 0) {
            qWarning() << "CConnectionlessSocket: readDatagram failed or returned 0 bytes:" << m_pUdpSocket->errorString();
            continue;
        }

        qDebug() << "CConnectionlessSocket: Received" << bytesRead << "bytes from" << sender.toString() << ":" << senderPort;

        CVector<uint8_t> vecbyRecBuf;
        vecbyRecBuf.Init(bytesRead);
        memcpy(vecbyRecBuf.data(), datagram.constData(), bytesRead);

        CVector<uint8_t> vecbyMesBodyData;
        int iRecCounter;
        int iRecID;
        if (!CProtocol::ParseMessageFrame(vecbyRecBuf, vecbyRecBuf.Size(), vecbyMesBodyData, iRecCounter, iRecID)) {
             qDebug() << "CConnectionlessSocket: Parsed message with ID" << iRecID;
             if (CProtocol::IsConnectionLessMessageID(iRecID)) {
                CHostAddress hostAddr;
                hostAddr.InetAddr = sender;
                hostAddr.iPort = senderPort;
                emit CLMessageReceived(vecbyMesBodyData, iRecID, hostAddr);
             }
        } else {
            qWarning() << "CConnectionlessSocket: Failed to parse message frame.";
        }
    }
}
