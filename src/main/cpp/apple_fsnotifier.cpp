/*
 Initial version copied from Initial version copied from https://github.com/JetBrains/intellij-community/blob/master/native/fsNotifier/mac/fsnotifier.c
 */
// Copyright 2000-2018 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license that can be found in the LICENSE file.

/*
 * Apple specific functions.
 */
#if defined(__APPLE__)

#include "native.h"
#include "generic.h"
#include <CoreServices/CoreServices.h>
#include <pthread.h>
#include <strings.h>

JavaVM* jvm = NULL;
bool invalidStateDetected = false;

typedef struct watch_details {
    CFMutableArrayRef rootsToWatch;
    FSEventStreamRef watcherStream;
    pthread_t watcherThread;
    jobject watcherCallback;
    CFRunLoopRef threadLoop;
} watch_details_t;

static void reportEvent(const char *event, char *path, jobject watcherCallback) {
    size_t len = 0;
    if (path != NULL) {
        len = strlen(path);
        for (char *p = path; *p != '\0'; p++) {
            if (*p == '\n') {
                *p = '\0';
            }
        }
    }
    JNIEnv* env;
    int getEnvStat = jvm->GetEnv((void **)&env, JNI_VERSION_1_6);
    if (getEnvStat == JNI_EDETACHED) {
        if (jvm->AttachCurrentThread((void **) &env, NULL) != JNI_OK) {
            invalidStateDetected = true;
            return;
        }
    } else if (getEnvStat == JNI_EVERSION) {
        invalidStateDetected = true;
        return;
    }

    jclass callback_class = env->GetObjectClass(watcherCallback);
    jmethodID methodCallback = env->GetMethodID(callback_class, "pathChanged", "(Ljava/lang/String;)V");
    env->CallVoidMethod(watcherCallback, methodCallback, env->NewStringUTF(path));
}

static void callback(ConstFSEventStreamRef streamRef,
                     void *clientCallBackInfo,
                     size_t numEvents,
                     void *eventPaths,
                     const FSEventStreamEventFlags eventFlags[],
                     const FSEventStreamEventId eventIds[]) {
    if (invalidStateDetected) return;
    char **paths = (char**) eventPaths;

    jobject watcherCallback = (jobject) clientCallBackInfo;

    for (int i = 0; i < numEvents; i++) {
        // TODO[max] Lion has much more detailed flags we need accurately process. For now just reduce to SL events range.
        FSEventStreamEventFlags flags = eventFlags[i] & 0xFF;
        if ((flags & kFSEventStreamEventFlagMustScanSubDirs) != 0) {
            reportEvent("RECDIRTY", paths[i], watcherCallback);
        } else if (flags != kFSEventStreamEventFlagNone) {
            reportEvent("RESET", NULL, watcherCallback);
        } else {
            reportEvent("DIRTY", paths[i], watcherCallback);
        }
    }
}

static void *EventProcessingThread(void *data) {
    watch_details_t *details = (watch_details_t*) data;
    CFRunLoopRef threadLoop = CFRunLoopGetCurrent();
    FSEventStreamScheduleWithRunLoop(details->watcherStream, threadLoop, kCFRunLoopDefaultMode);
    FSEventStreamStart(details->watcherStream);
    details->threadLoop = threadLoop;
    // This triggers run loop for this thread, causing it to run until we explicitly stop it.
    CFRunLoopRun();
    return NULL;
}

