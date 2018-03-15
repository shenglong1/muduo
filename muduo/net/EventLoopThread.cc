// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/EventLoopThread.h>

#include <muduo/net/EventLoop.h>

#include <boost/bind.hpp>

using namespace muduo;
using namespace muduo::net;


// 一旦构造了EventLoopThread，就绑定了回调流程,
// thread_.start -> thread_.fun_ -> EventLoopThread::threadFunc -> cb
EventLoopThread::EventLoopThread(const ThreadInitCallback& cb,
                                 const string& name)
  : loop_(NULL),
    exiting_(false),
    thread_(boost::bind(&EventLoopThread::threadFunc, this), name),
    mutex_(),
    cond_(mutex_),
    callback_(cb)
{
}

EventLoopThread::~EventLoopThread()
{
  exiting_ = true;
  if (loop_ != NULL) // not 100% race-free, eg. threadFunc could be running callback_.
  {
    // still a tiny chance to call destructed object, if threadFunc exits just now.
    // but when EventLoopThread destructs, usually programming is exiting anyway.
    loop_->quit();
    thread_.join();
  }
}

//
// 创建线程去执行threadFunc, 并loop住，返回该线程创建并使用的EL
// 从Thread::start 开始执行整个回调流程
EventLoop* EventLoopThread::startLoop()
{
  assert(!thread_.started());

  // Thread::start -> ELThread::threadFunc -> ELThread::cb
  // start a new thread call threadFunc
  thread_.start();
  // todo: 这里不会子线程先notify，然后本线程再wait吗？
  // 不会，这里没问题，如果父先运行，则父wait，直到子进入loop();
  // 如果子先运行，父直接不会wait;

  {
    MutexLockGuard lock(mutex_);
    while (loop_ == NULL)
    {
      cond_.wait(); // 等待子线程初始化好loop
    }
  }
  // 为何要wait呢，要保证loop_确实非空了，才能返回

  return loop_;
}

// 子线程的整个生命周期就在这里
void EventLoopThread::threadFunc()
{
  EventLoop loop; // 这个loop生命周期随子线程

  if (callback_)
  {
    callback_(&loop);
  }

  {
    MutexLockGuard lock(mutex_);
    loop_ = &loop;
    cond_.notify();
  }

  loop.loop();
  //assert(exiting_);
  loop_ = NULL;
}

