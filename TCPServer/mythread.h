#ifndef MYTHREAD_H
#define MYTHREAD_H

#include <QThread>
#include <QMutex>
#include <QStringList>

class QTcpSocket;

// 每个客户端连接对应一个 MyThread。
//
// 为什么不用 QTcpServer 默认的“单线程 + 事件循环”模式？
//   默认模式下所有客户端共用一个事件循环，某个客户端的处理（尤其是数据库查询）
//   会把其他客户端一起拖住。这里改成“一个连接一个线程”，每个线程里用阻塞式
//   waitForReadyRead() 收数据，互相之间完全独立。
//
// 线程边界：
//   本类只做 socket 收发 + 按 \n 切帧；解析协议、读写数据库全部在主线程完成
//   （QSqlDatabase 的连接不能跨线程使用，放主线程最省事），
//   两者之间用自定义信号 sigLine / sigConnected / sigDisconnected 传递。
class MyThread : public QThread
{
    Q_OBJECT
public:
    explicit MyThread(qintptr socketDescriptor, QObject *parent = nullptr);
    ~MyThread() override;

    // 线程安全：主线程随时可以调，内容先入队，由 run() 里的循环负责真正写出
    void sendLine(const QString &line);
    // 发完队列里已有的内容再收工（用于“账号被删除”这类需要先通知再断开的情况）
    void requestClose();
    void stopThread();

signals:
    void sigConnected(MyThread *self, const QString &peer);       // 连接建立
    void sigDisconnected(MyThread *self, const QString &peer);    // 连接断开
    void sigLine(MyThread *self, const QString &line);            // 收到一整行
    void sigError(MyThread *self, const QString &message);        // 接管/收发异常

protected:
    void run() override;   // 重写 run：线程体的全部逻辑都在这里

private:
    qintptr m_socketDescriptor;
    QMutex m_mutex;        // 保护 m_out / m_abort
    QStringList m_out;     // 待发送队列（主线程写入，run() 消费）
    bool m_abort = false;  // 立即中止
    bool m_close = false;  // 把队列发完再断开
};

#endif // MYTHREAD_H
