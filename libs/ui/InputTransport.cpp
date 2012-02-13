//
// Copyright 2010 The Android Open Source Project
//
// Provides a shared memory transport for input events.
//
#define LOG_TAG "InputTransport"

//#define LOG_NDEBUG 0

// Log debug messages about channel messages (send message, receive message)
#define DEBUG_CHANNEL_MESSAGES 0

// Log debug messages whenever InputChannel objects are created/destroyed
#define DEBUG_CHANNEL_LIFECYCLE 0

// Log debug messages about transport actions
#define DEBUG_TRANSPORT_ACTIONS 0


#include <cutils/log.h>
#include <errno.h>
#include <fcntl.h>
#include <ui/InputTransport.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>


namespace android {

// Socket buffer size.  The default is typically about 128KB, which is much larger than
// we really need.  So we make it smaller.  It just needs to be big enough to hold
// a few dozen large multi-finger motion events in the case where an application gets
// behind processing touches.
static const size_t SOCKET_BUFFER_SIZE = 32 * 1024;


// --- InputMessage ---

bool InputMessage::isValid(size_t actualSize) const {
    if (size() == actualSize) {
        switch (header.type) {
        case TYPE_KEY:
            return true;
        case TYPE_MOTION:
            return body.motion.pointerCount > 0
                    && body.motion.pointerCount <= MAX_POINTERS;
        case TYPE_FINISHED:
            return true;
        }
    }
    return false;
}

size_t InputMessage::size() const {
    switch (header.type) {
    case TYPE_KEY:
        return sizeof(Header) + body.key.size();
    case TYPE_MOTION:
        return sizeof(Header) + body.motion.size();
    case TYPE_FINISHED:
        return sizeof(Header) + body.finished.size();
    }
    return sizeof(Header);
}


// --- InputChannel ---

InputChannel::InputChannel(const String8& name, int fd) :
        mName(name), mFd(fd) {
#if DEBUG_CHANNEL_LIFECYCLE
    ALOGD("Input channel constructed: name='%s', fd=%d",
            mName.string(), fd);
#endif

    int result = fcntl(mFd, F_SETFL, O_NONBLOCK);
    LOG_ALWAYS_FATAL_IF(result != 0, "channel '%s' ~ Could not make socket "
            "non-blocking.  errno=%d", mName.string(), errno);
}

InputChannel::~InputChannel() {
#if DEBUG_CHANNEL_LIFECYCLE
    ALOGD("Input channel destroyed: name='%s', fd=%d",
            mName.string(), mFd);
#endif

    ::close(mFd);
}

status_t InputChannel::openInputChannelPair(const String8& name,
        sp<InputChannel>& outServerChannel, sp<InputChannel>& outClientChannel) {
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets)) {
        status_t result = -errno;
        ALOGE("channel '%s' ~ Could not create socket pair.  errno=%d",
                name.string(), errno);
        outServerChannel.clear();
        outClientChannel.clear();
        return result;
    }

    int bufferSize = SOCKET_BUFFER_SIZE;
    setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize));
    setsockopt(sockets[0], SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize));
    setsockopt(sockets[1], SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize));
    setsockopt(sockets[1], SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize));

    String8 serverChannelName = name;
    serverChannelName.append(" (server)");
    outServerChannel = new InputChannel(serverChannelName, sockets[0]);

    String8 clientChannelName = name;
    clientChannelName.append(" (client)");
    outClientChannel = new InputChannel(clientChannelName, sockets[1]);
    return OK;
}

