// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is an internal header file, you should not include this.

#ifndef MUDUO_NET_EVENTLOOPTHREADPOOL_H
#define MUDUO_NET_EVENTLOOPTHREADPOOL_H

#include <muduo/base/Types.h>

#include <vector>
#include <boost/function.hpp>
#include <boost/noncopyable.hpp>
#include <boost/ptr_container/ptr_vector.hpp>

namespace muduo
{

namespace net
{

class EventLoop;
class EventLoopThread;

// EventLoopThreadPool就是实现了one_loop_per_thread模式的线程池
class EventLoopThreadPool : boost::noncopyable
{
 public:
  typedef boost::function<void(EventLoop*)> ThreadInitCallback;

  EventLoopThreadPool(EventLoop* baseLoop, const string& nameArg);
  ~EventLoopThreadPool();
  void setThreadNum(int numThreads) { numThreads_ = numThreads; }
  void start(const ThreadInitCallback& cb = ThreadInitCallback());

  // valid after calling start()
  /// round-robin
  EventLoop* getNextLoop();

  /// with the same hash code, it will always return the same EventLoop
  EventLoop* getLoopForHash(size_t hashCode);

  std::vector<EventLoop*> getAllLoops();

  bool started() const
  { return started_; }

  const string& name() const
  { return name_; }

 private:

  EventLoop* baseLoop_; // 线程池外部主EL
  string name_;
  bool started_;
  int numThreads_;
  int next_; // 下一个待使用的EL编号，在loops_中的下标
  boost::ptr_vector<EventLoopThread> threads_;
  std::vector<EventLoop*> loops_; // 线程池内部EL
  // 这些EL实际是EventLoopThread子线程运行threadFunc栈上的EL
};

}
}

#endif  // MUDUO_NET_EVENTLOOPTHREADPOOL_H
