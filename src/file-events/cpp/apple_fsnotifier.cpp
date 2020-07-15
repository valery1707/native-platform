#if defined(__APPLE__)

#include "apple_fsnotifier.h"

using namespace std;

void Server::openEventStream() {
    if (watchPoints.empty()) {
        logToJava(LogLevel::FINE, "Not starting event stream as there is nothing to watch", nullptr);
        return;
    }

    CFMutableArrayRef pathArray = CFArrayCreateMutable(NULL, watchPoints.size(), NULL);
    if (pathArray == NULL) {
        throw FileWatcherException("Could not allocate array to store roots to watch");
    }

    for (auto& it : watchPoints) {
        const u16string& path = it.first;
        CFStringRef cfPath = CFStringCreateWithCharacters(NULL, (UniChar*) path.c_str(), path.length());
        if (cfPath == nullptr) {
            throw FileWatcherException("Could not allocate CFString for path", path);
        }
        CFArrayAppendValue(pathArray, cfPath);
        // Do not release path separately as it causes a segmentation fault
        // CFRelease(cfPath);
    }

    // We are not going to get a kFSEventStreamEventFlagHistoryDone event if we start from now
    finishedProcessingHistoricalEvents = lastSeenEventId == kFSEventStreamEventIdSinceNow;

    FSEventStreamContext context = {
        0,               // version, must be 0
        (void*) this,    // info
        NULL,            // retain
        NULL,            // release
        NULL             // copyDescription
    };
    logToJava(LogLevel::FINE, "Starting stream from %d", lastSeenEventId);
    FSEventStreamRef eventStream = FSEventStreamCreate(
        NULL,
        &handleEventsCallback,
        &context,
        pathArray,
        lastSeenEventId,
        latencyInMillis / 1000.0,
        kFSEventStreamCreateFlagNoDefer | kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagWatchRoot);
    CFRelease(pathArray);
    if (eventStream == NULL) {
        throw FileWatcherException("Couldn't update event stream");
    }
    FSEventStreamScheduleWithRunLoop(eventStream, threadLoop, kCFRunLoopDefaultMode);
    FSEventStreamStart(eventStream);
    this->eventStream = eventStream;
}

void Server::closeEventStream() {
    if (eventStream == nullptr) {
        logToJava(LogLevel::FINE, "Event stream not open, not closing", nullptr);
        return;
    }

    FSEventStreamFlushSync(eventStream);
    FSEventStreamStop(eventStream);
    FSEventStreamInvalidate(eventStream);
    FSEventStreamRelease(eventStream);
    eventStream = nullptr;
    logToJava(LogLevel::FINE, "Closed event stream with last seen ID: %d", lastSeenEventId);
}

//
// Server
//

void acceptTrigger(void* info) {
    Server* server = (Server*) info;
    server->handleCommands();
}

void Server::handleCommands() {
    unique_lock<mutex> lock(commandMutex);
    while (!commands.empty()) {
        auto command = commands.front();
        commands.pop();
        executeCommand(command);
    }
}

void Server::queueOnRunLoop(Command* command) {
    unique_lock<mutex> lock(commandMutex);
    commands.push(command);
    CFRunLoopSourceSignal(messageSource);
    CFRunLoopWakeUp(threadLoop);
}

Server::Server(JNIEnv* env, jobject watcherCallback, long latencyInMillis, long commandTimeoutInMillis)
    : AbstractServer(env, watcherCallback)
    , latencyInMillis(latencyInMillis)
    , commandTimeoutInMillis(commandTimeoutInMillis) {
    CFRunLoopSourceContext context = {
        0,               // version;
        (void*) this,    // info;
        NULL,            // retain()
        NULL,            // release()
        NULL,            // copyDescription()
        NULL,            // equal()
        NULL,            // hash()
        NULL,            // schedule()
        NULL,            // cancel()
        acceptTrigger    // perform()
    };
    messageSource = CFRunLoopSourceCreate(
        kCFAllocatorDefault,    // allocator
        0,                      // index
        &context                // context
    );
}

