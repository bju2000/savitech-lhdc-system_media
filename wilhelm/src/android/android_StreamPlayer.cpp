/*
 * Copyright (C) 2010 The Android Open Source Project
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

//#define USE_LOG SLAndroidLogLevel_Verbose

#include "sles_allinclusive.h"
#include "android_StreamPlayer.h"

#include <media/IStreamSource.h>
#include <media/IMediaPlayerService.h>
#include <media/stagefright/foundation/ADebug.h>

//--------------------------------------------------------------------------------------------------
// FIXME abstract out the diff between CMediaPlayer and CAudioPlayer

void android_StreamPlayer_realize_l(CAudioPlayer *ap, const notif_cbf_t cbf, void* notifUser) {
    SL_LOGV("android_StreamPlayer_realize_l(%p)", ap);

    AudioPlayback_Parameters ap_params;
    ap_params.sessionId = ap->mSessionId;
    ap_params.streamType = ap->mStreamType;
    ap_params.trackcb = NULL;
    ap_params.trackcbUser = NULL;
    android::StreamPlayer* splr = new android::StreamPlayer(&ap_params, false /*hasVideo*/);
    ap->mAPlayer = splr;
    splr->init(cbf, notifUser);
}


//--------------------------------------------------------------------------------------------------
namespace android {

StreamSourceAppProxy::StreamSourceAppProxy(
        const void* user, bool userIsAudioPlayer,
        void *context, const void *caller, const sp<CallbackProtector> &callbackProtector,
        const sp<StreamPlayer> &player) :
    mUser(user),
    mUserIsAudioPlayer(userIsAudioPlayer),
    mAndroidBufferQueue(NULL),
    mAppContext(context),
    mCaller(caller),
    mCallbackProtector(callbackProtector),
    mPlayer(player)
{
    SL_LOGV("StreamSourceAppProxy::StreamSourceAppProxy()");

    if (mUserIsAudioPlayer) {
        mAndroidBufferQueue = &((CAudioPlayer*)mUser)->mAndroidBufferQueue;
    } else {
        mAndroidBufferQueue = &((CMediaPlayer*)mUser)->mAndroidBufferQueue;
    }
}

StreamSourceAppProxy::~StreamSourceAppProxy() {
    SL_LOGD("StreamSourceAppProxy::~StreamSourceAppProxy()");
    mListener.clear();
    mBuffers.clear();
}

const SLuint32 StreamSourceAppProxy::kItemProcessed[NB_BUFFEREVENT_ITEM_FIELDS] = {
        SL_ANDROID_ITEMKEY_BUFFERQUEUEEVENT, // item key
        sizeof(SLuint32),                    // item size
        SL_ANDROIDBUFFERQUEUEEVENT_PROCESSED // item data
};

//--------------------------------------------------
// IStreamSource implementation
void StreamSourceAppProxy::setListener(const sp<IStreamListener> &listener) {
    Mutex::Autolock _l(mLock);
    mListener = listener;
}

void StreamSourceAppProxy::setBuffers(const Vector<sp<IMemory> > &buffers) {
    mBuffers = buffers;
}

void StreamSourceAppProxy::onBufferAvailable(size_t index) {
    //SL_LOGD("StreamSourceAppProxy::onBufferAvailable(%d)", index);

    CHECK_LT(index, mBuffers.size());
    sp<IMemory> mem = mBuffers.itemAt(index);
    SLAint64 length = (SLAint64) mem->size();

    {
        Mutex::Autolock _l(mLock);
        mAvailableBuffers.push_back(index);
    }
    //SL_LOGD("onBufferAvailable() now %d buffers available in queue", mAvailableBuffers.size());

    // a new shared mem buffer is available: let's try to fill immediately
    pullFromBuffQueue();
}

void StreamSourceAppProxy::receivedCmd_l(IStreamListener::Command cmd, const sp<AMessage> &msg) {
    if (mListener != 0) {
        mListener->issueCommand(cmd, false /* synchronous */, msg);
    }
}

void StreamSourceAppProxy::receivedBuffer_l(size_t buffIndex, size_t buffLength) {
    if (mListener != 0) {
        mListener->queueBuffer(buffIndex, buffLength);
    }
}

//--------------------------------------------------
// consumption from ABQ: pull from the ABQ, and push to shared memory (media server)
void StreamSourceAppProxy::pullFromBuffQueue() {

  if (android::CallbackProtector::enterCbIfOk(mCallbackProtector)) {

    size_t bufferId;
    void* bufferLoc;
    size_t buffSize;

    slAndroidBufferQueueCallback callback = NULL;
    void* pBufferContext, *pBufferData, *callbackPContext = NULL;
    AdvancedBufferHeader *oldFront = NULL;
    uint32_t dataSize /* , dataUsed */;

    // retrieve data from the buffer queue
    interface_lock_exclusive(mAndroidBufferQueue);

    if (mAndroidBufferQueue->mState.count != 0) {
        // SL_LOGD("nbBuffers in ABQ = %u, buffSize=%u",abq->mState.count, buffSize);
        assert(mAndroidBufferQueue->mFront != mAndroidBufferQueue->mRear);

        oldFront = mAndroidBufferQueue->mFront;
        AdvancedBufferHeader *newFront = &oldFront[1];

        // consume events when starting to read data from a buffer for the first time
        if (oldFront->mDataSizeConsumed == 0) {
            // note this code assumes at most one event per buffer; see IAndroidBufferQueue_Enqueue
            if (oldFront->mItems.mTsCmdData.mTsCmdCode & ANDROID_MP2TSEVENT_EOS) {
                receivedCmd_l(IStreamListener::EOS);
            } else if (oldFront->mItems.mTsCmdData.mTsCmdCode & ANDROID_MP2TSEVENT_DISCONTINUITY) {
                receivedCmd_l(IStreamListener::DISCONTINUITY);
            } else if (oldFront->mItems.mTsCmdData.mTsCmdCode & ANDROID_MP2TSEVENT_DISCON_NEWPTS) {
                sp<AMessage> msg = new AMessage();
                msg->setInt64(IStreamListener::kKeyResumeAtPTS,
                        (int64_t)oldFront->mItems.mTsCmdData.mPts);
                receivedCmd_l(IStreamListener::DISCONTINUITY, msg /*msg*/);
            } else if (oldFront->mItems.mTsCmdData.mTsCmdCode & ANDROID_MP2TSEVENT_FORMAT_CHANGE) {
                sp<AMessage> msg = new AMessage();
                // positive value for format change key makes the discontinuity "hard", see key def
                msg->setInt32(IStreamListener::kKeyFormatChange, (int32_t) 1);
                receivedCmd_l(IStreamListener::DISCONTINUITY, msg /*msg*/);
            }
            oldFront->mItems.mTsCmdData.mTsCmdCode = ANDROID_MP2TSEVENT_NONE;
        }

        {
            // we're going to change the shared mem buffer queue, so lock it
            Mutex::Autolock _l(mLock);
            if (!mAvailableBuffers.empty()) {
                bufferId = *mAvailableBuffers.begin();
                CHECK_LT(bufferId, mBuffers.size());
                sp<IMemory> mem = mBuffers.itemAt(bufferId);
                bufferLoc = mem->pointer();
                buffSize = mem->size();

                char *pSrc = ((char*)oldFront->mDataBuffer) + oldFront->mDataSizeConsumed;
                if (oldFront->mDataSizeConsumed + buffSize < oldFront->mDataSize) {
                    // more available than requested, copy as much as requested
                    // consume data: 1/ copy to given destination
                    memcpy(bufferLoc, pSrc, buffSize);
                    //               2/ keep track of how much has been consumed
                    oldFront->mDataSizeConsumed += buffSize;
                    //               3/ notify shared mem listener that new data is available
                    receivedBuffer_l(bufferId, buffSize);
                    mAvailableBuffers.erase(mAvailableBuffers.begin());
                } else {
                    // requested as much available or more: consume the whole of the current
                    //   buffer and move to the next
                    size_t consumed = oldFront->mDataSize - oldFront->mDataSizeConsumed;
                    //SL_LOGD("consuming rest of buffer: enqueueing=%u", consumed);
                    oldFront->mDataSizeConsumed = oldFront->mDataSize;

                    // move queue to next
                    if (newFront == &mAndroidBufferQueue->
                            mBufferArray[mAndroidBufferQueue->mNumBuffers + 1]) {
                        // reached the end, circle back
                        newFront = mAndroidBufferQueue->mBufferArray;
                    }
                    mAndroidBufferQueue->mFront = newFront;
                    mAndroidBufferQueue->mState.count--;
                    mAndroidBufferQueue->mState.index++;

                    if (consumed > 0) {
                        // consume data: 1/ copy to given destination
                        memcpy(bufferLoc, pSrc, consumed);
                        //               2/ keep track of how much has been consumed
                        // here nothing to do because we are done with this buffer
                        //               3/ notify StreamPlayer that new data is available
                        receivedBuffer_l(bufferId, consumed);
                        mAvailableBuffers.erase(mAvailableBuffers.begin());
                    }

                    // data has been consumed, and the buffer queue state has been updated
                    // we will notify the client if applicable
                    if (mAndroidBufferQueue->mCallbackEventsMask &
                            SL_ANDROIDBUFFERQUEUEEVENT_PROCESSED) {
                        callback = mAndroidBufferQueue->mCallback;
                        // save callback data while under lock
                        callbackPContext = mAndroidBufferQueue->mContext;
                        pBufferContext = (void *)oldFront->mBufferContext;
                        pBufferData    = (void *)oldFront->mDataBuffer;
                        dataSize       = oldFront->mDataSize;
                        // here a buffer is only dequeued when fully consumed
                        //dataUsed     = oldFront->mDataSizeConsumed;
                    }
                }
                //SL_LOGD("%d buffers available after enqueue", mAvailableBuffers.size());
                if (!mAvailableBuffers.empty()) {
                    // there is still room in the shared memory, recheck later if we can pull
                    // data from the buffer queue and write it to shared memory
                    mPlayer->queueRefilled();
                }
            }
        }


    } else { // empty queue
        SL_LOGD("ABQ empty, starving!");
        // signal we're at the end of the content, but don't pause (see note in function)
        if (mUserIsAudioPlayer) {
            // FIXME declare this external
            //audioPlayer_dispatch_headAtEnd_lockPlay((CAudioPlayer*)user,
            //        false /*set state to paused?*/, false);
        } else {
            // FIXME implement headAtEnd dispatch for CMediaPlayer
        }
    }

    interface_unlock_exclusive(mAndroidBufferQueue);

    // notify client
    if (NULL != callback) {
        SLresult result = (*callback)(&mAndroidBufferQueue->mItf, callbackPContext,
                pBufferContext, pBufferData, dataSize,
                dataSize, /* dataUsed  */
                // no messages during playback other than marking the buffer as processed
                (const SLAndroidBufferItem*)(&kItemProcessed) /* pItems */,
                NB_BUFFEREVENT_ITEM_FIELDS *sizeof(SLuint32) /* itemsLength */ );
        if (SL_RESULT_SUCCESS != result) {
            // Reserved for future use
            SL_LOGW("Unsuccessful result %d returned from AndroidBufferQueueCallback", result);
        }
    }

    mCallbackProtector->exitCb();
  } // enterCbIfOk
}


//--------------------------------------------------------------------------------------------------
StreamPlayer::StreamPlayer(AudioPlayback_Parameters* params, bool hasVideo) :
        GenericMediaPlayer(params, hasVideo),
        mAppProxy(0)
{
    SL_LOGD("StreamPlayer::StreamPlayer()");

    mPlaybackParams = *params;

}

StreamPlayer::~StreamPlayer() {
    SL_LOGD("StreamPlayer::~StreamPlayer()");

    mAppProxy.clear();
}


void StreamPlayer::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatPullFromAbq:
            onPullFromAndroidBufferQueue();
            break;

