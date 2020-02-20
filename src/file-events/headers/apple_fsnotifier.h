#pragma once

#if defined(__APPLE__)

#include "generic_fsnotifier.h"
#include "net_rubygrapefruit_platform_internal_jni_OsxFileEventFunctions.h"
#include <CoreServices/CoreServices.h>

using namespace std;

class Server;

static void handleEventsCallback(
    ConstFSEventStreamRef streamRef,
    void* clientCallBackInfo,
    size_t numEvents,
    void* eventPaths,
    const FSEventStreamEventFlags eventFlags[],
    const FSEventStreamEventId eventIds[]);

class EventStream {
public:
    EventStream(Server* server, CFRunLoopRef runLoop, CFArrayRef rootsToWatch, long latencyInMillis);
    ~EventStream();

private:
    FSEventStreamRef watcherStream;
    Server* server;
};

class Server : AbstractServer {
public:
    Server(JNIEnv* env, jobject watcherCallback, CFArrayRef rootsToWatch, long latencyInMillis);
    ~Server();

    void handleEvents(
        size_t numEvents,
        char** eventPaths,
        const FSEventStreamEventFlags eventFlags[],
        const FSEventStreamEventId eventIds[]);

protected:
    void runLoop(JNIEnv* env, function<void(exception_ptr)> notifyStarted) override;

private:
    void handleEvent(JNIEnv* env, char* path, FSEventStreamEventFlags flags);

    const CFArrayRef rootsToWatch;
    const long latencyInMillis;

    CFRunLoopRef threadLoop;
};

#endif
