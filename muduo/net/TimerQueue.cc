// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif

#include <muduo/net/TimerQueue.h>

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/Timer.h>
#include <muduo/net/TimerId.h>

#include <boost/bind.hpp>

#include <sys/timerfd.h>
#include <unistd.h>

namespace muduo
{
namespace net
{
namespace detail
{

int createTimerfd()
{
  int timerfd = ::timerfd_create(CLOCK_MONOTONIC,
                                 TFD_NONBLOCK | TFD_CLOEXEC);
  if (timerfd < 0)
  {
    LOG_SYSFATAL << "Failed in timerfd_create";
  }
  return timerfd;
}

  // when - now()
struct timespec howMuchTimeFromNow(Timestamp when)
{
  int64_t microseconds = when.microSecondsSinceEpoch()
                         - Timestamp::now().microSecondsSinceEpoch();
  if (microseconds < 100)
  {
    microseconds = 100;
  }
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(
      microseconds / Timestamp::kMicroSecondsPerSecond);
  ts.tv_nsec = static_cast<long>(
      (microseconds % Timestamp::kMicroSecondsPerSecond) * 1000);
  return ts;
}

void readTimerfd(int timerfd, Timestamp now)
{
  uint64_t howmany;
  ssize_t n = ::read(timerfd, &howmany, sizeof howmany);
  LOG_TRACE << "TimerQueue::handleRead() " << howmany << " at " << now.toString();
  if (n != sizeof howmany)
  {
    LOG_ERROR << "TimerQueue::handleRead() reads " << n << " bytes instead of 8";
  }
}

    // 重设timefd的超时时间，用在重新设置超时监听时
void resetTimerfd(int timerfd, Timestamp expiration)
{
  // wake up loop by timerfd_settime()
  struct itimerspec newValue;
  struct itimerspec oldValue;
  bzero(&newValue, sizeof newValue);
  bzero(&oldValue, sizeof oldValue);
  newValue.it_value = howMuchTimeFromNow(expiration);
  int ret = ::timerfd_settime(timerfd, 0, &newValue, &oldValue);
  if (ret)
  {
    LOG_SYSERR << "timerfd_settime()";
  }
}

}
}
}

using namespace muduo;
using namespace muduo::net;
using namespace muduo::net::detail;

TimerQueue::TimerQueue(EventLoop* loop)
  : loop_(loop),
    timerfd_(createTimerfd()),
    timerfdChannel_(loop, timerfd_),
    timers_(),
    callingExpiredTimers_(false)
{
  timerfdChannel_.setReadCallback(
      boost::bind(&TimerQueue::handleRead, this));
  // we are always reading the timerfd, we disarm it with timerfd_settime.
  timerfdChannel_.enableReading(); // todo: channel 在这里注册到了EL.Poller中
}

TimerQueue::~TimerQueue()
{
  timerfdChannel_.disableAll();
  timerfdChannel_.remove();
  ::close(timerfd_);
  // do not remove channel, since we're in EventLoop::dtor();
  for (TimerList::iterator it = timers_.begin();
      it != timers_.end(); ++it)
  {
    delete it->second;
  }
}

// 线程安全的addTimer
TimerId TimerQueue::addTimer(const TimerCallback& cb,
                             Timestamp when,
                             double interval)
{
  Timer* timer = new Timer(cb, when, interval);
  loop_->runInLoop(
      boost::bind(&TimerQueue::addTimerInLoop, this, timer));
  return TimerId(timer, timer->sequence());
}

#ifdef __GXX_EXPERIMENTAL_CXX0X__
TimerId TimerQueue::addTimer(TimerCallback&& cb,
                             Timestamp when,
                             double interval)
{
  Timer* timer = new Timer(std::move(cb), when, interval);
  loop_->runInLoop(
      boost::bind(&TimerQueue::addTimerInLoop, this, timer));
  return TimerId(timer, timer->sequence());
}
#endif

void TimerQueue::cancel(TimerId timerId)
{
  loop_->runInLoop(
      boost::bind(&TimerQueue::cancelInLoop, this, timerId));
}

// 完整的流程，添加Timer到TimerQueue
// 只有TimerQueue的EL才能执行
void TimerQueue::addTimerInLoop(Timer* timer)
{
  loop_->assertInLoopThread();
  bool earliestChanged = insert(timer);

  // 最小超时Timer改变，则更新timefd的超时
  if (earliestChanged)
  {
    resetTimerfd(timerfd_, timer->expiration());
  }
}

