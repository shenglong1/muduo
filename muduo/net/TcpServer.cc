// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/TcpServer.h>

#include <muduo/base/Logging.h>
#include <muduo/net/Acceptor.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/EventLoopThreadPool.h>
#include <muduo/net/SocketsOps.h>

#include <boost/bind.hpp>

#include <stdio.h>  // snprintf

using namespace muduo;
using namespace muduo::net;

TcpServer::TcpServer(EventLoop* loop,
                     const InetAddress& listenAddr,
                     const string& nameArg,
                     Option option)
  : loop_(CHECK_NOTNULL(loop)),
    ipPort_(listenAddr.toIpPort()),
    name_(nameArg),

    acceptor_(new Acceptor(loop, listenAddr, option == kReusePort)),
    threadPool_(new EventLoopThreadPool(loop, name_)),
    connectionCallback_(defaultConnectionCallback),
    messageCallback_(defaultMessageCallback),
    nextConnId_(1)
{
  acceptor_->setNewConnectionCallback(
      boost::bind(&TcpServer::newConnection, this, _1, _2));
}

TcpServer::~TcpServer()
{
  loop_->assertInLoopThread();
  LOG_TRACE << "TcpServer::~TcpServer [" << name_ << "] destructing";

  for (ConnectionMap::iterator it(connections_.begin());
      it != connections_.end(); ++it)
  {
    TcpConnectionPtr conn(it->second);
    it->second.reset();
    conn->getLoop()->runInLoop(
      boost::bind(&TcpConnection::connectDestroyed, conn));
  }
}

void TcpServer::setThreadNum(int numThreads)
{
  assert(0 <= numThreads);
  threadPool_->setThreadNum(numThreads);
}

void TcpServer::start()
{
  if (started_.getAndSet(1) == 0) // 多次start保护
  {
    threadPool_->start(threadInitCallback_); // create n个EventLoopThread

    assert(!acceptor_->listenning());
    loop_->runInLoop(
        boost::bind(&Acceptor::listen, get_pointer(acceptor_)));
  }
}

// 连接建立后首次拿到connfd后建立TcpConnection的核心操作
// Acceptor 通知TcpServer 连接建立了
// call by Accept.default_cb after accept
// sockfd is new connfd
void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr)
{
  loop_->assertInLoopThread();
  EventLoop* ioLoop = threadPool_->getNextLoop();
  char buf[64];
  snprintf(buf, sizeof buf, "-%s#%d", ipPort_.c_str(), nextConnId_);
  ++nextConnId_;
  string connName = name_ + buf;

  LOG_INFO << "TcpServer::newConnection [" << name_
           << "] - new connection [" << connName
           << "] from " << peerAddr.toIpPort();
  InetAddress localAddr(sockets::getLocalAddr(sockfd));
  // FIXME poll with zero timeout to double confirm the new connection
  // FIXME use make_shared if necessary
  // todo: 绑定conn到threadPool中的某一个EL
  TcpConnectionPtr conn(new TcpConnection(ioLoop,
                                          connName,
                                          sockfd,
                                          localAddr,
                                          peerAddr)); // create conn-channel
  connections_[connName] = conn;
  conn->setConnectionCallback(connectionCallback_);
  conn->setMessageCallback(messageCallback_);
  conn->setWriteCompleteCallback(writeCompleteCallback_);

  // todo: TcpServer才知道删除一个conn时要从两个地方注销conn，EL.poller和TcpServer.connections_
  // todo: 这里是典型的this 裸指针给出，必须确保this的声明周期长于TcpConnection !!!
  conn->setCloseCallback(
      boost::bind(&TcpServer::removeConnection, this, _1)); // FIXME: unsafe
  ioLoop->runInLoop(boost::bind(&TcpConnection::connectEstablished, conn));
  // conn 是sp，这里交给FO没有生命周期问题
}

// conn通知TcpServer删除自己
// 给conn的closecb
// todo: 这里为何要runInLoop呢？因为这个FO是注册到conn中的，conn调用此FO时，实际上是在conn所在的loop thread中
// 这是注册的TcpConnection.closeCallback
// in sub thread
void TcpServer::removeConnection(const TcpConnectionPtr& conn)
{
  // conn ref = 1
  // FIXME: unsafe
  // conn 是sp，bind后拷贝了，这里交给FO没有生命周期问题
  loop_->runInLoop(boost::bind(&TcpServer::removeConnectionInLoop, this, conn));
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn)
{
  // conn ref = 2
  loop_->assertInLoopThread();
  LOG_INFO << "TcpServer::removeConnectionInLoop [" << name_
           << "] - connection " << conn->name();
  size_t n = connections_.erase(conn->name()); // 这里conn ref=1 不析构
  (void)n;
  assert(n == 1);
  EventLoop* ioLoop = conn->getLoop();
  // todo: 为何要用queueInLoop
  ioLoop->queueInLoop(
      boost::bind(&TcpConnection::connectDestroyed, conn)); // copy conn
  // todo: 当从这返回后，当前FO持有的conn sp析构，ref=1，最后一个conn sp 在queue出去的FO中
}