void Server::initializeRunLoop() {
    threadLoop = CFRunLoopGetCurrent();
    CFRunLoopAddSource(threadLoop, messageSource, kCFRunLoopDefaultMode);
}

void Server::runLoop() {
    CFRunLoopRun();

    closeEventStream();
    CFRelease(messageSource);
}

void Server::shutdownRunLoop() {
    CFRunLoopStop(threadLoop);
}

static void handleEventsCallback(
    ConstFSEventStreamRef,
    void* clientCallBackInfo,
    size_t numEvents,
    void* eventPaths,
    const FSEventStreamEventFlags eventFlags[],
    const FSEventStreamEventId eventIds[]) {
    Server* server = (Server*) clientCallBackInfo;
    server->handleEvents(numEvents, (char**) eventPaths, eventFlags, eventIds);
}

/**
 * List of events ignored by our implementation.
 * Anything not ignored here should be handled.
 * If macOS later adds more flags, we'll report those as unknown events this way.
 */
static const FSEventStreamEventFlags IGNORED_FLAGS = kFSEventStreamCreateFlagNone
    // | kFSEventStreamEventFlagMustScanSubDirs
    | kFSEventStreamEventFlagUserDropped
    | kFSEventStreamEventFlagKernelDropped
    | kFSEventStreamEventFlagEventIdsWrapped
    | kFSEventStreamEventFlagHistoryDone
    // | kFSEventStreamEventFlagRootChanged
    // | kFSEventStreamEventFlagMount
    // | kFSEventStreamEventFlagUnmount
    // | kFSEventStreamEventFlagItemCreated
    // | kFSEventStreamEventFlagItemRemoved
    // | kFSEventStreamEventFlagItemInodeMetaMod
    // | kFSEventStreamEventFlagItemRenamed
    // | kFSEventStreamEventFlagItemModified
    // | kFSEventStreamEventFlagItemFinderInfoMod
    // | kFSEventStreamEventFlagItemChangeOwner
    // | kFSEventStreamEventFlagItemXattrMod
    | kFSEventStreamEventFlagItemIsFile
    | kFSEventStreamEventFlagItemIsDir
    | kFSEventStreamEventFlagItemIsSymlink
    | kFSEventStreamEventFlagOwnEvent
    | kFSEventStreamEventFlagItemIsHardlink
    | kFSEventStreamEventFlagItemIsLastHardlink
    | kFSEventStreamEventFlagItemCloned;

WatchPointState Server::getWatchPointState(const u16string& path) {
    for (auto& it : watchPoints) {
        const u16string& prefix = it.first;
        if (path.compare(0, prefix.size(), prefix) == 0
            && (prefix.size() == path.size() || path.at(prefix.size() == '/'))) {
            return it.second;
        }
    }
    throw FileWatcherException("Couldn't find watch point for path", path);
}

void Server::handleEvents(
    size_t numEvents,
    char** eventPaths,
    const FSEventStreamEventFlags eventFlags[],
    const FSEventStreamEventId eventIds[]) {
    JNIEnv* env = getThreadEnv();

    try {
        for (size_t i = 0; i < numEvents; i++) {
            const FSEventStreamEventFlags flags = eventFlags[i];
            const FSEventStreamEventId eventId = eventIds[i];
            lastSeenEventId = eventId;
            if (IS_SET(flags, kFSEventStreamEventFlagHistoryDone)) {
                // Mark all new watch points as able to receive historical events from this point on
                for (auto& it : watchPoints) {
                    if (it.second == WatchPointState::NEW) {
                        it.second = WatchPointState::HISTORICAL;
                    }
                }
                finishedProcessingHistoricalEvents = true;
                logToJava(LogLevel::FINE, "Finished processing historical events (ID %d)", eventId);
                continue;
            }

            const char* path = eventPaths[i];
            logToJava(LogLevel::FINE, "Event 0x%x (ID %d) for '%s'", flags, eventId, path);

            const u16string pathStr = utf8ToUtf16String(path);

            if (eventId == 0 && IS_SET(flags, kFSEventStreamEventFlagRootChanged)) {
                reportChangeEvent(env, ChangeType::INVALIDATED, pathStr);
                continue;
            }

            // Ignore historical events for freshly registered paths
            if (!finishedProcessingHistoricalEvents) {
                WatchPointState state = getWatchPointState(pathStr);
                if (state == WatchPointState::NEW) {
                    logToJava(LogLevel::FINE, "Ignoring historical event (ID %d)", eventId);
                    continue;
                }
            }

            if ((flags & ~IGNORED_FLAGS) == kFSEventStreamCreateFlagNone) {
                logToJava(LogLevel::FINE, "Ignoring event (ID %d)", eventId);
                continue;
            }

            handleEvent(env, pathStr, flags);
        }
    } catch (const exception& ex) {
        reportFailure(env, ex);
    }
}

