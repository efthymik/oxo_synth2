//-----------------------------------------------------------------------------
// Flags       : clang-format auto
// Project     : VST SDK
//
// Category    : EditorHost
// Filename    : public.sdk/samples/vst-hosting/editorhost/source/platform/linux/runloop.cpp
// Created by  : Steinberg 09.2016
// Description : Example of opening a plug-in editor
//
//-----------------------------------------------------------------------------
// This file is part of a Steinberg SDK. It is subject to the license terms
// in the LICENSE file found in the top-level directory of this distribution
// and at www.steinberg.net/sdklicenses.
// No part of the SDK, including this file, may be copied, modified, propagated,
// or distributed except according to the terms contained in the LICENSE file.
//-----------------------------------------------------------------------------

#include "platform/linux/runloop.h"
#include <algorithm>
#include <iostream>

//------------------------------------------------------------------------
namespace Steinberg {
namespace Vst {
namespace EditorHost {

using LockGuard = std::lock_guard<std::recursive_mutex>;

//------------------------------------------------------------------------
RunLoop& RunLoop::instance ()
{
	static RunLoop gInstance;
	return gInstance;
}

//------------------------------------------------------------------------
void RunLoop::setDisplay (Display* display)
{
	this->display = display;
}

//------------------------------------------------------------------------
void RunLoop::registerWindow (XID window, const EventCallback& callback)
{
	map.emplace (window, callback);
}

//------------------------------------------------------------------------
void RunLoop::unregisterWindow (XID window)
{
	auto it = map.find (window);
	if (it == map.end ())
		return;
	map.erase (it);
}

//------------------------------------------------------------------------
void RunLoop::registerFileDescriptor (int fd, const FileDescriptorCallback& callback)
{
	fileDescriptors.emplace (fd, callback);
}

//------------------------------------------------------------------------
void RunLoop::unregisterFileDescriptor (int fd)
{
	auto it = fileDescriptors.find (fd);
	if (it == fileDescriptors.end ())
		return;
	fileDescriptors.erase (it);
}

//------------------------------------------------------------------------
void RunLoop::select (timeval* timeout)
{
	int nfds = 0;
	fd_set readFDs = {};
	FD_ZERO (&readFDs);

	// Read readiness only. The sample also watched for write readiness,
	// which an X connection always reports, so select() returned
	// immediately on every call and the loop spun (measured ~150k
	// recvmsg/s at 100 % CPU). IEventHandler::onFDIsSet means "data is
	// available to read".
	for (auto& e : fileDescriptors)
	{
		int fd = e.first;
		FD_SET (fd, &readFDs);
		nfds = std::max (nfds, fd + 1); // select wants the highest fd + 1
	}

	int result = ::select (nfds, &readFDs, nullptr, nullptr, timeout);

	if (result > 0)
	{
		for (auto& e : fileDescriptors)
		{
			if (FD_ISSET (e.first, &readFDs))
				e.second (e.first);
		}
	}
}

//------------------------------------------------------------------------
bool RunLoop::handleEvents ()
{
	auto count = XPending (display);
	if (count == 0)
		return false;
	for (auto i = 0; i < count; ++i)
	{
		XEvent event {};
		XNextEvent (display, &event);
		auto it = map.find (event.xany.window);
		if (it != map.end ())
		{
			it->second (event);
			if (event.type == DestroyNotify)
			{
				map.erase (it);
			}
		}
		else
		{
			XPutBackEvent (display, &event);
			break;
		}
	}
	return true;
}

//------------------------------------------------------------------------
TimerID RunLoop::registerTimer (TimerInterval interval, const TimerCallback& callback)
{
	return timerProcessor.registerTimer (interval, callback);
}

//------------------------------------------------------------------------
void RunLoop::unregisterTimer (TimerID id)
{
	timerProcessor.unregisterTimer (id);
}

//------------------------------------------------------------------------
bool timeValEmpty (timeval& val)
{
	return val.tv_sec == 0 && val.tv_usec == 0;
}

//------------------------------------------------------------------------
void RunLoop::start ()
{
	using namespace std::chrono;
	using clock = high_resolution_clock;

	running = true;

	auto fd = XConnectionNumber (display);
	registerFileDescriptor (fd, [this] (int) { handleEvents (); });

	XSync (display, false);
	handleEvents ();
	while (running && !map.empty ())
	{
		// The delay to the next timer, whatever happened: the sample only
		// recomputed it when a timer had just fired and otherwise blocked in
		// select() with no timeout, so a plugin's 16 ms timer stopped firing
		// as soon as the X connection went quiet.
		auto nextFireTime = timerProcessor.handleTimersAndReturnNextFireTimeInMs ();
		if (nextFireTime == TimerProcessor::noTimers)
		{
			select (nullptr);
		}
		else
		{
			timeval selectTimeout {};
			selectTimeout.tv_sec = nextFireTime / 1000;
			selectTimeout.tv_usec = (nextFireTime - (selectTimeout.tv_sec * 1000)) * 1000;
			select (&selectTimeout);
		}
	}
}

//------------------------------------------------------------------------
void RunLoop::stop ()
{
	running = false;
}

//------------------------------------------------------------------------
//------------------------------------------------------------------------
//------------------------------------------------------------------------
uint64_t TimerProcessor::handleTimersAndReturnNextFireTimeInMs ()
{
	using std::chrono::time_point_cast;

	if (timers.empty ())
		return noTimers;

	auto current = time_point_cast<Millisecond> (Clock::now ());

	std::vector<TimerID> timersToFire;
	for (auto& timer : timers)
	{
		if (timer.nextFireTime > current)
			break;
		timersToFire.push_back (timer.id);
		updateTimerNextFireTime (timer, current);
	}

	for (auto id : timersToFire)
	{
		for (auto& timer : timers)
		{
			if (timer.id == id)
			{
				timer.callback (timer.id);
				break;
			}
		}
	}
	if (!timersToFire.empty ())
		sortTimers ();

	// Always the delay to the next timer: the sample returned noTimers when
	// nothing fired, which made the caller block until the next X event.
	auto nextFireTime = timers.front ().nextFireTime;
	current = now ();
	if (nextFireTime <= current)
		return 0;
	return (nextFireTime - current).count ();
}

//------------------------------------------------------------------------
void TimerProcessor::updateTimerNextFireTime (Timer& timer, TimePoint current)
{
	timer.nextFireTime = current + Millisecond (timer.interval);
}

//------------------------------------------------------------------------
void TimerProcessor::sortTimers ()
{
	std::sort (timers.begin (), timers.end (),
	           [] (const Timer& t1, const Timer& t2) { return t1.nextFireTime < t2.nextFireTime; });
}

//------------------------------------------------------------------------
auto TimerProcessor::now () -> TimePoint
{
	using std::chrono::time_point_cast;

	return time_point_cast<Millisecond> (Clock::now ());
}

//------------------------------------------------------------------------
auto TimerProcessor::registerTimer (TimerInterval interval, const TimerCallback& callback)
    -> TimerID
{
	auto timerId = ++timerIdCounter;
	Timer timer;
	timer.id = timerId;
	timer.callback = callback;
	timer.interval = interval;
	updateTimerNextFireTime (timer, now ());

	timers.emplace_back (std::move (timer));
	sortTimers ();

	return timerId;
}

//------------------------------------------------------------------------
void TimerProcessor::unregisterTimer (TimerID id)
{
	for (auto it = timers.begin (), end = timers.end (); it != end; ++it)
	{
		if (it->id == id)
		{
			timers.erase (it);
			break;
		}
	}
}

//------------------------------------------------------------------------
} // EditorHost
} // Vst
} // Steinberg