status_t InputChannel::sendMessage(const InputMessage* msg) {
    size_t msgLength = msg->size();
    ssize_t nWrite;
    do {
        nWrite = ::send(mFd, msg, msgLength, MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (nWrite == -1 && errno == EINTR);

    if (nWrite < 0) {
        int error = errno;
#if DEBUG_CHANNEL_MESSAGES
        ALOGD("channel '%s' ~ error sending message of type %d, errno=%d", mName.string(),
                msg->header.type, error);
#endif
        if (error == EAGAIN || error == EWOULDBLOCK) {
            return WOULD_BLOCK;
        }
        if (error == EPIPE || error == ENOTCONN) {
            return DEAD_OBJECT;
        }
        return -error;
    }

    if (size_t(nWrite) != msgLength) {
#if DEBUG_CHANNEL_MESSAGES
        ALOGD("channel '%s' ~ error sending message type %d, send was incomplete",
                mName.string(), msg->header.type);
#endif
        return DEAD_OBJECT;
    }

#if DEBUG_CHANNEL_MESSAGES
    ALOGD("channel '%s' ~ sent message of type %d", mName.string(), msg->header.type);
#endif
    return OK;
}

status_t InputChannel::receiveMessage(InputMessage* msg) {
    ssize_t nRead;
    do {
        nRead = ::recv(mFd, msg, sizeof(InputMessage), MSG_DONTWAIT);
    } while (nRead == -1 && errno == EINTR);

    if (nRead < 0) {
        int error = errno;
#if DEBUG_CHANNEL_MESSAGES
        ALOGD("channel '%s' ~ receive message failed, errno=%d", mName.string(), errno);
#endif
        if (error == EAGAIN || error == EWOULDBLOCK) {
            return WOULD_BLOCK;
        }
        if (error == EPIPE || error == ENOTCONN) {
            return DEAD_OBJECT;
        }
        return -error;
    }

    if (nRead == 0) { // check for EOF
#if DEBUG_CHANNEL_MESSAGES
        ALOGD("channel '%s' ~ receive message failed because peer was closed", mName.string());
#endif
        return DEAD_OBJECT;
    }

    if (!msg->isValid(nRead)) {
#if DEBUG_CHANNEL_MESSAGES
        ALOGD("channel '%s' ~ received invalid message", mName.string());
#endif
        return BAD_VALUE;
    }

#if DEBUG_CHANNEL_MESSAGES
    ALOGD("channel '%s' ~ received message of type %d", mName.string(), msg->header.type);
#endif
    return OK;
}


// --- InputPublisher ---

InputPublisher::InputPublisher(const sp<InputChannel>& channel) :
        mChannel(channel) {
}

InputPublisher::~InputPublisher() {
}

status_t InputPublisher::publishKeyEvent(
        uint32_t seq,
        int32_t deviceId,
        int32_t source,
        int32_t action,
        int32_t flags,
        int32_t keyCode,
        int32_t scanCode,
        int32_t metaState,
        int32_t repeatCount,
        nsecs_t downTime,
        nsecs_t eventTime) {
#if DEBUG_TRANSPORT_ACTIONS
    ALOGD("channel '%s' publisher ~ publishKeyEvent: seq=%u, deviceId=%d, source=0x%x, "
            "action=0x%x, flags=0x%x, keyCode=%d, scanCode=%d, metaState=0x%x, repeatCount=%d,"
            "downTime=%lld, eventTime=%lld",
            mChannel->getName().string(), seq,
            deviceId, source, action, flags, keyCode, scanCode, metaState, repeatCount,
            downTime, eventTime);
#endif

    if (!seq) {
        ALOGE("Attempted to publish a key event with sequence number 0.");
        return BAD_VALUE;
    }

    InputMessage msg;
    msg.header.type = InputMessage::TYPE_KEY;
    msg.body.key.seq = seq;
    msg.body.key.deviceId = deviceId;
    msg.body.key.source = source;
    msg.body.key.action = action;
    msg.body.key.flags = flags;
    msg.body.key.keyCode = keyCode;
    msg.body.key.scanCode = scanCode;
    msg.body.key.metaState = metaState;
    msg.body.key.repeatCount = repeatCount;
    msg.body.key.downTime = downTime;
    msg.body.key.eventTime = eventTime;
    return mChannel->sendMessage(&msg);
}

status_t InputPublisher::publishMotionEvent(
        uint32_t seq,
        int32_t deviceId,
        int32_t source,
        int32_t action,
        int32_t flags,
        int32_t edgeFlags,
        int32_t metaState,
        int32_t buttonState,
        float xOffset,
        float yOffset,
        float xPrecision,
        float yPrecision,
        nsecs_t downTime,
        nsecs_t eventTime,
        size_t pointerCount,
        const PointerProperties* pointerProperties,
        const PointerCoords* pointerCoords) {
#if DEBUG_TRANSPORT_ACTIONS
    ALOGD("channel '%s' publisher ~ publishMotionEvent: seq=%u, deviceId=%d, source=0x%x, "
            "action=0x%x, flags=0x%x, edgeFlags=0x%x, metaState=0x%x, buttonState=0x%x, "
            "xOffset=%f, yOffset=%f, "
            "xPrecision=%f, yPrecision=%f, downTime=%lld, eventTime=%lld, "
            "pointerCount=%d",
            mChannel->getName().string(), seq,
            deviceId, source, action, flags, edgeFlags, metaState, buttonState,
            xOffset, yOffset, xPrecision, yPrecision, downTime, eventTime, pointerCount);
#endif

    if (!seq) {
        ALOGE("Attempted to publish a motion event with sequence number 0.");
        return BAD_VALUE;
    }

    if (pointerCount > MAX_POINTERS || pointerCount < 1) {
        ALOGE("channel '%s' publisher ~ Invalid number of pointers provided: %d.",
                mChannel->getName().string(), pointerCount);
        return BAD_VALUE;
    }

    InputMessage msg;
    msg.header.type = InputMessage::TYPE_MOTION;
    msg.body.motion.seq = seq;
    msg.body.motion.deviceId = deviceId;
    msg.body.motion.source = source;
    msg.body.motion.action = action;
    msg.body.motion.flags = flags;
    msg.body.motion.edgeFlags = edgeFlags;
    msg.body.motion.metaState = metaState;
    msg.body.motion.buttonState = buttonState;
    msg.body.motion.xOffset = xOffset;
    msg.body.motion.yOffset = yOffset;
    msg.body.motion.xPrecision = xPrecision;
    msg.body.motion.yPrecision = yPrecision;
    msg.body.motion.downTime = downTime;
    msg.body.motion.eventTime = eventTime;
    msg.body.motion.pointerCount = pointerCount;
    for (size_t i = 0; i < pointerCount; i++) {
        msg.body.motion.pointers[i].properties.copyFrom(pointerProperties[i]);
        msg.body.motion.pointers[i].coords.copyFrom(pointerCoords[i]);
    }
    return mChannel->sendMessage(&msg);
}

status_t InputPublisher::receiveFinishedSignal(uint32_t* outSeq, bool* outHandled) {
#if DEBUG_TRANSPORT_ACTIONS
    ALOGD("channel '%s' publisher ~ receiveFinishedSignal",
            mChannel->getName().string());
#endif

    InputMessage msg;
    status_t result = mChannel->receiveMessage(&msg);
    if (result) {
        *outSeq = 0;
        *outHandled = false;
        return result;
    }
    if (msg.header.type != InputMessage::TYPE_FINISHED) {
        ALOGE("channel '%s' publisher ~ Received unexpected message of type %d from consumer",
                mChannel->getName().string(), msg.header.type);
        return UNKNOWN_ERROR;
    }
    *outSeq = msg.body.finished.seq;
    *outHandled = msg.body.finished.handled;
    return OK;
}

// --- InputConsumer ---

InputConsumer::InputConsumer(const sp<InputChannel>& channel) :
        mChannel(channel), mDeferredEventSeq(0) {
}

InputConsumer::~InputConsumer() {
}

status_t InputConsumer::consume(InputEventFactoryInterface* factory,
        bool consumeBatches, uint32_t* outSeq, InputEvent** outEvent) {
#if DEBUG_TRANSPORT_ACTIONS
    ALOGD("channel '%s' consumer ~ consume: consumeBatches=%s",
            mChannel->getName().string(), consumeBatches ? "true" : "false");
#endif

    *outSeq = 0;
    *outEvent = NULL;

    // Report deferred event first, if we had to end a batch earlier than we expected
    // during the previous time consume was called.
    if (mDeferredEventSeq) {
        MotionEvent* motionEvent = factory->createMotionEvent();
        if (! motionEvent) return NO_MEMORY;

        motionEvent->copyFrom(&mDeferredEvent, true /*keepHistory*/);
        *outSeq = mDeferredEventSeq;
        *outEvent = motionEvent;
        mDeferredEventSeq = 0;
#if DEBUG_TRANSPORT_ACTIONS
        ALOGD("channel '%s' consumer ~ consumed deferred event, seq=%u",
                mChannel->getName().string(), *outSeq);
#endif
        return OK;
    }

    // Fetch the next input message.
    // Loop until an event can be returned or no additional events are received.
    while (!*outEvent) {
        InputMessage msg;
        status_t result = mChannel->receiveMessage(&msg);
        if (result) {
            // Consume the next batched event unless batches are being held for later.
            if (!mBatches.isEmpty() && (consumeBatches || result != WOULD_BLOCK)) {
                MotionEvent* motionEvent = factory->createMotionEvent();
                if (! motionEvent) return NO_MEMORY;

                const Batch& batch = mBatches.top();
                motionEvent->copyFrom(&batch.event, true /*keepHistory*/);
                *outSeq = batch.seq;
                *outEvent = motionEvent;
                mBatches.pop();
#if DEBUG_TRANSPORT_ACTIONS
                ALOGD("channel '%s' consumer ~ consumed batch event, seq=%u",
                        mChannel->getName().string(), *outSeq);
#endif
                break;
            }
            return result;
        }

        switch (msg.header.type) {
        case InputMessage::TYPE_KEY: {
            KeyEvent* keyEvent = factory->createKeyEvent();
            if (!keyEvent) return NO_MEMORY;

            initializeKeyEvent(keyEvent, &msg);
            *outSeq = msg.body.key.seq;
            *outEvent = keyEvent;
#if DEBUG_TRANSPORT_ACTIONS
            ALOGD("channel '%s' consumer ~ consumed key event, seq=%u",
                    mChannel->getName().string(), *outSeq);
#endif
            break;
        }

        case AINPUT_EVENT_TYPE_MOTION: {
            ssize_t batchIndex = findBatch(msg.body.motion.deviceId, msg.body.motion.source);
            if (batchIndex >= 0) {
                Batch& batch = mBatches.editItemAt(batchIndex);
                if (canAppendSamples(&batch.event, &msg)) {
                    // Send finished message for the earlier part of the batch.
                    // Claim that we handled the event.  (The dispatcher doesn't care either
                    // way at the moment.)
                    status_t status = sendFinishedSignal(batch.seq, true);
                    if (status) {
                        return status;
                    }

                    // Append to the batch and save the new sequence number for the tail end.
                    appendSamples(&batch.event, &msg);
                    batch.seq = msg.body.motion.seq;
#if DEBUG_TRANSPORT_ACTIONS
                    ALOGD("channel '%s' consumer ~ appended to batch event",
                            mChannel->getName().string());
#endif
                    break;
                } else {
                    MotionEvent* motionEvent = factory->createMotionEvent();
                    if (! motionEvent) return NO_MEMORY;

                    // We cannot append to the batch in progress, so we need to consume
                    // the previous batch right now and defer the new event until later.
                    mDeferredEventSeq = msg.body.motion.seq;
                    initializeMotionEvent(&mDeferredEvent, &msg);

                    // Return the end of the previous batch.
                    motionEvent->copyFrom(&batch.event, true /*keepHistory*/);
                    *outSeq = batch.seq;
                    *outEvent = motionEvent;
                    mBatches.removeAt(batchIndex);
#if DEBUG_TRANSPORT_ACTIONS
                    ALOGD("channel '%s' consumer ~ consumed batch event and "
                            "deferred current event, seq=%u",
                            mChannel->getName().string(), *outSeq);
#endif
                    break;
                }
            }

            // Start a new batch if needed.
            if (msg.body.motion.action == AMOTION_EVENT_ACTION_MOVE
                    || msg.body.motion.action == AMOTION_EVENT_ACTION_HOVER_MOVE) {
                mBatches.push();
                Batch& batch = mBatches.editTop();
                batch.seq = msg.body.motion.seq;
                initializeMotionEvent(&batch.event, &msg);
#if DEBUG_TRANSPORT_ACTIONS
                ALOGD("channel '%s' consumer ~ started batch event",
                        mChannel->getName().string());
#endif
                break;
            }

            MotionEvent* motionEvent = factory->createMotionEvent();
            if (! motionEvent) return NO_MEMORY;

            initializeMotionEvent(motionEvent, &msg);
            *outSeq = msg.body.motion.seq;
            *outEvent = motionEvent;
#if DEBUG_TRANSPORT_ACTIONS
            ALOGD("channel '%s' consumer ~ consumed motion event, seq=%u",
                    mChannel->getName().string(), *outSeq);
#endif
            break;
        }

        default:
            ALOGE("channel '%s' consumer ~ Received unexpected message of type %d",
                    mChannel->getName().string(), msg.header.type);
            return UNKNOWN_ERROR;
        }
    }
    return OK;
}

status_t InputConsumer::sendFinishedSignal(uint32_t seq, bool handled) {
#if DEBUG_TRANSPORT_ACTIONS
    ALOGD("channel '%s' consumer ~ sendFinishedSignal: seq=%u, handled=%s",
            mChannel->getName().string(), seq, handled ? "true" : "false");
#endif

    if (!seq) {
        ALOGE("Attempted to send a finished signal with sequence number 0.");
        return BAD_VALUE;
    }

    InputMessage msg;
    msg.header.type = InputMessage::TYPE_FINISHED;
    msg.body.finished.seq = seq;
    msg.body.finished.handled = handled;
    return mChannel->sendMessage(&msg);
}

bool InputConsumer::hasPendingBatch() const {
    return !mBatches.isEmpty();
}

ssize_t InputConsumer::findBatch(int32_t deviceId, int32_t source) const {
    for (size_t i = 0; i < mBatches.size(); i++) {
        const Batch& batch = mBatches.itemAt(i);
        if (batch.event.getDeviceId() == deviceId && batch.event.getSource() == source) {
            return i;
        }
    }
    return -1;
}

void InputConsumer::initializeKeyEvent(KeyEvent* event, const InputMessage* msg) {
    event->initialize(
            msg->body.key.deviceId,
            msg->body.key.source,
            msg->body.key.action,
            msg->body.key.flags,
            msg->body.key.keyCode,
            msg->body.key.scanCode,
            msg->body.key.metaState,
            msg->body.key.repeatCount,
            msg->body.key.downTime,
            msg->body.key.eventTime);
}

void InputConsumer::initializeMotionEvent(MotionEvent* event, const InputMessage* msg) {
    size_t pointerCount = msg->body.motion.pointerCount;
    PointerProperties pointerProperties[pointerCount];
    PointerCoords pointerCoords[pointerCount];
    for (size_t i = 0; i < pointerCount; i++) {
        pointerProperties[i].copyFrom(msg->body.motion.pointers[i].properties);
        pointerCoords[i].copyFrom(msg->body.motion.pointers[i].coords);
    }

    event->initialize(
            msg->body.motion.deviceId,
            msg->body.motion.source,
            msg->body.motion.action,
            msg->body.motion.flags,
            msg->body.motion.edgeFlags,
            msg->body.motion.metaState,
            msg->body.motion.buttonState,
            msg->body.motion.xOffset,
            msg->body.motion.yOffset,
            msg->body.motion.xPrecision,
            msg->body.motion.yPrecision,
            msg->body.motion.downTime,
            msg->body.motion.eventTime,
            pointerCount,
            pointerProperties,
            pointerCoords);
}

bool InputConsumer::canAppendSamples(const MotionEvent* event, const InputMessage *msg) {
    size_t pointerCount = msg->body.motion.pointerCount;
    if (event->getPointerCount() != pointerCount
            || event->getAction() != msg->body.motion.action) {
        return false;
    }
    for (size_t i = 0; i < pointerCount; i++) {
        if (*event->getPointerProperties(i) != msg->body.motion.pointers[i].properties) {
            return false;
        }
    }
    return true;
}

void InputConsumer::appendSamples(MotionEvent* event, const InputMessage* msg) {
    size_t pointerCount = msg->body.motion.pointerCount;
    PointerCoords pointerCoords[pointerCount];
    for (size_t i = 0; i < pointerCount; i++) {
        pointerCoords[i].copyFrom(msg->body.motion.pointers[i].coords);
    }

    event->setMetaState(event->getMetaState() | msg->body.motion.metaState);
    event->addSample(msg->body.motion.eventTime, pointerCoords);
}

} // namespace android