void Server::handleEvent(JNIEnv* env, const u16string& path, const FSEventStreamEventFlags flags) {
    if (IS_SET(flags, kFSEventStreamEventFlagMustScanSubDirs)) {
        reportOverflow(env, path);
        return;
    }

    ChangeType type;
    if (IS_SET(flags,
            kFSEventStreamEventFlagMount
                | kFSEventStreamEventFlagUnmount)) {
        type = ChangeType::INVALIDATED;
    } else if (IS_SET(flags, kFSEventStreamEventFlagItemRenamed)) {
        if (IS_SET(flags, kFSEventStreamEventFlagItemCreated)) {
            type = ChangeType::REMOVED;
        } else {
            type = ChangeType::CREATED;
        }
    } else if (IS_SET(flags, kFSEventStreamEventFlagItemModified)) {
        type = ChangeType::MODIFIED;
    } else if (IS_SET(flags, kFSEventStreamEventFlagItemRemoved)) {
        type = ChangeType::REMOVED;
    } else if (IS_SET(flags,
                   kFSEventStreamEventFlagItemInodeMetaMod    // file locked
                       | kFSEventStreamEventFlagItemFinderInfoMod
                       | kFSEventStreamEventFlagItemChangeOwner
                       | kFSEventStreamEventFlagItemXattrMod)) {
        type = ChangeType::MODIFIED;
    } else if (IS_SET(flags, kFSEventStreamEventFlagItemCreated)) {
        type = ChangeType::CREATED;
    } else {
        reportUnknownEvent(env, path);
        return;
    }

    reportChangeEvent(env, type, path);
}

void Server::registerPaths(const vector<u16string>& paths) {
    executeOnRunLoop(commandTimeoutInMillis, [this, paths]() {
        closeEventStream();
        for (auto& path : paths) {
            if (watchPoints.find(path) != watchPoints.end()) {
                throw FileWatcherException("Already watching path", path);
            }
            WatchPointState state = lastSeenEventId == kFSEventStreamEventIdSinceNow
                ? WatchPointState::HISTORICAL
                : WatchPointState::NEW;
            logToJava(LogLevel::FINE, "Registering watch point '%s' with state %d", utf16ToUtf8String(path).c_str(), state);
            watchPoints.emplace(path, state);
        }
        openEventStream();
        return true;
    });
}

bool Server::unregisterPaths(const vector<u16string>& paths) {
    return executeOnRunLoop(commandTimeoutInMillis, [this, paths]() {
        closeEventStream();
        bool success = true;
        for (auto& path : paths) {
            logToJava(LogLevel::FINE, "Unregistering watch point '%s'", utf16ToUtf8String(path).c_str());
            if (watchPoints.erase(path) == 0) {
                logToJava(LogLevel::INFO, "Path is not watched: %s", utf16ToUtf8String(path).c_str());
                success = false;
            }
        }
        openEventStream();
        return success;
    });
}

JNIEXPORT jobject JNICALL
Java_net_rubygrapefruit_platform_internal_jni_OsxFileEventFunctions_startWatcher0(JNIEnv* env, jclass, long latencyInMillis, long commandTimeoutInMillis, jobject javaCallback) {
    return wrapServer(env, new Server(env, javaCallback, latencyInMillis, commandTimeoutInMillis));
}

#endif