JNIEXPORT jobject JNICALL
Java_net_rubygrapefruit_platform_internal_jni_OsxFileEventFunctions_startWatch(JNIEnv *env, jclass target, jobjectArray paths, CFAbsoluteTime latency, jobject javaCallback, jobject result) {
    invalidStateDetected = false;
    CFMutableArrayRef rootsToWatch = CFArrayCreateMutable(NULL, 0, NULL);
    if (rootsToWatch == NULL) {
        mark_failed_with_errno(env, "Could not allocate array to store roots to watch.", result);
        return NULL;
    }
    int count = env->GetArrayLength(paths);
    if (count == 0) {
        mark_failed_with_errno(env, "No paths given to watch.", result);
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        jstring path = (jstring) env->GetObjectArrayElement(paths, i);
        char* pathString = java_to_char(env, path, result);
        if (pathString == NULL) {
            mark_failed_with_errno(env, "Could not allocate string to store root to watch.", result);
            return NULL;
        }
        CFStringRef stringPath = CFStringCreateWithCString(NULL, pathString, kCFStringEncodingUTF8);
        free(pathString);
        if (stringPath == NULL) {
            mark_failed_with_errno(env, "Could not create CFStringRef.", result);
            return NULL;
        }
        CFArrayAppendValue(rootsToWatch, stringPath);
    }

    jobject watcherCallback = env->NewGlobalRef(javaCallback);
    if (watcherCallback == NULL) {
        mark_failed_with_errno(env, "Could not create global reference for callback.", result);
        return NULL;
    }

    FSEventStreamContext context = {0, (void*) watcherCallback, NULL, NULL, NULL};
    FSEventStreamRef watcherStream = FSEventStreamCreate (
                NULL,
                &callback,
                &context,
                rootsToWatch,
                kFSEventStreamEventIdSinceNow,
                latency,
                kFSEventStreamCreateFlagNoDefer);
    if (watcherStream == NULL) {
         mark_failed_with_errno(env, "Could not create FSEventStreamCreate to track changes.", result);
         return NULL;
    }

    jclass clsWatch = env->FindClass("net/rubygrapefruit/platform/internal/jni/OsxFileEventFunctions$WatchImpl");
    jmethodID constructor = env->GetMethodID(clsWatch, "<init>", "(Ljava/lang/Object;)V");
    watch_details_t* details = (watch_details_t*)malloc(sizeof(watch_details_t));
    details->rootsToWatch = rootsToWatch;
    details->watcherStream = watcherStream;
    details->watcherCallback = watcherCallback;

    if (pthread_create(&(details->watcherThread), NULL, EventProcessingThread, details) != 0) {
        mark_failed_with_errno(env, "Could not create file watcher thread.", result);
        return NULL;
    }

    // TODO Should this be somewhere global?
    int jvmStatus = env->GetJavaVM(&jvm);
    if (jvmStatus < 0) {
        mark_failed_with_errno(env, "Could not store jvm instance.", result);
        return NULL;
    }

    return env->NewObject(clsWatch, constructor, env->NewDirectByteBuffer(details, sizeof(details)));
}

JNIEXPORT void JNICALL
Java_net_rubygrapefruit_platform_internal_jni_OsxFileEventFunctions_stopWatch(JNIEnv *env, jclass target, jobject detailsObj, jobject result) {
    watch_details_t *details = (watch_details_t*) env->GetDirectBufferAddress(detailsObj);
    CFMutableArrayRef rootsToWatch = details->rootsToWatch;
    FSEventStreamRef watcherStream = details->watcherStream;
    pthread_t watcherThread = details->watcherThread;
    jobject watcherCallback = details->watcherCallback;
    CFRunLoopRef threadLoop = details->threadLoop;
    free(details);

    // if there were no roots to watch, there are no resources to release
    if (rootsToWatch == NULL) return;
    if (invalidStateDetected) {
        // report and reset flag, but try to clean up state as much as possible
        mark_failed_with_errno(env, "Watcher is in invalid state, reported changes may be incorrect.", result);
    }

    for (int i = 0; i < CFArrayGetCount(rootsToWatch); i++) {
        const void *value = CFArrayGetValueAtIndex(rootsToWatch, i);
        CFRelease(value);
    }
    // TODO Can we release these earlier?
    CFRelease(rootsToWatch);
    rootsToWatch = NULL;

    FSEventStreamStop(watcherStream);
    FSEventStreamInvalidate(watcherStream);
    FSEventStreamRelease(watcherStream);
    // TODO: consider using FSEventStreamFlushSync to flush all pending events.
    watcherStream = NULL;

    // TODO Shouldn't we stop this first and then release the rest?
    CFRunLoopStop(threadLoop);
    threadLoop = NULL;

    env->DeleteGlobalRef(watcherCallback);
    watcherCallback = NULL;

    pthread_join(watcherThread, NULL);
    watcherThread = NULL;
}

#endif