// todo: 这里有cancel队列的add操作
void TimerQueue::cancelInLoop(TimerId timerId)
{
  loop_->assertInLoopThread();
  assert(timers_.size() == activeTimers_.size());
  ActiveTimer timer(timerId.timer_, timerId.sequence_);
  ActiveTimerSet::iterator it = activeTimers_.find(timer);
  if (it != activeTimers_.end())
  {
    // 在注册队列中找到，直接删除
    // 这时两种情况
    // 1.没有run expired，所有Timer都在注册队列中
    // 2.正在run expired，但目标Timer没触发，在注册队列中
    size_t n = timers_.erase(Entry(it->first->expiration(), it->first));
    assert(n == 1); (void)n;
    delete it->first; // FIXME: no delete please
    activeTimers_.erase(it);
    // todo: 不需要更新下timefd???
    // 不需要更新timefd，即使erase的是注册队列最小时间，
    // 依然可以按照原来最小时间来timefd触发，最多就是getExpired为空
    // 原则：timefd的触发时间可以小，不可以大，大则会错过注册队列应有的触发时间
  }
  else if (callingExpiredTimers_)
  {
    // 如果正在run expired队列，且要删除的Timer在Expired队列中, 计入cancel
    cancelingTimers_.insert(timer);
  }
  assert(timers_.size() == activeTimers_.size());
}

// call by timefd-channel readable
// todo: 超时timefd触发的处理主流程
// get expired, run, rebind Expired to timers_
void TimerQueue::handleRead()
{
  loop_->assertInLoopThread();
  Timestamp now(Timestamp::now());
  readTimerfd(timerfd_, now);

  std::vector<Entry> expired = getExpired(now);

  callingExpiredTimers_ = true;
  cancelingTimers_.clear();
  // safe to callback outside critical section
  for (std::vector<Entry>::iterator it = expired.begin();
      it != expired.end(); ++it)
  {
    it->second->run(); // timer->run
  }
  callingExpiredTimers_ = false;

  reset(expired, now);
}

// 获取超时Timer到expired，从注册队列和激活队列中删除
std::vector<TimerQueue::Entry> TimerQueue::getExpired(Timestamp now)
{
  // 以now做时间分割，小于now的为超时
  assert(timers_.size() == activeTimers_.size());
  std::vector<Entry> expired;
  Entry sentry(now, reinterpret_cast<Timer*>(UINTPTR_MAX)); // todo: ???
  TimerList::iterator end = timers_.lower_bound(sentry);
  assert(end == timers_.end() || now < end->first);
  std::copy(timers_.begin(), end, back_inserter(expired));

  timers_.erase(timers_.begin(), end); // delete from register queue

  // delete from active queue
  for (std::vector<Entry>::iterator it = expired.begin();
      it != expired.end(); ++it)
  {
    ActiveTimer timer(it->second, it->second->sequence());
    size_t n = activeTimers_.erase(timer);
    assert(n == 1); (void)n;
  }

  assert(timers_.size() == activeTimers_.size());
  return expired;
}

// todo: 这里包括了cancel队列的数据如何使用
// rebind 重新安装Expired队列中的Timer到注册队列激活队列
void TimerQueue::reset(const std::vector<Entry>& expired, Timestamp now)
{
  Timestamp nextExpire;

  for (std::vector<Entry>::const_iterator it = expired.begin();
      it != expired.end(); ++it)
  {
    ActiveTimer timer(it->second, it->second->sequence());
    if (it->second->repeat()
        && cancelingTimers_.find(timer) == cancelingTimers_.end())
    {
      // 重新绑定expired中的timer到注册队列，除了canceling队列中的
      it->second->restart(now); // reset Timer.timestamp
      insert(it->second); // insert to register queue
    }
    else
    {
      // FIXME move to a free list
      delete it->second; // FIXME: no delete please
    }
  }

  if (!timers_.empty())
  {
    // 取注册队列中最小时间项做下一个超时时间
    nextExpire = timers_.begin()->second->expiration();
  }

  if (nextExpire.valid())
  {
    resetTimerfd(timerfd_, nextExpire);
  }
}

// 添加一个Timer到TimerQueue中，包括注册队列和激活队列
// 如果timer是最小时间，则要改变timerfd的设置时间
bool TimerQueue::insert(Timer* timer)
{
  loop_->assertInLoopThread();
  assert(timers_.size() == activeTimers_.size());
  bool earliestChanged = false;
  Timestamp when = timer->expiration();
  TimerList::iterator it = timers_.begin();
  if (it == timers_.end() || when < it->first)
  {
    earliestChanged = true;
  }
  {
    std::pair<TimerList::iterator, bool> result
      = timers_.insert(Entry(when, timer));
    assert(result.second); (void)result;
  }
  {
    std::pair<ActiveTimerSet::iterator, bool> result
      = activeTimers_.insert(ActiveTimer(timer, timer->sequence()));
    assert(result.second); (void)result;
  }

  assert(timers_.size() == activeTimers_.size());
  return earliestChanged;
}

