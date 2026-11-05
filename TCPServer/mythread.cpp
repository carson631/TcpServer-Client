#include "mythread.h"

#include <QTcpSocket>
#include <QHostAddress>
#include <QDebug>

MyThread::MyThread(qintptr socketDescriptor, QObject *parent)
    : QThread(parent)
    , m_socketDescriptor(socketDescriptor)
{
}

MyThread::~MyThread()
{
    stopThread();
    wait(2000);
}

void MyThread::sendLine(const QString &line)
{
    QMutexLocker locker(&m_mutex);
    m_out.append(line);
}

void MyThread::requestClose()
{
    QMutexLocker locker(&m_mutex);
    m_close = true;
}

void MyThread::stopThread()
{
    QMutexLocker locker(&m_mutex);
    m_abort = true;
}

// run() 是线程体：在这里创建 QTcpSocket、循环收数据、把每一行通过信号抛给主线程。
// 注意 QTcpSocket 必须在线程内部创建——直接在别的线程 new 出来再传进来是不允许的。
void MyThread::run()
{
    QTcpSocket socket;

    // 用 QTcpServer 交出来的描述符接管这条连接
    if (!socket.setSocketDescriptor(m_socketDescriptor)) {
        emit sigError(this, QStringLiteral("接管连接失败：%1").arg(socket.errorString()));
        return;
    }

    const QString peer = QStringLiteral("%1:%2")
                             .arg(socket.peerAddress().toString())
                             .arg(socket.peerPort());
    emit sigConnected(this, peer);

    QByteArray buffer;

    forever {
        // 1) 把主线程塞进队列的回复真正写出去（QTcpSocket 不是线程安全的，
        //    所以只能由本线程来写，主线程只负责入队）
        QStringList pending;
        bool closeAfterFlush = false;
        {
            QMutexLocker locker(&m_mutex);
            if (m_abort)
                break;
            pending = m_out;
            m_out.clear();
            closeAfterFlush = m_close;
        }
        for (const QString &line : pending)
            socket.write((line + QLatin1Char('\n')).toUtf8());
        if (!pending.isEmpty())
            socket.flush();
        if (closeAfterFlush)
            break;

        // 2) 阻塞等待新数据，最多 20ms —— 顺便保证发送队列的延迟不会超过 20ms
        if (!socket.waitForReadyRead(20)) {
            if (socket.state() != QAbstractSocket::ConnectedState)
                break;              // 对端已断开
            continue;
        }

        // 3) TCP 是字节流，没有消息边界：按 \n 切帧，不完整的留在 buffer 等下次
        buffer += socket.readAll();
        int idx;
        while ((idx = buffer.indexOf('\n')) >= 0) {
            QByteArray raw = buffer.left(idx);
            buffer.remove(0, idx + 1);
            if (raw.endsWith('\r'))     // 兼容 \r\n
                raw.chop(1);
            if (raw.isEmpty())
                continue;
            emit sigLine(this, QString::fromUtf8(raw));
        }
    }

    socket.disconnectFromHost();
    if (socket.state() != QAbstractSocket::UnconnectedState)
        socket.waitForDisconnected(500);

    emit sigDisconnected(this, peer);
}
