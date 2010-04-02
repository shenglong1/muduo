// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is an internal header file, you should not include this.

#ifndef MUDUO_NET_EPOLLPOLLER_H
#define MUDUO_NET_EPOLLPOLLER_H

#include <muduo/net/Poller.h>

#include <map>
#include <vector>

namespace muduo
{
namespace net
{

///
/// IO Multiplexing with epoll(4).
///
class EPollPoller : public Poller
{
 public:

  virtual ~EPollPoller();

  virtual Timestamp poll(int timeoutMs, ChannelList* activeChannels);
  virtual void updateChannel(Channel* channel);

 private:
  std::map<int, Channel*> channels_;
};

}
}
#endif  // MUDUO_NET_EPOLLPOLLER_H