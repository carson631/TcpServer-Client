#ifndef MYTCPSERVER_H
#define MYTCPSERVER_H

#include <QTcpServer>

// 只做一件事：把新连接的文件描述符交给主线程，由主线程开一个 MyThread 去接管。
//
// 为什么不用 QTcpServer::newConnection 信号？
//   那个信号触发时 QTcpServer 已经自己 new 了一个 QTcpSocket，而 QTcpSocket 不能
//   跨线程搬移。重写 incomingConnection(qintptr) 后，QTcpServer 不再自己建 socket，
//   我们拿到原始描述符，就能在子线程里用 setSocketDescriptor() 接管——这是 Qt 官方
//   文档里多线程服务器的标准做法。
class MyTcpServer : public QTcpServer
{
    Q_OBJECT
public:
    explicit MyTcpServer(QObject *parent = nullptr);

signals:
    void newClientDescriptor(qintptr socketDescriptor);

protected:
    void incomingConnection(qintptr socketDescriptor) override;
};

#endif // MYTCPSERVER_H
