/*
 * Copyright 2020 the original author or authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package net.rubygrapefruit.platform.file

import groovy.transform.Memoized
import net.rubygrapefruit.platform.Native
import net.rubygrapefruit.platform.file.FileWatcherCallback.Type
import net.rubygrapefruit.platform.internal.Platform
import net.rubygrapefruit.platform.internal.jni.AbstractFileEventFunctions
import net.rubygrapefruit.platform.internal.jni.LinuxFileEventFunctions
import net.rubygrapefruit.platform.internal.jni.NativeLogger
import net.rubygrapefruit.platform.internal.jni.OsxFileEventFunctions
import net.rubygrapefruit.platform.internal.jni.WindowsFileEventFunctions
import net.rubygrapefruit.platform.testfixture.JniChecksEnabled
import net.rubygrapefruit.platform.testfixture.JulLogging
import org.junit.Rule
import org.junit.experimental.categories.Category
import org.junit.rules.TemporaryFolder
import org.junit.rules.TestName
import org.spockframework.util.Assert
import spock.lang.Specification
import spock.lang.Timeout

import java.util.concurrent.BlockingQueue
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException
import java.util.function.BooleanSupplier
import java.util.function.Predicate
import java.util.logging.Level
import java.util.logging.Logger
import java.util.regex.Pattern

import static java.util.concurrent.TimeUnit.SECONDS
import static java.util.logging.Level.CONFIG

@Timeout(value = 10, unit = SECONDS)
@Category(JniChecksEnabled)
abstract class AbstractFileEventFunctionsTest extends Specification {

    public static final Logger LOGGER = Logger.getLogger(AbstractFileEventFunctionsTest.name)

    @Rule
    TemporaryFolder tmpDir
    @Rule
    TestName testName
    @Rule
    JulLogging logging = new JulLogging(NativeLogger, CONFIG)

    def eventQueue = newEventQueue()
    File testDir
    File rootDir
    TestFileWatcher watcher
    List<Throwable> uncaughtFailureOnThread

    private Map<Pattern, Level> expectedLogMessages

    // We could do this with @Delegate, but Groovy doesn't let us :(
    protected FileWatcherFixture watcherFixture

    def setup() {
        watcherFixture = FileWatcherFixture.of(Platform.current())
        LOGGER.info(">>> Running '${testName.methodName}'")
        testDir = tmpDir.newFolder(testName.methodName).canonicalFile
        rootDir = new File(testDir, "root")
        assert rootDir.mkdirs()
        uncaughtFailureOnThread = []
        expectedLogMessages = [:]
    }

    def cleanup() {
        stopWatcher()
        LOGGER.info("<<< Finished '${testName.methodName}'")

        uncaughtFailureOnThread.each {
            it.printStackTrace()
        }
        // Avoid power assertion printing exceptions again
        Assert.that(uncaughtFailureOnThread.empty, "There were uncaught exceptions, see stacktraces above")

        // Check if the logs (INFO and above) match our expectations
        if (expectedLogMessages != null) {
            Map<String, Level> unexpectedLogMessages = logging.messages
                .findAll { message, level -> level.intValue() >= Level.INFO.intValue() }
            def remainingExpectedLogMessages = new LinkedHashMap<Pattern, Level>(expectedLogMessages)
            unexpectedLogMessages.removeAll { message, level ->
                remainingExpectedLogMessages.removeAll { expectedMessage, expectedLevel ->
                    expectedMessage.matcher(message).matches() && expectedLevel == level
                }
            }
            Assert.that(
                unexpectedLogMessages.isEmpty() && remainingExpectedLogMessages.isEmpty(),
                createLogMessageFailure(unexpectedLogMessages, remainingExpectedLogMessages)
            )
        }
    }

    private static String createLogMessageFailure(Map<String, Level> unexpectedLogMessages, LinkedHashMap<Pattern, Level> remainingExpectedLogMessages) {
        String failure = "Log messages differ from expected:\n"
        unexpectedLogMessages.each { message, level ->
            failure += " - UNEXPECTED $level $message\n"
        }
        remainingExpectedLogMessages.each { message, level ->
            failure += " - MISSING    $level $message\n"
        }
        return failure
    }

    void ignoreLogMessages() {
        expectedLogMessages = null
    }

    void expectLogMessage(Level level, String message) {
        expectLogMessage(level, Pattern.compile(Pattern.quote(message)))
    }

    void expectLogMessage(Level level, Pattern pattern) {
        expectedLogMessages.put(pattern, level)
    }

    enum FileWatcherFixture {
        MAC_OS(){
            private static final int LATENCY_IN_MILLIS = 0

            @Memoized
            @Override
            OsxFileEventFunctions getService() {
                Native.get(OsxFileEventFunctions)
            }

            @Override
            FileWatcher startNewWatcherInternal(FileWatcherCallback callback, boolean preventOverflow) {
                // Avoid setup operations to be reported
                waitForChangeEventLatency()
                service.startWatcher(
                    LATENCY_IN_MILLIS, TimeUnit.MILLISECONDS,
                    callback
                )
            }

            @Override
            void waitForChangeEventLatency() {
                TimeUnit.MILLISECONDS.sleep(LATENCY_IN_MILLIS + 50)
            }
        },
        LINUX(){
            @Memoized
            @Override
            LinuxFileEventFunctions getService() {
                Native.get(LinuxFileEventFunctions)
            }

            @Override
            FileWatcher startNewWatcherInternal(FileWatcherCallback callback, boolean preventOverflow) {
                // Avoid setup operations to be reported
                waitForChangeEventLatency()
                service.startWatcher(callback)
            }

            @Override
            void waitForChangeEventLatency() {
                TimeUnit.MILLISECONDS.sleep(50)
            }
        },
        WINDOWS(){
            @Memoized
            @Override
            WindowsFileEventFunctions getService() {
                Native.get(WindowsFileEventFunctions)
            }

            @Override
            FileWatcher startNewWatcherInternal(FileWatcherCallback callback, boolean preventOverflow) {
                int bufferSizeInKb
                if (preventOverflow) {
                    bufferSizeInKb = 16384
                    AbstractFileEventFunctionsTest.LOGGER.info("Using $bufferSizeInKb kByte buffer to prevent overflow events");
                } else {
                    bufferSizeInKb = 16
                }
                service.startWatcher(bufferSizeInKb * 1024, callback)
            }

            @Override
            void waitForChangeEventLatency() {
                Thread.sleep(50)
            }
        },
        UNSUPPORTED() {
            @Override
            AbstractFileEventFunctions getService() {
                throw new UnsupportedOperationException()
            }

            @Override
            FileWatcher startNewWatcherInternal(FileWatcherCallback callback, boolean preventOverflow) {
                throw new UnsupportedOperationException()
            }

            @Override
            void waitForChangeEventLatency() {
                throw new UnsupportedOperationException()
            }
        }

        static FileWatcherFixture of(Platform platform) {
            if (platform.macOs) {
                return MAC_OS
            } else if (platform.linux) {
                return LINUX
            } else if (platform.windows) {
                return WINDOWS
            } else {
                return UNSUPPORTED
            }
        }

        abstract AbstractFileEventFunctions getService()

        abstract FileWatcher startNewWatcherInternal(FileWatcherCallback callback, boolean preventOverflow)

        TestFileWatcher startNewWatcher(FileWatcherCallback callback) {
            new TestFileWatcher(startNewWatcherInternal(callback, false))
        }

        /**
         * Create a watcher that has a larger buffer to avoid overflow events happening during stress tests.
         * Overflow events are okay when we have lots of chagnes, but they make it impossible to test
         * other behavior we care about in stress tests.
         */
        TestFileWatcher startNewWatcherWithOverflowPrevention(FileWatcherCallback callback) {
            new TestFileWatcher(startNewWatcherInternal(callback, true))
        }

        abstract void waitForChangeEventLatency()
    }

    protected class RecordedEvent {
        final Type type
        final String path
        final Throwable failure

        RecordedEvent(Type type, String path, Throwable failure) {
            this.type = type
            this.path = path
            this.failure = failure
        }

        @Override
        String toString() {
            if (type == null) {
                return "FAILURE ${failure.message}"
            } else {
                return "$type ${shorten(path)}"
            }
        }
    }

    protected static BlockingQueue<RecordedEvent> newEventQueue() {
        new LinkedBlockingQueue<RecordedEvent>()
    }

    protected FileWatcherCallback newEventSinkCallback() {
        new TestCallback()
    }

    private class TestCallback implements FileWatcherCallback {
        @Override
        void pathChanged(Type type, String path) {
            LOGGER.info("> Received  $type ${shorten(path)}")
            if (path.empty) {
                throw new IllegalArgumentException("Empty path reported")
            }
            if (!new File(path).absolute) {
                throw new IllegalArgumentException("Relative path reported: ${path}")
            }
        }

        @Override
        void reportError(Throwable ex) {
            System.err.println("Caught exception from native side:")
            ex.printStackTrace()
            uncaughtFailureOnThread << ex
        }
    }

    private class QueuingCallback extends TestCallback {
        private final BlockingQueue<RecordedEvent> eventQueue

        QueuingCallback(BlockingQueue<RecordedEvent> eventQueue) {
            this.eventQueue = eventQueue
        }

        @Override
        void pathChanged(Type type, String path) {
            super.pathChanged(type, path)
            eventQueue.put(new RecordedEvent(type, path, null))
        }

        @Override
        void reportError(Throwable ex) {
            eventQueue.put(new RecordedEvent(null, null, ex))
        }
    }

    private interface ExpectedEvent {
        boolean matches(RecordedEvent event)
        boolean isOptional()
    }

    private class ExpectedChange implements ExpectedEvent {
        private final Type type
        private final File file
        final boolean optional

        ExpectedChange(Type type, File file, boolean optional) {
            this.type = type
            this.file = file
            this.optional = optional
        }

        @Override
        boolean matches(RecordedEvent event) {
            type == event.type && file.absolutePath == event.path
        }

        @Override
        String toString() {
            return "${optional ? "optional " : ""}$type ${shorten(file)}"
        }
    }

    private static class ExpectedFailure implements ExpectedEvent {
        private final Pattern message
        private final Class<? extends Throwable> type

        ExpectedFailure(Class<? extends Throwable> type, Pattern message) {
            this.type = type
            this.message = message
        }

        @Override
        boolean matches(RecordedEvent event) {
            event.type == Type.FAILURE \
                && type.isInstance(event.failure) \
                && message.matcher(event.failure.message).matches()
        }

        @Override
        boolean isOptional() {
            false
        }

        @Override
        String toString() {
            return "FAILURE /${message.pattern()}/"
        }
    }

    protected AbstractFileEventFunctions getService() {
        watcherFixture.service
    }

    protected void waitForChangeEventLatency() {
        watcherFixture.waitForChangeEventLatency()
    }

    protected void startWatcher(BlockingQueue<RecordedEvent> eventQueue = this.eventQueue, File... roots) {
        watcher = startNewWatcher(eventQueue, roots)
    }

    protected TestFileWatcher startNewWatcher(BlockingQueue<RecordedEvent> eventQueue = this.eventQueue, File... roots) {
        startNewWatcher(new QueuingCallback(eventQueue), roots)
    }

    protected TestFileWatcher startNewWatcher(FileWatcherCallback callback, File... roots) {
        def watcher = startNewWatcher(callback)
        watcher.startWatching(roots)
        return watcher
    }

    protected TestFileWatcher startNewWatcher(FileWatcherCallback callback) {
        watcherFixture.startNewWatcher(callback)
    }

    protected void stopWatcher() {
        def copyWatcher = watcher
        watcher = null
        copyWatcher?.close()
    }

    private void ensureNoMoreEvents(BlockingQueue<RecordedEvent> eventQueue = this.eventQueue) {
        def event = eventQueue.poll()
        if (event != null) {
            throw new RuntimeException("Unexpected event $event")
        }
    }

    protected void expectNoEvents(BlockingQueue<RecordedEvent> eventQueue = this.eventQueue) {
        // Let's make sure there are no events occurring,
        // and we don't just miss them because of timing
        waitForChangeEventLatency()
        ensureNoMoreEvents(eventQueue)
    }

    protected void expectEvents(BlockingQueue<RecordedEvent> eventQueue = this.eventQueue, int timeoutValue = 1, TimeUnit timeoutUnit = SECONDS, ExpectedEvent... events) {
        expectEvents(eventQueue, timeoutValue, timeoutUnit, events as List)
    }

    protected void expectEvents(BlockingQueue<RecordedEvent> eventQueue = this.eventQueue, int timeoutValue = 1, TimeUnit timeoutUnit = SECONDS, List<ExpectedEvent> events) {
        events.each { event ->
            LOGGER.info("> Expecting $event")
        }
        def expectedEvents = new ArrayList<ExpectedEvent>(events)
        expectEvents(
            eventQueue,
            timeoutValue,
            timeoutUnit,
            { !expectedEvents.empty },
            { event ->
                if (event == null) {
                    if (expectedEvents.every { it.optional }) {
                        return false
                    } else {
                        throw new TimeoutException("Did not receive events in $timeoutValue ${timeoutUnit.name().toLowerCase()}:\n- " + expectedEvents.join("\n- "))
                    }
                }
                def expectedEventIndex = expectedEvents.findIndexOf { expected ->
                    expected.matches(event)
                }
                if (expectedEventIndex == -1) {
                    throw new RuntimeException("Unexpected event $event")
                }
                expectedEvents.remove(expectedEventIndex)
                return true
            })
        ensureNoMoreEvents(eventQueue)
    }

    protected void expectEvents(
        BlockingQueue<RecordedEvent> eventQueue = this.eventQueue,
        int timeoutValue = 1,
        TimeUnit timeoutUnit = SECONDS,
        BooleanSupplier shouldContinue = { true },
        Predicate<RecordedEvent> eventHandler
    ) {
        long start = System.currentTimeMillis()
        long end = start + timeoutUnit.toMillis(timeoutValue)
        while (shouldContinue.asBoolean) {
            def current = System.currentTimeMillis()
            long timeout = end - current
            def event = eventQueue.poll(timeout, TimeUnit.MILLISECONDS)
            if (!eventHandler.test(event)) {
                break
            }
        }
    }

    private String shorten(File file) {
        shorten(file.absolutePath)
    }

    private String shorten(String path) {
        def prefix = testDir.absolutePath
        return path.startsWith(prefix + File.separator)
            ? "..." + path.substring(prefix.length())
            : path
    }

    protected ExpectedEvent change(Type type, File file) {
        new ExpectedChange(type, file, false)
    }

    protected ExpectedEvent optionalChange(Type type, File file) {
        return new ExpectedChange(type, file, true)
    }

    protected ExpectedEvent failure(Class<? extends Throwable> type = Exception, String message) {
        failure(type, Pattern.quote(message))
    }

    protected ExpectedEvent failure(Class<? extends Throwable> type = Exception, Pattern message) {
        return new ExpectedFailure(type, message)
    }

    protected void createNewFile(File file) {
        LOGGER.info("> Creating ${shorten(file)}")
        file.createNewFile()
        LOGGER.info("< Created ${shorten(file)}")
    }

    static class TestFileWatcher implements FileWatcher {
        @Delegate
        private final FileWatcher delegate

        TestFileWatcher(FileWatcher delegate) {
            this.delegate = delegate
        }

        void startWatching(File... paths) {
            delegate.startWatching(paths as List)
        }

        boolean stopWatching(File... paths) {
            delegate.stopWatching(paths as List)
        }
    }
}