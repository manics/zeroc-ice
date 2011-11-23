// **********************************************************************
//
// Copyright (c) 2003-2009 ZeroC, Inc. All rights reserved.
//
// This copy of Ice is licensed to you under the terms described in the
// ICE_LICENSE file included in this distribution.
//
// **********************************************************************

#ifndef ICE_EVENT_HANDLER_F_H
#define ICE_EVENT_HANDLER_F_H

#include <IceUtil/Shared.h>

#include <Ice/Handle.h>

namespace IceInternal
{

class EventHandler;
ICE_API IceUtil::Shared* upCast(EventHandler*);
typedef Handle<EventHandler> EventHandlerPtr;

class ThreadPoolWorkItem;
ICE_API IceUtil::Shared* upCast(ThreadPoolWorkItem*);
typedef Handle<ThreadPoolWorkItem> ThreadPoolWorkItemPtr;

}

#endif