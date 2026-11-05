#include "mytcpserver.h"

MyTcpServer::MyTcpServer(QObject *parent)
    : QTcpServer(parent)
{
}

void MyTcpServer::incomingConnection(qintptr socketDescriptor)
{
    // 不调用基类实现：基类会 new 一个 QTcpSocket 并触发 newConnection，
    // 而那个 socket 只能留在主线程用。这里直接把描述符抛出去。
    emit newClientDescriptor(socketDescriptor);
}