        default:
            GenericMediaPlayer::onMessageReceived(msg);
            break;
    }
}


void StreamPlayer::registerQueueCallback(
        const void* user, bool userIsAudioPlayer,
        void *context,
        const void *caller) {
    SL_LOGD("StreamPlayer::registerQueueCallback");
    Mutex::Autolock _l(mAppProxyLock);

    mAppProxy = new StreamSourceAppProxy(
            user, userIsAudioPlayer,
            context, caller, mCallbackProtector, this);

    CHECK(mAppProxy != 0);
    SL_LOGD("StreamPlayer::registerQueueCallback end");
}


/**
 * Asynchronously notify the player that the queue is ready to be pulled from.
 */
void StreamPlayer::queueRefilled() {
    // async notification that the ABQ was refilled: the player should pull from the ABQ, and
    //    and push to shared memory (to the media server)
    (new AMessage(kWhatPullFromAbq, id()))->post();
}


void StreamPlayer::appClear_l() {
    // the user of StreamPlayer has cleared its AndroidBufferQueue:
    // there's no clear() for the shared memory queue, so this is a no-op
}


//--------------------------------------------------
// Event handlers
void StreamPlayer::onPrepare() {
    SL_LOGD("StreamPlayer::onPrepare()");
    Mutex::Autolock _l(mAppProxyLock);
    if (mAppProxy != 0) {
        sp<IMediaPlayerService> mediaPlayerService(getMediaPlayerService());
        if (mediaPlayerService != NULL) {
            mPlayer = mediaPlayerService->create(getpid(), mPlayerClient /*IMediaPlayerClient*/,
                    mPlaybackParams.sessionId);
            if (mPlayer == NULL) {
                SL_LOGE("media player service failed to create player by app proxy");
            } else if (mPlayer->setDataSource(mAppProxy /*IStreamSource*/) != NO_ERROR) {
                SL_LOGE("setDataSource failed");
                mPlayer.clear();
            }
        }
    } else {
        SL_LOGE("Nothing to do here because there is no registered callback");
    }
    if (mPlayer == NULL) {
        mStateFlags |= kFlagPreparedUnsuccessfully;
    }
    GenericMediaPlayer::onPrepare();
    SL_LOGD("StreamPlayer::onPrepare() done");
}


void StreamPlayer::onPlay() {
    SL_LOGD("StreamPlayer::onPlay()");
    // enqueue a message that will cause StreamAppProxy to consume from the queue (again if the
    // player had starved the shared memory)
    queueRefilled();

    GenericMediaPlayer::onPlay();
}


void StreamPlayer::onPullFromAndroidBufferQueue() {
    //SL_LOGD("StreamPlayer::onQueueRefilled()");
    Mutex::Autolock _l(mAppProxyLock);
    if (mAppProxy != 0) {
        mAppProxy->pullFromBuffQueue();
    }
}

} // namespace android
