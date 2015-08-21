//  Natron
//
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/*
 * Created by Alexandre GAUTHIER-FOICHAT on 6/1/2012.
 * contact: immarespond at gmail dot com
 *
 */

// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>

#include "OutputSchedulerThread.h"

#include <iostream>
#include <set>
#include <list>
#include <algorithm> // min, max

#include <QMetaType>
#include <QMutex>
#include <QWaitCondition>
#include <QCoreApplication>
#include <QString>
#include <QThreadPool>
#include <QDebug>
#include <QtConcurrentRun>
#include <QFuture>
#include <QFutureWatcher>
#include <QRunnable>
#include <QTextStream>

#include "Global/MemoryInfo.h"

#include "Engine/AppManager.h"
#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/Node.h"
#include "Engine/OpenGLViewerI.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"
#include "Engine/Timer.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewerInstance.h"
#include "Engine/ViewerInstancePrivate.h"

#define NATRON_FPS_REFRESH_RATE_SECONDS 1.5


using namespace Natron;



///Sort the frames by time and then by view
struct BufferedFrameCompare_less
{
    bool operator()(const BufferedFrame& lhs,const BufferedFrame& rhs) const
    {
        if (lhs.time < rhs.time) {
            return true;
        } else if (lhs.time > rhs.time) {
            return false;
        } else {
            if (lhs.view < rhs.view) {
                return true;
            } else if (lhs.view > rhs.view) {
                return false;
            } else {
                if (lhs.frame && rhs.frame) {
                    if (lhs.frame->getUniqueID() < rhs.frame->getUniqueID()) {
                        return true;
                    } else if (lhs.frame->getUniqueID() > rhs.frame->getUniqueID()) {
                        return false;
                    } else {
                        return false;
                    }
                } else {
                    return false;
                }
            }
        }
    }
};

typedef std::set< BufferedFrame , BufferedFrameCompare_less > FrameBuffer;


namespace {
    class MetaTypesRegistration
    {
    public:
        inline MetaTypesRegistration()
        {
            qRegisterMetaType<BufferedFrames>("BufferedFrames");
            qRegisterMetaType<BufferableObjectList>("BufferableObjectList");
        }
    };
}
static MetaTypesRegistration registration;


struct RunArgs
{
    ///The frame range that the scheduler should render
    int firstFrame,lastFrame;
    
    /// the timelineDirection represents the direction the timeline should move to
    OutputSchedulerThread::RenderDirectionEnum timelineDirection;

};

struct RenderThread {
    RenderThreadTask* thread;
    bool active;
};
typedef std::list<RenderThread> RenderThreads;

// Struct used in a queue when rendering the current frame with a viewer, the id is meaningless just to have a member
// in the structure. We then compare the pointer of this struct
struct RequestedFrame
{
    int id;
};

struct ProducedFrame
{
    BufferableObjectList frames;
    boost::shared_ptr<RequestedFrame> request;
    bool processRequest;
};

struct OutputSchedulerThreadPrivate
{
    
    FrameBuffer buf; //the frames rendered by the worker threads that needs to be rendered in order by the output device
    QWaitCondition bufCondition;
    mutable QMutex bufMutex;
    
    bool working; // true when the scheduler is currently having render threads doing work
    mutable QMutex workingMutex;
    
    bool hasQuit; //true when the thread has exited and shouldn't be restarted.
    bool mustQuit; //true when the thread must exit
    QWaitCondition mustQuitCond;
    QMutex mustQuitMutex;
    
    int startRequests;
    QWaitCondition startRequestsCond;
    QMutex startRequestsMutex;
    
    int abortRequested; // true when the user wants to stop the engine, e.g: the user disconnected the viewer
    bool isAbortRequestBlocking;
    QWaitCondition abortedRequestedCondition;
    QMutex abortedRequestedMutex; // protects abortRequested
    
    bool abortFlag; // Same as abortRequested > 0 but held by a mutex on a smaller scope.
    mutable QMutex abortFlagMutex; // protects abortFlag

    
    QMutex abortBeingProcessedMutex; //protects abortBeingProcessed
    
    ///Basically when calling stopRender() we are resetting the abortRequested flag and putting the scheduler thread(this)
    ///asleep. We don't want that another thread attemps to post an abort request at the same time.
    bool abortBeingProcessed;
    
    bool processRunning; //true when the scheduler is actively "processing" a frame (i.e: updating the viewer or writing in a file on disk)
    QWaitCondition processCondition;
    QMutex processMutex;

    //doesn't need any protection since it never changes and is set in the constructor
    OutputSchedulerThread::ProcessFrameModeEnum mode; //is the frame to be processed on the main-thread (i.e OpenGL rendering) or on the scheduler thread

    
    boost::scoped_ptr<Timer> timer; // Timer regulating the engine execution. It is controlled by the GUI and MT-safe.
    
    
    ///The idea here is that the render() function will set the requestedRunArgs, and once the scheduler has finished
    ///the previous render it will copy them to the livingRunArgs to fullfil the new render request
    RunArgs requestedRunArgs,livingRunArgs;
    
    ///When the render threads are not using the appendToBuffer API, the scheduler has no way to know the rendering is finished
    ///but to count the number of frames rendered via notifyFrameRended which is called by the render thread.
    U64 nFramesRendered;
    bool renderFinished; //< set to true when nFramesRendered = livingRunArgs.lastFrame - livingRunArgs.firstFrame + 1
    
    QMutex runArgsMutex; // protects requestedRunArgs & livingRunArgs & nFramesRendered
    

    ///Worker threads
    mutable QMutex renderThreadsMutex;
    RenderThreads renderThreads;
    QWaitCondition allRenderThreadsInactiveCond; // wait condition to make sure all render threads are asleep
    QWaitCondition allRenderThreadsQuitCond; //to make sure all render threads have quit
    
    ///Work queue filled by the scheduler thread when in playback/render on disk
    QMutex framesToRenderMutex; // protects framesToRender & currentFrameRequests
    std::list<int> framesToRender;
    
    ///index of the last frame pushed (framesToRender.back())
    ///we store this because when we call pushFramesToRender we need to know what was the last frame that was queued
    ///Protected by framesToRenderMutex
    int lastFramePushedIndex;
    
    ///Render threads wait in this condition and the scheduler wake them when it needs to render some frames
    QWaitCondition framesToRenderNotEmptyCond;

    
    Natron::OutputEffectInstance* outputEffect; //< The effect used as output device
    RenderEngine* engine;

    bool runningCallback;
    QMutex runningCallbackMutex;
    QWaitCondition runningCallbackCond;
    
    OutputSchedulerThreadPrivate(RenderEngine* engine,Natron::OutputEffectInstance* effect,OutputSchedulerThread::ProcessFrameModeEnum mode)
    : buf()
    , bufCondition()
    , bufMutex()
    , working(false)
    , workingMutex()
    , hasQuit(false)
    , mustQuit(false)
    , mustQuitCond()
    , mustQuitMutex()
    , startRequests(0)
    , startRequestsCond()
    , startRequestsMutex()
    , abortRequested(0)
    , isAbortRequestBlocking(false)
    , abortedRequestedCondition()
    , abortedRequestedMutex()
    , abortFlag(false)
    , abortFlagMutex()
    , abortBeingProcessedMutex()
    , abortBeingProcessed(false)
    , processRunning(false)
    , processCondition()
    , processMutex()
    , mode(mode)
    , timer(new Timer)
    , requestedRunArgs()
    , livingRunArgs()
    , nFramesRendered(0)
    , renderFinished(false)
    , runArgsMutex()
    , renderThreadsMutex()
    , renderThreads()
    , allRenderThreadsInactiveCond()
    , allRenderThreadsQuitCond()
    , framesToRenderMutex()
    , framesToRender()
    , lastFramePushedIndex(0)
    , framesToRenderNotEmptyCond()
    , outputEffect(effect)
    , engine(engine)
    , runningCallback(false)
    , runningCallbackMutex()
    , runningCallbackCond()
    {
       
    }
    
    bool appendBufferedFrame(double time,
                             int view,
                             const boost::shared_ptr<TimeLapse>& timeRecorder,
                             const boost::shared_ptr<BufferableObject>& image) WARN_UNUSED_RETURN
    {
        ///Private, shouldn't lock
        assert(!bufMutex.tryLock());
        
        BufferedFrame k;
        k.time = time;
        k.view = view;
        k.frame = image;
        k.timeRecorder = timeRecorder;
        std::pair<FrameBuffer::iterator,bool> ret = buf.insert(k);
        return ret.second;
    }
    
    void getFromBufferAndErase(double time,BufferedFrames& frames)
    {
        
        ///Private, shouldn't lock
        assert(!bufMutex.tryLock());
        
        FrameBuffer newBuf;
        for (FrameBuffer::iterator it = buf.begin(); it != buf.end(); ++it) {
            
            if (it->time == time) {
                if (it->frame) {
                    frames.push_back(*it);
                }
            } else {
                newBuf.insert(*it);
            }
        }
        buf = newBuf;
    }
    
    void clearBuffer()
    {
        ///Private, shouldn't lock
        assert(!bufMutex.tryLock());
        
        buf.clear();
    }
    
    void appendRunnable(RenderThreadTask* runnable)
    {
        RenderThread r;
        r.thread = runnable;
        r.active = true;
        renderThreads.push_back(r);
        runnable->start();
        
    }
    
    RenderThreads::iterator getRunnableIterator(RenderThreadTask* runnable)
    {
        ///Private shouldn't lock
        assert(!renderThreadsMutex.tryLock());
        for (RenderThreads::iterator it = renderThreads.begin(); it != renderThreads.end(); ++it) {
            if (it->thread == runnable) {
                return it;
            }
        }
        return renderThreads.end();
    }
    

    int getNBufferedFrames() const {
        QMutexLocker l(&bufMutex);
        return buf.size();
    }
    
    static bool getNextFrameInSequence(PlaybackModeEnum pMode,
                                       OutputSchedulerThread::RenderDirectionEnum direction,
                                       int frame,
                                       int firstFrame,
                                       int lastFrame,
                                       int* nextFrame,
                                       OutputSchedulerThread::RenderDirectionEnum* newDirection);
    
    static void getNearestInSequence(OutputSchedulerThread::RenderDirectionEnum direction,
                                     int frame,
                                     int firstFrame,
                                     int lastFrame,
                                     int* nextFrame);
    
    /**
     * @brief Checks if mustQuit has been set to true, if so then it will return true and the scheduler thread should stop
     **/
    bool checkForExit()
    {
        QMutexLocker l(&mustQuitMutex);
        if (mustQuit) {
            mustQuit = false;
            mustQuitCond.wakeOne();
            return true;
        }
        
        return false;
    }
    
    void waitForRenderThreadsToBeDone() {
        
        assert( !renderThreadsMutex.tryLock() );
        while (renderThreads.size() > 0 && getNActiveRenderThreads() > 0) {
            allRenderThreadsInactiveCond.wait(&renderThreadsMutex);
        }
    }
    
    int getNActiveRenderThreads() const {
        ///Private shouldn't lock
        assert( !renderThreadsMutex.tryLock() );
        int ret = 0;
        for (RenderThreads::const_iterator it = renderThreads.begin(); it != renderThreads.end(); ++it) {
            if (it->active) {
                ++ret;
            }
        }
        return ret;
    }
    
    void removeQuitRenderThreadsInternal()
    {
        for (;;) {
            bool hasRemoved = false;
            for (RenderThreads::iterator it = renderThreads.begin(); it != renderThreads.end(); ++it) {
                if (it->thread->hasQuit()) {
                    it->thread->deleteLater();
                    renderThreads.erase(it);
                    hasRemoved = true;
                    break;
                }
            }
            
            if (!hasRemoved) {
                break;
            }
        }
    }
    
    void removeAllQuitRenderThreads() {
        ///Private shouldn't lock
        assert(!renderThreadsMutex.tryLock());
        
        removeQuitRenderThreadsInternal();
        
        ///Wake-up the main-thread if it was waiting for all threads to quit
        allRenderThreadsQuitCond.wakeOne();
    }
    
    void waitForRenderThreadsToQuit() {
    
        RenderThreads threads;
        {
            QMutexLocker l(&renderThreadsMutex);
            threads = renderThreads;
        }
        
        for (RenderThreads::iterator it = threads.begin(); it != threads.end(); ++it) {
            it->thread->wait();
        }
        {
            QMutexLocker l(&renderThreadsMutex);
            
            removeQuitRenderThreadsInternal();
            assert(renderThreads.empty());
        }
        
    }
    
};


OutputSchedulerThread::OutputSchedulerThread(RenderEngine* engine,Natron::OutputEffectInstance* effect,ProcessFrameModeEnum mode)
: QThread()
, _imp(new OutputSchedulerThreadPrivate(engine,effect,mode))
{
    QObject::connect(this, SIGNAL(s_doProcessOnMainThread(BufferedFrames,bool,int)), this,
                     SLOT(doProcessFrameMainThread(BufferedFrames,bool,int)));
    
    QObject::connect(_imp->timer.get(), SIGNAL(fpsChanged(double,double)), _imp->engine, SIGNAL(fpsChanged(double,double)));
    
    QObject::connect(this, SIGNAL(s_abortRenderingOnMainThread(bool)), this, SLOT(abortRendering(bool)));
    
    QObject::connect(this, SIGNAL(s_executeCallbackOnMainThread(QString)), this, SLOT(onExecuteCallbackOnMainThread(QString)));
    
    setObjectName("Scheduler thread");
}

OutputSchedulerThread::~OutputSchedulerThread()
{
    ///Wake-up all threads and tell them that they must quit
    stopRenderThreads(0);
    

    ///Make sure they are all gone, there will be a deadlock here if that's not the case.
    _imp->waitForRenderThreadsToQuit();
}


bool
OutputSchedulerThreadPrivate::getNextFrameInSequence(PlaybackModeEnum pMode,OutputSchedulerThread::RenderDirectionEnum direction,int frame,
                                                     int firstFrame,int lastFrame,
                                                     int* nextFrame,OutputSchedulerThread::RenderDirectionEnum* newDirection)
{
    *newDirection = direction;
    if (firstFrame == lastFrame) {
        *nextFrame = firstFrame;
        return true;
    }
    if (frame <= firstFrame) {
        switch (pMode) {
                case Natron::ePlaybackModeLoop:
                if (direction == OutputSchedulerThread::eRenderDirectionForward) {
                    *nextFrame = firstFrame + 1;
                } else {
                    *nextFrame  = lastFrame - 1;
                }
                break;
                case Natron::ePlaybackModeBounce:
                if (direction == OutputSchedulerThread::eRenderDirectionForward) {
                    *newDirection = OutputSchedulerThread::eRenderDirectionBackward;
                    *nextFrame  = lastFrame - 1;
                } else {
                    *newDirection = OutputSchedulerThread::eRenderDirectionForward;
                    *nextFrame  = firstFrame + 1;
                }
                break;
                case Natron::ePlaybackModeOnce:
                default:
                if (direction == OutputSchedulerThread::eRenderDirectionForward) {
                    *nextFrame = firstFrame + 1;
                    break;
                } else {
                    return false;
                }
                
                
        }
    } else if (frame >= lastFrame) {
        switch (pMode) {
                case Natron::ePlaybackModeLoop:
                if (direction == OutputSchedulerThread::eRenderDirectionForward) {
                    *nextFrame = firstFrame;
                } else {
                    *nextFrame = lastFrame - 1;
                }
                break;
                case Natron::ePlaybackModeBounce:
                if (direction == OutputSchedulerThread::eRenderDirectionForward) {
                    *newDirection = OutputSchedulerThread::eRenderDirectionBackward;
                    *nextFrame = lastFrame - 1;
                } else {
                    *newDirection = OutputSchedulerThread::eRenderDirectionForward;
                    *nextFrame = firstFrame + 1;
                }
                break;
                case Natron::ePlaybackModeOnce:
            default:
                if (direction == OutputSchedulerThread::eRenderDirectionForward) {
                    return false;
                } else {
                    *nextFrame = lastFrame - 1;
                    break;
                }

                
        }
    } else {
        if (direction == OutputSchedulerThread::eRenderDirectionForward) {
            *nextFrame = frame + 1;
            
        } else {
            *nextFrame = frame - 1;
        }
    }
    return true;

}

void
OutputSchedulerThreadPrivate::getNearestInSequence(OutputSchedulerThread::RenderDirectionEnum direction,int frame,
                          int firstFrame,int lastFrame,
                          int* nextFrame)
{
    if (frame >= firstFrame && frame <= lastFrame) {
        *nextFrame = frame;
    } else if (frame < firstFrame) {
        if (direction == OutputSchedulerThread::eRenderDirectionForward) {
            *nextFrame = firstFrame;
        } else {
            *nextFrame = lastFrame;
        }
    } else { // frame > lastFrame
        if (direction == OutputSchedulerThread::eRenderDirectionForward) {
            *nextFrame = lastFrame;
        } else {
            *nextFrame = firstFrame;
        }
    }
    
}

void
OutputSchedulerThread::pushFramesToRender(int startingFrame,int nThreads)
{

    QMutexLocker l(&_imp->framesToRenderMutex);
    _imp->lastFramePushedIndex = startingFrame;
    
    pushFramesToRenderInternal(startingFrame, nThreads);
}

void
OutputSchedulerThread::pushFramesToRenderInternal(int startingFrame,int nThreads)
{
    
    assert(!_imp->framesToRenderMutex.tryLock());
    
    ///Make sure at least 1 frame is pushed
    if (nThreads <= 0) {
        nThreads = 1;
    }
    
    RenderDirectionEnum direction;
    int firstFrame,lastFrame;
    {
        QMutexLocker l(&_imp->runArgsMutex);
        direction = _imp->livingRunArgs.timelineDirection;
        firstFrame = _imp->livingRunArgs.firstFrame;
        lastFrame = _imp->livingRunArgs.lastFrame;
    }
    
    PlaybackModeEnum pMode = _imp->engine->getPlaybackMode();
    
    if (firstFrame == lastFrame) {
        _imp->framesToRender.push_back(startingFrame);
        _imp->lastFramePushedIndex = startingFrame;
    } else {
        ///Push 2x the count of threads to be sure no one will be waiting
        while ((int)_imp->framesToRender.size() < nThreads * 2) {
            _imp->framesToRender.push_back(startingFrame);
            
            _imp->lastFramePushedIndex = startingFrame;
            
            if (!OutputSchedulerThreadPrivate::getNextFrameInSequence(pMode, direction, startingFrame,
                                                                      firstFrame, lastFrame, &startingFrame, &direction)) {
                break;
            }
        }
    }
  
    ///Wake up render threads to notify them theres work to do
    _imp->framesToRenderNotEmptyCond.wakeAll();

}

void
OutputSchedulerThread::pushAllFrameRange()
{
    QMutexLocker l(&_imp->framesToRenderMutex);
    RenderDirectionEnum direction;
    int firstFrame,lastFrame;
    {
        QMutexLocker l(&_imp->runArgsMutex);
        direction = _imp->livingRunArgs.timelineDirection;
        firstFrame = _imp->livingRunArgs.firstFrame;
        lastFrame = _imp->livingRunArgs.lastFrame;
    }
    
    if (direction == eRenderDirectionForward) {
        for (int i = firstFrame; i <= lastFrame; ++i) {
            _imp->framesToRender.push_back(i);
        }
    } else {
        for (int i = lastFrame; i >= firstFrame; --i) {
            _imp->framesToRender.push_back(i);
        }
    }
    ///Wake up render threads to notify them theres work to do
    _imp->framesToRenderNotEmptyCond.wakeAll();
}

void
OutputSchedulerThread::pushFramesToRender(int nThreads)
{
    QMutexLocker l(&_imp->framesToRenderMutex);

    RenderDirectionEnum direction;
    int firstFrame,lastFrame;
    {
        QMutexLocker l(&_imp->runArgsMutex);
        direction = _imp->livingRunArgs.timelineDirection;
        firstFrame = _imp->livingRunArgs.firstFrame;
        lastFrame = _imp->livingRunArgs.lastFrame;
    }
    
    PlaybackModeEnum pMode = _imp->engine->getPlaybackMode();
    
    int frame = _imp->lastFramePushedIndex;
    if (firstFrame == lastFrame && frame == firstFrame) {
        return;
    }

    ///If startingTime is already taken into account in the framesToRender, push new frames from the last one in the stack instead
    bool canContinue = OutputSchedulerThreadPrivate::getNextFrameInSequence(pMode, direction, frame,
                                                                        firstFrame, lastFrame, &frame, &direction);
    
    if (canContinue) {
        pushFramesToRenderInternal(frame, nThreads);
    } else {
        ///Still wake up threads that may still sleep
        _imp->framesToRenderNotEmptyCond.wakeAll();
    }
}

int
OutputSchedulerThread::pickFrameToRender(RenderThreadTask* thread)
{
    ///Flag the thread as inactive
    {
        QMutexLocker l(&_imp->renderThreadsMutex);
        RenderThreads::iterator found = _imp->getRunnableIterator(thread);
        assert(found != _imp->renderThreads.end());
        found->active = false;
        
        ///Wake up the scheduler if it is waiting for all threads do be inactive
        _imp->allRenderThreadsInactiveCond.wakeOne();
    }
    
    ///Simple heuristic to limit the size of the internal buffer.
    ///If the buffer grows too much, we will keep shared ptr to images, hence keep them in RAM which
    ///can lead to RAM issue for the end user.
    ///We can end up in this situation for very simple graphs where the rendering of the output node (the writer or viewer)
    ///is much slower than things upstream, hence the buffer grows quickly, and fills up the RAM.
    int nbThreadsHardware = appPTR->getHardwareIdealThreadCount();
    bool bufferFull;
    {
        QMutexLocker k(&_imp->bufMutex);
        bufferFull = (int)_imp->buf.size() >= nbThreadsHardware * 3;
    }
    
    QMutexLocker l(&_imp->framesToRenderMutex);
    while ((bufferFull || _imp->framesToRender.empty()) && !thread->mustQuit() ) {
        
        ///Notify that we're no longer doing work
        thread->notifyIsRunning(false);
        
        
        _imp->framesToRenderNotEmptyCond.wait(&_imp->framesToRenderMutex);
        
        {
            QMutexLocker k(&_imp->bufMutex);
            bufferFull = (int)_imp->buf.size() >= nbThreadsHardware * 3;
        }
    }
    
   
    if (!_imp->framesToRender.empty()) {
        
        ///Notify that we're running for good, will do nothing if flagged already running
        thread->notifyIsRunning(true);
        
        int ret = _imp->framesToRender.front();
        _imp->framesToRender.pop_front();
        
        ///Flag the thread as active
        {
            QMutexLocker l(&_imp->renderThreadsMutex);
            RenderThreads::iterator found = _imp->getRunnableIterator(thread);
            assert(found != _imp->renderThreads.end());
            found->active = true;
        }
        
        return ret;
    } else {
        // thread is quitting, make sure we notified the application it is no longer running
        thread->notifyIsRunning(false);
        
    }
    return -1;
}


void
OutputSchedulerThread::notifyThreadAboutToQuit(RenderThreadTask* thread)
{
    QMutexLocker l(&_imp->renderThreadsMutex);
    RenderThreads::iterator found = _imp->getRunnableIterator(thread);
    if (found != _imp->renderThreads.end()) {
        found->active = false;
        _imp->allRenderThreadsInactiveCond.wakeOne();
        _imp->allRenderThreadsQuitCond.wakeOne();
    }
}

bool
OutputSchedulerThread::isBeingAborted() const
{
    QMutexLocker k(&_imp->abortFlagMutex);
    return _imp->abortFlag;
}

void
OutputSchedulerThread::startRender()
{
    
    if ( isFPSRegulationNeeded() ) {
        _imp->timer->playState = ePlayStateRunning;
    }
    
    ///We will push frame to renders starting at startingFrame.
    ///They will be in the range determined by firstFrame-lastFrame
    int startingFrame;
    int firstFrame,lastFrame;
    bool forward;
    {
        ///Copy the last requested run args
        
        QMutexLocker l(&_imp->runArgsMutex);
        _imp->livingRunArgs = _imp->requestedRunArgs;
        firstFrame = _imp->livingRunArgs.firstFrame;
        lastFrame = _imp->livingRunArgs.lastFrame;
        startingFrame = timelineGetTime();
        forward = _imp->livingRunArgs.timelineDirection == OutputSchedulerThread::eRenderDirectionForward;
    }
    
    aboutToStartRender();
    
    ///Notify everyone that the render is started
    _imp->engine->s_renderStarted(forward);
    
    ///Flag that we're now doing work
    {
        QMutexLocker l(&_imp->workingMutex);
        _imp->working = true;
    }
    
    int nThreads;
    {
        QMutexLocker l(&_imp->renderThreadsMutex);
        _imp->removeAllQuitRenderThreads();
        nThreads = (int)_imp->renderThreads.size();
    }
    
    ///Start with one thread if it doesn't exist
    if (nThreads == 0) {
        int lastNThreads;
        adjustNumberOfThreads(&nThreads, &lastNThreads);
    }
    
    QMutexLocker l(&_imp->renderThreadsMutex);
    
    
    Natron::SchedulingPolicyEnum policy = getSchedulingPolicy();
    
    if (policy == Natron::eSchedulingPolicyFFA) {
        
        
        ///push all frame range and let the threads deal with it
        pushAllFrameRange();
    } else {
        
        ///If the output effect is sequential (only WriteFFMPEG for now)
        Natron::SequentialPreferenceEnum pref = _imp->outputEffect->getSequentialPreference();
        if (pref == eSequentialPreferenceOnlySequential || pref == eSequentialPreferencePreferSequential) {
            
            RenderScale scaleOne;
            scaleOne.x = scaleOne.y = 1.;
            if (_imp->outputEffect->beginSequenceRender_public(firstFrame, lastFrame,
                                                               1,
                                                               false,
                                                               scaleOne, true,
                                                               true,
                                                               _imp->outputEffect->getApp()->getMainView()) == eStatusFailed) {
                l.unlock();
                abortRendering(false);
                return;
            }
        }
        
        ///Push as many frames as there are threads
        pushFramesToRender(startingFrame,nThreads);
    }
    
    

}

void
OutputSchedulerThread::stopRender()
{
    _imp->timer->playState = ePlayStatePause;
    
    ///Wait for all render threads to be done
    {
        QMutexLocker l(&_imp->renderThreadsMutex);
        
        _imp->removeAllQuitRenderThreads();
        _imp->waitForRenderThreadsToBeDone();
    }
    
    
    ///If the output effect is sequential (only WriteFFMPEG for now)
    Natron::SequentialPreferenceEnum pref = _imp->outputEffect->getSequentialPreference();
    if (pref == eSequentialPreferenceOnlySequential || pref == eSequentialPreferencePreferSequential) {
        
        int firstFrame,lastFrame;
        {
            QMutexLocker l(&_imp->runArgsMutex);
            firstFrame = _imp->livingRunArgs.firstFrame;
            lastFrame = _imp->livingRunArgs.lastFrame;
        }

        
        RenderScale scaleOne;
        scaleOne.x = scaleOne.y = 1.;
        ignore_result(_imp->outputEffect->endSequenceRender_public(firstFrame, lastFrame,
                                                           1,
                                                           !appPTR->isBackground(),
                                                           scaleOne, true,
                                                           !appPTR->isBackground(),
                                                           _imp->outputEffect->getApp()->getMainView()));
           
        
    }
    
    {
        QMutexLocker abortBeingProcessedLocker(&_imp->abortBeingProcessedMutex);
        _imp->abortBeingProcessed = true;
        
        bool wasAborted;
        {
            QMutexLocker l(&_imp->abortedRequestedMutex);
            wasAborted = _imp->abortRequested > 0;
            
            ///reset back the abort flag
            _imp->abortRequested = 0;
            
            {
                QMutexLocker k(&_imp->abortFlagMutex);
                _imp->abortFlag = false;
            }
            //_imp->outputEffect->getApp()->getProject()->setAllNodesAborted(false);
            _imp->abortedRequestedCondition.wakeAll();
        }
        
        ///Flag that we're no longer doing work
        {
            QMutexLocker l(&_imp->workingMutex);
            _imp->working = false;
        }
        
        ///Clear any frames that were processed ahead
        {
            QMutexLocker l2(&_imp->bufMutex);
            _imp->clearBuffer();
        }
        
        ///Notify everyone that the render is finished
        _imp->engine->s_renderFinished(wasAborted ? 1 : 0);
        
        onRenderStopped(wasAborted);

        
    }
    
    
    {
        QMutexLocker l(&_imp->startRequestsMutex);
        while (_imp->startRequests <= 0) {
            _imp->startRequestsCond.wait(&_imp->startRequestsMutex);
        }
        ///We got the request, reset it back to 0
        _imp->startRequests = 0;
    }
}

void
OutputSchedulerThread::run()
{
    for (;;) { ///infinite loop
        
        if ( _imp->checkForExit() ) {
            return;
        }
        
        startRender();
        for (;;) {
            ///When set to true, we don't sleep in the bufEmptyCondition but in the startCondition instead, indicating
            ///we finished a render
            bool renderFinished = false;
            
            {
                ///_imp->renderFinished might be set when in FFA scheduling policy
                QMutexLocker l(&_imp->runArgsMutex);
                if (_imp->renderFinished) {
                    renderFinished = true;
                }
            }
            
            bool bufferEmpty;
            {
                QMutexLocker l(&_imp->bufMutex);
                bufferEmpty = _imp->buf.empty();
            }
            
            while (!bufferEmpty) {
                
                if ( _imp->checkForExit() ) {
                    return;
                }
                
                ///Check for abortion
                {
                    QMutexLocker locker(&_imp->abortedRequestedMutex);
                    if (_imp->abortRequested > 0) {
                        
                        ///Do not wait in the buf wait condition and go directly into the stopEngine()
                        renderFinished = true;
                        break;
                    }
                }
                
                int expectedTimeToRender = timelineGetTime();
                
                BufferedFrames framesToRender;
                {
                    QMutexLocker l(&_imp->bufMutex);
                    _imp->getFromBufferAndErase(expectedTimeToRender, framesToRender);
                }
                
                ///The expected frame is not yet ready, go to sleep again
                if (framesToRender.empty()) {
                    break;
                }
    
                int nextFrameToRender = -1;
               
                if (!renderFinished) {
                
                    ///////////
                    /////Refresh frame range if needed (for viewers)
                    

                    int firstFrame,lastFrame;
                    getFrameRangeToRender(firstFrame, lastFrame);
                    
                    
                    RenderDirectionEnum timelineDirection;
                    {
                        QMutexLocker l(&_imp->runArgsMutex);
                        
                        ///Refresh the firstframe/lastFrame as they might have changed on the timeline
                        _imp->livingRunArgs.firstFrame = firstFrame;
                        _imp->livingRunArgs.lastFrame = lastFrame;
                        
                        
                        
                        timelineDirection = _imp->livingRunArgs.timelineDirection;
                    }
                    
                    ///////////
                    ///Determine if we finished rendering or if we should just increment/decrement the timeline
                    ///or just loop/bounce
                    Natron::PlaybackModeEnum pMode = _imp->engine->getPlaybackMode();
                    RenderDirectionEnum newDirection;
                    if (firstFrame == lastFrame && pMode == ePlaybackModeOnce) {
                        renderFinished = true;
                        newDirection = eRenderDirectionForward;
                    } else {
                        renderFinished = !OutputSchedulerThreadPrivate::getNextFrameInSequence(pMode, timelineDirection,
                                                                                          expectedTimeToRender, firstFrame,
                                                                                          lastFrame, &nextFrameToRender, &newDirection);
                    }
                    if (newDirection != timelineDirection) {
                        QMutexLocker l(&_imp->runArgsMutex);
                        _imp->livingRunArgs.timelineDirection = newDirection;
                        _imp->requestedRunArgs.timelineDirection = newDirection;
                    }
                                        
                    if (!renderFinished) {
                        ///////////
                        /////If we were analysing the CPU activity, now set the appropriate number of threads to render.
                        int newNThreads, lastNThreads;
                        adjustNumberOfThreads(&newNThreads,&lastNThreads);
                        
                        ///////////
                        /////Append render requests for the render threads
                        pushFramesToRender(newNThreads);
                    }
                } // if (!renderFinished) {
                
                if (_imp->timer->playState == ePlayStateRunning) {
                    _imp->timer->waitUntilNextFrameIsDue(); // timer synchronizing with the requested fps
                }
                
                
                
                if (_imp->mode == eProcessFrameBySchedulerThread) {
                    processFrame(framesToRender);
                    
                    if (!renderFinished) {
                        ///Timeline might have changed if another thread moved the playhead
                        int timelineCurrentTime = timelineGetTime();
                        if (timelineCurrentTime != expectedTimeToRender) {
                            timelineGoTo(timelineCurrentTime);
                        } else {
                            timelineGoTo(nextFrameToRender);
                        }
                        
                    }
                } else {
                    ///Process on main-thread
                                    
                    QMutexLocker processLocker (&_imp->processMutex);
                    
                    ///Check for abortion while under processMutex to be sure the main thread is not deadlock in abortRendering
                    {
                        QMutexLocker locker(&_imp->abortedRequestedMutex);
                        if (_imp->abortRequested > 0) {
                            
                            ///Do not wait in the buf wait condition and go directly into the stopRender()
                            renderFinished = true;
                            
                            break;
                        }
                    }
                    
                    _imp->processRunning = true;
                    
                    int timeToSeek = 0;
                    if (!renderFinished) {
                        ///Timeline might have changed if another thread moved the playhead
                        int timelineCurrentTime = timelineGetTime();
                        if (timelineCurrentTime != expectedTimeToRender) {
                            timeToSeek = timelineCurrentTime;
                        } else {
                            timeToSeek = nextFrameToRender;
                        }
                        
                    }

                    Q_EMIT s_doProcessOnMainThread(framesToRender,!renderFinished, timeToSeek);
                    
                    while (_imp->processRunning) {
                        _imp->processCondition.wait(&_imp->processMutex);
                    }
                } // if (_imp->mode == eProcessFrameBySchedulerThread) {
                
                
                ////////////
                /////At this point the frame has been processed by the output device
                
                assert(!framesToRender.empty());
                notifyFrameRendered(expectedTimeToRender, 0, 1, framesToRender.front().timeRecorder, eSchedulingPolicyOrdered);
                
                ///////////
                /// End of the loop, refresh bufferEmpty
                {
                    QMutexLocker l(&_imp->bufMutex);
                    bufferEmpty = _imp->buf.empty();
                }
                
            } // while(!bufferEmpty)
            
            bool isAbortRequested;
            bool blocking;
            {
                QMutexLocker abortRequestedLock (&_imp->abortedRequestedMutex);
                isAbortRequested = _imp->abortRequested > 0;
                blocking = _imp->isAbortRequestBlocking;
            }
            if (!renderFinished && !isAbortRequested) {
                
                    QMutexLocker bufLocker (&_imp->bufMutex);
                    ///Wait here for more frames to be rendered, we will be woken up once appendToBuffer(...) is called
                    _imp->bufCondition.wait(&_imp->bufMutex);
            } else {
                if (blocking) {
                    //Move the timeline to the last rendered frame to keep it in sync with what is displayed
                    timelineGoTo(getLastRenderedTime());
                }
                break;
            }
        }
        
         stopRender();
        
    } // for(;;)
    
}

void
OutputSchedulerThread::adjustNumberOfThreads(int* newNThreads, int *lastNThreads)
{
    ///////////
    /////If we were analysing the CPU activity, now set the appropriate number of threads to render.
    int optimalNThreads;
    
    ///How many parallel renders the user wants
    int userSettingParallelThreads = appPTR->getCurrentSettings()->getNumberOfParallelRenders();
    
    ///How many threads are running in the application
    int runningThreads = appPTR->getNRunningThreads() + QThreadPool::globalInstance()->activeThreadCount();
    
    ///How many current threads are used by THIS renderer
    int currentParallelRenders = getNRenderThreads();
    *lastNThreads = currentParallelRenders;
    
    if (userSettingParallelThreads == 0) {
        ///User wants it to be automatically computed, do a simple heuristic: launch as many parallel renders
        ///as there are cores
        optimalNThreads = appPTR->getHardwareIdealThreadCount();
    } else {
        optimalNThreads = userSettingParallelThreads;
    }
    optimalNThreads = std::max(1,optimalNThreads);


    if ((runningThreads < optimalNThreads && currentParallelRenders < optimalNThreads) || currentParallelRenders == 0) {
     
        ////////
        ///Launch 1 thread
        QMutexLocker l(&_imp->renderThreadsMutex);
        
        _imp->appendRunnable(createRunnable());
        *newNThreads = currentParallelRenders +  1;
        
    } else if (runningThreads > optimalNThreads && currentParallelRenders > optimalNThreads) {
        ////////
        ///Stop 1 thread
        stopRenderThreads(1);
        *newNThreads = currentParallelRenders - 1;
        
    } else {
        /////////
        ///Keep the current count
        *newNThreads = std::max(1,currentParallelRenders);
    }
}

void
OutputSchedulerThread::notifyFrameRendered(int frame,
                                           int viewIndex,
                                           int viewsCount,
                                           const boost::shared_ptr<TimeLapse>& timeRecorder,
                                           Natron::SchedulingPolicyEnum policy)
{
    
    double percentage ;
    double timeSpent;
    int nbCurParallelRenders;
    if (timeRecorder) {
        timeSpent = timeRecorder->getTimeSinceCreation();
    }
    U64 nbFramesLeftToRender;
    bool isBackground = appPTR->isBackground();
    
    if (policy == eSchedulingPolicyFFA) {
        
        QMutexLocker l(&_imp->runArgsMutex);
        if (viewIndex == viewsCount -1) {
            ++_imp->nFramesRendered;
        }
        U64 totalFrames = _imp->livingRunArgs.lastFrame - _imp->livingRunArgs.firstFrame + 1;
        
        percentage = (double)_imp->nFramesRendered / totalFrames;
        nbFramesLeftToRender = totalFrames - _imp->nFramesRendered;
        
        if ( _imp->nFramesRendered == totalFrames) {

            _imp->renderFinished = true;
            l.unlock();

            ///Notify the scheduler rendering is finished by append a fake frame to the buffer
            {
                QMutexLocker bufLocker (&_imp->bufMutex);
                ignore_result(_imp->appendBufferedFrame(0, 0, boost::shared_ptr<TimeLapse>(), boost::shared_ptr<BufferableObject>()));
                _imp->bufCondition.wakeOne();
            }
            nbCurParallelRenders = getNRenderThreads();
        } else {
            l.unlock();
            
            ///////////
            /////If we were analysing the CPU activity, now set the appropriate number of threads to render.
            int newNThreads;
            adjustNumberOfThreads(&newNThreads, &nbCurParallelRenders);
        }
    } else {
        {
            QMutexLocker l(&_imp->runArgsMutex);
            if (_imp->livingRunArgs.timelineDirection == eRenderDirectionForward) {
                percentage = (double)frame / _imp->livingRunArgs.lastFrame - _imp->livingRunArgs.firstFrame + 1;
                nbFramesLeftToRender = _imp->livingRunArgs.lastFrame - frame;
            } else {
                U64 totalFrames = _imp->livingRunArgs.lastFrame - _imp->livingRunArgs.firstFrame + 1;
                percentage = 1. - (double)frame / totalFrames;
                nbFramesLeftToRender = frame - _imp->livingRunArgs.firstFrame;
            }
        }
        nbCurParallelRenders = getNRenderThreads();
    } // if (policy == eSchedulingPolicyFFA) {
    
    double avgTimeSpent,timeRemaining,totalTimeSpent;
    if (timeRecorder) {
        _imp->outputEffect->updateRenderTimeInfos(timeSpent, &avgTimeSpent, &totalTimeSpent);
        assert(nbCurParallelRenders > 0);
        timeRemaining = (nbFramesLeftToRender * avgTimeSpent) / (double)nbCurParallelRenders;
    }
    
    if (isBackground) {
        QString longMessage;
        QTextStream ts(&longMessage);
        QString frameStr = QString::number(frame);
        ts << kFrameRenderedStringLong << frameStr << " (" << QString::number(percentage * 100,'f',1) << "%)";
        if (timeRecorder) {
            QString timeSpentStr = Timer::printAsTime(timeSpent, false);
            QString timeRemainingStr = Timer::printAsTime(timeRemaining, true);
            ts << "\nTime elapsed for frame: " << timeSpentStr;
            ts << "\nTime remaining: " << timeRemainingStr;
            frameStr.append(';');
            frameStr.append(QString::number(timeSpent));
            frameStr.append(';');
            frameStr.append(QString::number(timeRemaining));
        }
        appPTR->writeToOutputPipe(longMessage,kFrameRenderedStringShort + frameStr);
    }
    
    if (viewIndex == viewsCount -1) {
        if (!timeRecorder) {
            _imp->engine->s_frameRendered(frame);
        } else {
            _imp->engine->s_frameRenderedWithTimer(frame, timeSpent, timeRemaining);
        }
    }
    
    if (_imp->outputEffect->isWriter()) {
        std::string cb = _imp->outputEffect->getNode()->getAfterFrameRenderCallback();
        if (!cb.empty()) {  
            std::vector<std::string> args;
            std::string error;
            Natron::getFunctionArguments(cb, &error, &args);
            if (!error.empty()) {
                _imp->outputEffect->getApp()->appendToScriptEditor("Failed to run after frame render callback: " + error);
                return;
            }
            
            std::string signatureError;
            signatureError.append("The after frame render callback supports the following signature(s):\n");
            signatureError.append("- callback(frame, thisNode, app)");
            if (args.size() != 3) {
                _imp->outputEffect->getApp()->appendToScriptEditor("Failed to run after frame render callback: " + signatureError);
                return;
            }
            
            if (args[0] != "frame" || args[1] != "thisNode" || args[2] != "app" ) {
                _imp->outputEffect->getApp()->appendToScriptEditor("Failed to run after frame render callback: " + signatureError);
                return;
            }
            
            std::stringstream ss;
            ss << cb << "(" << frame << ",";
            std::string script = ss.str();
            runCallbackWithVariables(script.c_str());
            
        }
    }
}

void
OutputSchedulerThread::appendToBuffer_internal(double time,
                                               int view,
                                               const boost::shared_ptr<TimeLapse>& timeRecorder,
                                               const boost::shared_ptr<BufferableObject>& frame,
                                               bool wakeThread)
{
    if (QThread::currentThread() == qApp->thread()) {
        ///Single-threaded , call directly the function
        if (frame) {
            BufferedFrame b;
            b.time = time;
            b.view = view;
            b.frame = frame;
            BufferedFrames frames;
            frames.push_back(b);
            processFrame(frames);
        }
    } else {
        
        ///Called by the scheduler thread when an image is rendered
        
        QMutexLocker l(&_imp->bufMutex);
        ignore_result(_imp->appendBufferedFrame(time, view, timeRecorder, frame));
        if (wakeThread) {
            ///Wake up the scheduler thread that an image is available if it is asleep so it can process it.
            _imp->bufCondition.wakeOne();
        }
        
    }
}

void
OutputSchedulerThread::appendToBuffer(double time,
                                      int view,
                                      const boost::shared_ptr<TimeLapse>& timeRecorder,
                                      const boost::shared_ptr<BufferableObject>& image)
{
    appendToBuffer_internal(time, view, timeRecorder, image, true);
}

void
OutputSchedulerThread::appendToBuffer(double time,
                                      int view,
                                      const boost::shared_ptr<TimeLapse>& timeRecorder,
                                      const BufferableObjectList& frames)
{
    if (frames.empty()) {
        return;
    }
    BufferableObjectList::const_iterator next = frames.begin();
    if (next != frames.end()) {
        ++next;
    }
    for (BufferableObjectList::const_iterator it = frames.begin(); it != frames.end(); ++it) {
        if (next != frames.end()) {
            appendToBuffer_internal(time, view, timeRecorder, *it, false);
            ++next;
        } else {
            appendToBuffer_internal(time, view, timeRecorder, *it, true);
        }
    }
}


void
OutputSchedulerThread::doProcessFrameMainThread(const BufferedFrames& frames,bool mustSeekTimeline,int time)
{
    assert(QThread::currentThread() == qApp->thread());
    {
        QMutexLocker processLocker (&_imp->processMutex);
        ///The flag might have been reseted back by abortRendering()
        if (!_imp->processRunning) {
            return;
        }
    }
    
    
    processFrame(frames);
    
    if (mustSeekTimeline) {
        timelineGoTo(time);
    }
    
    QMutexLocker processLocker (&_imp->processMutex);
    _imp->processRunning = false;
    _imp->processCondition.wakeOne();
}

void
OutputSchedulerThread::abortRendering(bool blocking)
{
    
    if ( !isRunning() || !isWorking() ) {
        return;
    }

    bool isMainThread = QThread::currentThread() == qApp->thread();
    
    

    {
        ///Before posting an abort request, we must make sure the scheduler thread is not currently processing an abort request
        ///in stopRender(), we ensure the former by taking the abortBeingProcessedMutex lock
        QMutexLocker l(&_imp->abortedRequestedMutex);
        _imp->abortBeingProcessed = false;
        _imp->isAbortRequestBlocking = blocking;
        
        ///We make sure the render-thread doesn't wait for the main-thread to process a frame
        ///This function (abortRendering) was probably called from a user event that was posted earlier in the
        ///event-loop, we just flag that the next event that will process the frame should NOT process it by
        ///reseting the processRunning flag
        {
            QMutexLocker l2(&_imp->processMutex);
            
            {
                QMutexLocker abortBeingProcessedLocker(&_imp->abortBeingProcessedMutex);
                
                ///We are already aborting but we don't want a blocking abort, it is useless to ask for a second abort
                if (!blocking && _imp->abortRequested > 0) {
                    return;
                }

                {
                    QMutexLocker k(&_imp->abortFlagMutex);
                    _imp->abortFlag = true;
                }
                _imp->outputEffect->getApp()->getProject()->notifyRenderBeingAborted();
                
                ++_imp->abortRequested;
            }
            
            ///Clear the work queue
            {
                QMutexLocker framesLocker (&_imp->framesToRenderMutex);
                _imp->framesToRender.clear();
            }
            
            
            if (isMainThread) {
                
                _imp->processRunning = false;
                _imp->processCondition.wakeOne();
            }
            
            {
                QMutexLocker l3(&_imp->runningCallbackMutex);
                _imp->runningCallback = false;
                _imp->runningCallbackCond.wakeAll();
            }
            
        }
        ///If the scheduler is asleep waiting for the buffer to be filling up, we post a fake request
        ///that will not be processed anyway because the first thing it does is checking for abort
        {
            QMutexLocker l2(&_imp->bufMutex);
            _imp->bufCondition.wakeOne();
        }
        
        while (blocking && _imp->abortRequested > 0 && QThread::currentThread() != this && isWorking()) {
            _imp->abortedRequestedCondition.wait(&_imp->abortedRequestedMutex);
        }
    }
}

void
OutputSchedulerThread::quitThread()
{
    if (!isRunning()) {
        return;
    }
    
    abortRendering(true);
    
    
    if (QThread::currentThread() == qApp->thread()) {
        ///If the scheduler thread was sleeping in the process condition, waiting for the main-thread to finish
        ///processing the frame then waiting in the mustQuitCond would create a deadlock.
        ///Instead we discard the processing of the frame by taking the lock and setting processRunning to false
        QMutexLocker processLocker (&_imp->processMutex);
        _imp->processRunning = false;
        _imp->processCondition.wakeOne();
    }
    
    {
        QMutexLocker l(&_imp->mustQuitMutex);
        _imp->mustQuit = true;
        
        ///Wake-up the thread with a fake request
        {
            QMutexLocker l3(&_imp->startRequestsMutex);
            ++_imp->startRequests;
            _imp->startRequestsCond.wakeOne();
        }
        
        ///Wait until it has really quit
        while (_imp->mustQuit) {
            _imp->mustQuitCond.wait(&_imp->mustQuitMutex);
        }
    }
    
    ///Wake-up all threads and tell them that they must quit
    stopRenderThreads(0);

    ///Make sure they are all gone, there will be a deadlock here if that's not the case.
    _imp->waitForRenderThreadsToQuit();
        
    
    
    wait();
}

bool
OutputSchedulerThread::mustQuitThread() const
{
    QMutexLocker l(&_imp->mustQuitMutex);
    return _imp->mustQuit;
}

void
OutputSchedulerThread::setDesiredFPS(double d)
{
    _imp->timer->setDesiredFrameRate(d);
}


double
OutputSchedulerThread::getDesiredFPS() const
{
    return _imp->timer->getDesiredFrameRate();
}

void
OutputSchedulerThread::renderFrameRange(int firstFrame,int lastFrame,RenderDirectionEnum direction)
{
    if (direction == eRenderDirectionForward) {
        timelineGoTo(firstFrame);
    } else {
        timelineGoTo(lastFrame);
    }
    
    {
        
        QMutexLocker l(&_imp->runArgsMutex);
        _imp->requestedRunArgs.firstFrame = firstFrame;
        _imp->requestedRunArgs.lastFrame = lastFrame;
        
        _imp->nFramesRendered = 0;
        _imp->renderFinished = false;
        
        ///Start with picking direction being the same as the timeline direction.
        ///Once the render threads are a few frames ahead the picking direction might be different than the
        ///timeline direction
        _imp->requestedRunArgs.timelineDirection = direction;

        
    }
    
    renderInternal();
    
}

void
OutputSchedulerThread::renderFromCurrentFrame(RenderDirectionEnum timelineDirection)
{
    

    {
        QMutexLocker l(&_imp->runArgsMutex);

        int firstFrame,lastFrame;
        getFrameRangeToRender(firstFrame, lastFrame);
  
        ///Make sure current frame is in the frame range
        int currentTime = timelineGetTime();
        OutputSchedulerThreadPrivate::getNearestInSequence(timelineDirection, currentTime, firstFrame, lastFrame, &currentTime);
        
        _imp->requestedRunArgs.firstFrame = firstFrame;
        _imp->requestedRunArgs.lastFrame = lastFrame;
        _imp->requestedRunArgs.timelineDirection = timelineDirection;
    }
    renderInternal();
}


void
OutputSchedulerThread::renderInternal()
{
    
    QMutexLocker quitLocker(&_imp->mustQuitMutex);
    if (_imp->hasQuit) {
        return;
    }
    
    if (!_imp->mustQuit) {
        if ( !isRunning() ) {
            ///The scheduler must remain responsive hence has the highest priority
            start(HighestPriority);
        } else {
            ///Wake up the thread with a start request
            QMutexLocker locker(&_imp->startRequestsMutex);
            if (_imp->startRequests <= 0) {
                ++_imp->startRequests;
            }
            _imp->startRequestsCond.wakeOne();
        }
    }
}

void
OutputSchedulerThread::notifyRenderFailure(const std::string& errorMessage)
{
    ///Abort all ongoing rendering
    doAbortRenderingOnMainThread(false);
    
    ///Handle failure: for viewers we make it black and don't display the error message which is irrelevant
    handleRenderFailure(errorMessage);
    
}



bool
OutputSchedulerThread::isWorking() const
{
    QMutexLocker l(&_imp->workingMutex);
    return _imp->working;
}


void
OutputSchedulerThread::getFrameRangeRequestedToRender(int &first,int& last) const
{
    first = _imp->livingRunArgs.firstFrame;
    last = _imp->livingRunArgs.lastFrame;
}

OutputSchedulerThread::RenderDirectionEnum
OutputSchedulerThread::getDirectionRequestedToRender() const
{
    QMutexLocker l(&_imp->runArgsMutex);
    return _imp->livingRunArgs.timelineDirection;
}

int
OutputSchedulerThread::getNRenderThreads() const
{
    QMutexLocker l(&_imp->renderThreadsMutex);
    return (int)_imp->renderThreads.size();
}

int
OutputSchedulerThread::getNActiveRenderThreads() const
{
    QMutexLocker l(&_imp->renderThreadsMutex);
    return _imp->getNActiveRenderThreads();
}

void
OutputSchedulerThread::stopRenderThreads(int nThreadsToStop)
{
    
   
    {
        
         ///First flag the number of threads to stop
        QMutexLocker l(&_imp->renderThreadsMutex);
        int i = 0;
        for (RenderThreads::iterator it = _imp->renderThreads.begin();
             it!=_imp->renderThreads.end() && (i < nThreadsToStop || nThreadsToStop == 0); ++it) {
            if (!it->thread->mustQuit()) {
                it->thread->scheduleForRemoval();
                ++i;
            }
            
        }
        
        ///Clean-up remaining zombie threads that are no longer useful
        _imp->removeAllQuitRenderThreads();
    }
    
    
    ///Wake-up all threads to make sure that they are notified that they must quit
    {
        QMutexLocker framesLocker(&_imp->framesToRenderMutex);
        _imp->framesToRenderNotEmptyCond.wakeAll();
    }
    


}

RenderEngine*
OutputSchedulerThread::getEngine() const
{
    return _imp->engine;
}

void
OutputSchedulerThread::onExecuteCallbackOnMainThread(QString callback)
{
    assert(QThread::currentThread() == qApp->thread());
    std::string err,output;
    if (!Natron::interpretPythonScript(callback.toStdString(), &err, &output)) {
        _imp->outputEffect->getApp()->appendToScriptEditor("Failed to run callback: " + err);
    } else if (!output.empty()) {
        _imp->outputEffect->getApp()->appendToScriptEditor(output);
    }
    
    QMutexLocker k(&_imp->runningCallbackMutex);
    _imp->runningCallback = false;
    _imp->runningCallbackCond.wakeAll();
}

void
OutputSchedulerThread::runCallback(const QString& callback)
{
    QMutexLocker k(&_imp->runningCallbackMutex);
    _imp->runningCallback = true;
    Q_EMIT s_executeCallbackOnMainThread(callback);
    
    while (_imp->runningCallback) {
        _imp->runningCallbackCond.wait(&_imp->runningCallbackMutex);
    }

}
void
OutputSchedulerThread::runCallbackWithVariables(const QString& callback)
{
    if (!callback.isEmpty()) {
        QString script = callback;
        std::string appID = _imp->outputEffect->getApp()->getAppIDString();
        std::string thisNodeStr = appID + "." + _imp->outputEffect->getNode()->getFullyQualifiedName();
        script.append(thisNodeStr.c_str());
        script.append(",");
        script.append(appID.c_str());
        script.append(")\n");
        Q_EMIT s_executeCallbackOnMainThread(script);
    }
}


////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
//////////////////////// RenderThreadTask ////////////

struct RenderThreadTaskPrivate
{
    OutputSchedulerThread* scheduler;
    
    Natron::OutputEffectInstance* output;
    
    QMutex mustQuitMutex;
    bool mustQuit;
    bool hasQuit;
    
    QMutex runningMutex;
    bool running;
    
    RenderThreadTaskPrivate(Natron::OutputEffectInstance* output,OutputSchedulerThread* scheduler)
    : scheduler(scheduler)
    , output(output)
    , mustQuitMutex()
    , mustQuit(false)
    , hasQuit(false)
    , runningMutex()
    , running(false)
    {
        
    }
};


RenderThreadTask::RenderThreadTask(Natron::OutputEffectInstance* output,OutputSchedulerThread* scheduler)
: QThread()
, _imp(new RenderThreadTaskPrivate(output,scheduler))
{
    setObjectName("Parallel render thread");
}

RenderThreadTask::~RenderThreadTask()
{
    
}

void
RenderThreadTask::run()
{
    
    notifyIsRunning(true);
    
    for (;;) {
        
        int time = _imp->scheduler->pickFrameToRender(this);
        
        if ( mustQuit() ) {
            break;
        }
        
        renderFrame(time);
        
        if ( mustQuit() ) {
            break;
        }
    }
    
    {
        QMutexLocker l(&_imp->mustQuitMutex);
        _imp->hasQuit = true;
    }
    notifyIsRunning(false);
    _imp->scheduler->notifyThreadAboutToQuit(this);

}

bool
RenderThreadTask::hasQuit() const
{
    
    QMutexLocker l(&_imp->mustQuitMutex);
    return _imp->hasQuit;
    
}

void
RenderThreadTask::scheduleForRemoval()
{
    QMutexLocker l(&_imp->mustQuitMutex);
    _imp->mustQuit = true;
}

bool
RenderThreadTask::mustQuit() const
{
    QMutexLocker l(&_imp->mustQuitMutex);
    return _imp->mustQuit;
}

void
RenderThreadTask::notifyIsRunning(bool running)
{
    {
        QMutexLocker l(&_imp->runningMutex);
        if (_imp->running == running) {
            return;
        }
        _imp->running = running;
    }
    
    appPTR->fetchAndAddNRunningThreads(running ? 1 : - 1);
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
//////////////////////// DefaultScheduler ////////////


DefaultScheduler::DefaultScheduler(RenderEngine* engine,Natron::OutputEffectInstance* effect)
: OutputSchedulerThread(engine,effect,eProcessFrameBySchedulerThread)
, _effect(effect)
{
    engine->setPlaybackMode(ePlaybackModeOnce);
}

DefaultScheduler::~DefaultScheduler()
{
    
}

class DefaultRenderFrameRunnable : public RenderThreadTask
{
    
public:
    
    DefaultRenderFrameRunnable(Natron::OutputEffectInstance* writer,OutputSchedulerThread* scheduler)
    : RenderThreadTask(writer,scheduler)
    {
        
    }
    
    virtual ~DefaultRenderFrameRunnable()
    {
        
    }
    
private:
    
    
    virtual void
    renderFrame(int time) {
        
        boost::shared_ptr<TimeLapse> timeRecorder(new TimeLapse);
        
        std::string cb = _imp->output->getNode()->getBeforeFrameRenderCallback();
        if (!cb.empty()) {
            std::vector<std::string> args;
            std::string error;
            Natron::getFunctionArguments(cb, &error, &args);
            if (!error.empty()) {
                _imp->output->getApp()->appendToScriptEditor("Failed to run before frame render callback: " + error);
                return;
            }
            
            std::string signatureError;
            signatureError.append("The before frame render callback supports the following signature(s):\n");
            signatureError.append("- callback(frame, thisNode, app)");
            if (args.size() != 3) {
                _imp->output->getApp()->appendToScriptEditor("Failed to run before frame render callback: " + signatureError);
                return;
            }
            
            if (args[0] != "frame" || args[1] != "thisNode" || args[2] != "app" ) {
                _imp->output->getApp()->appendToScriptEditor("Failed to run before frame render callback: " + signatureError);
                return;
            }

            std::stringstream ss;
            ss << cb << "(" << time << ",";
            std::string script = ss.str();
            _imp->scheduler->runCallbackWithVariables(script.c_str());

        }
        
        try {
            ////Writers always render at scale 1.
            int mipMapLevel = 0;
            RenderScale scale;
            scale.x = scale.y = 1.;
            
            RectD rod;
            bool isProjectFormat;
            int viewsCount = _imp->output->getApp()->getProject()->getProjectViewsCount();
            
            
            int mainView = 0;
            
            Natron::SequentialPreferenceEnum sequentiallity = _imp->output->getSequentialPreference();
            
            ///The effect is sequential (e.g: WriteFFMPEG), and thus cannot render multiple views, we have to choose one
            ///We pick the user defined main view in the project settings
            
            bool canOnlyHandleOneView = sequentiallity == Natron::eSequentialPreferenceOnlySequential || sequentiallity == Natron::eSequentialPreferencePreferSequential;
            if (canOnlyHandleOneView) {
                mainView = _imp->output->getApp()->getMainView();
            }
       
            
            /// If the writer dosn't need to render the frames in any sequential order (such as image sequences for instance), then
            /// we just render the frames directly in this thread, no need to use the scheduler thread for maximum efficiency.
        
            bool renderDirectly = sequentiallity == Natron::eSequentialPreferenceNotSequential;
            
            
            // Do not catch exceptions: if an exception occurs here it is probably fatal, since
            // it comes from Natron itself. All exceptions from plugins are already caught
            // by the HostSupport library.
            EffectInstance* activeInputToRender;
            if (renderDirectly) {
                activeInputToRender = _imp->output;
            } else {
                activeInputToRender = _imp->output->getInput(0);
                if (activeInputToRender) {
                    activeInputToRender = activeInputToRender->getNearestNonDisabled();
                } else {
                    _imp->scheduler->notifyRenderFailure("No input to render");
                    return;
                }
                
            }
            
            assert(activeInputToRender);
            U64 activeInputToRenderHash = activeInputToRender->getHash();
            
            const double par = activeInputToRender->getPreferredAspectRatio();
            
            for (int i = 0; i < viewsCount; ++i) {
                if ( canOnlyHandleOneView && (i != mainView) ) {
                    ///@see the warning in EffectInstance::evaluate
                    continue;
                }
                
                StatusEnum stat = activeInputToRender->getRegionOfDefinition_public(activeInputToRenderHash,time, scale, i, &rod, &isProjectFormat);
                if (stat != eStatusFailed) {
                    std::list<ImageComponents> components;
                    ImageBitDepthEnum imageDepth;
                    activeInputToRender->getPreferredDepthAndComponents(-1, &components, &imageDepth);
                    RectI renderWindow;
                    rod.toPixelEnclosing(scale, par, &renderWindow);
                    
                    ParallelRenderArgsSetter frameRenderArgs(activeInputToRender->getApp()->getProject().get(),
                                                             time,
                                                             i,
                                                             false,  // is this render due to user interaction ?
                                                             canOnlyHandleOneView, // is this sequential ?
                                                             true, // canAbort ?
                                                             0, //renderAge
                                                             _imp->output, // viewer requester
                                                             0, //texture index
                                                             _imp->output->getApp()->getTimeLine().get(),
                                                             NodePtr(),
                                                             false,
                                                             false,
                                                             false);
                    
                    RenderingFlagSetter flagIsRendering(activeInputToRender->getNode().get());
                    
                    ImageList planes;
                    EffectInstance::RenderRoIRetCode retCode =
                    activeInputToRender->renderRoI( EffectInstance::RenderRoIArgs(time, //< the time at which to render
                                                                                  scale, //< the scale at which to render
                                                                                  mipMapLevel, //< the mipmap level (redundant with the scale)
                                                                                  i, //< the view to render
                                                                                  false,
                                                                                  renderWindow, //< the region of interest (in pixel coordinates)
                                                                                  rod, // < any precomputed rod ? in canonical coordinates
                                                                                  components,
                                                                                  imageDepth,
                                                                                  _imp->output),&planes);
                    if (retCode != EffectInstance::eRenderRoIRetCodeOk) {
                        _imp->scheduler->notifyRenderFailure("Error caught while rendering");
                        return;
                    }
                    
                    ///If we need sequential rendering, pass the image to the output scheduler that will ensure the sequential ordering
                    if (!renderDirectly) {
                        for (ImageList::iterator it = planes.begin(); it != planes.end(); ++it) {
                            _imp->scheduler->appendToBuffer(time, i, timeRecorder, boost::dynamic_pointer_cast<BufferableObject>(*it));
                        }
                    } else {
                        _imp->scheduler->notifyFrameRendered(time, i, viewsCount, timeRecorder, eSchedulingPolicyFFA);
                    }
                    
                } else {
                    break;
                }
            }
            
        } catch (const std::exception& e) {
            _imp->scheduler->notifyRenderFailure(std::string("Error while rendering: ") + e.what());
        }
    }
};

RenderThreadTask*
DefaultScheduler::createRunnable()
{
    return new DefaultRenderFrameRunnable(_effect,this);
}



/**
 * @brief Called whenever there are images available to process in the buffer.
 * Once processed, the frame will be removed from the buffer.
 *
 * According to the ProcessFrameModeEnum given to the scheduler this function will be called either by the scheduler thread (this)
 * or by the application's main-thread (typically to do OpenGL rendering).
 **/
void
DefaultScheduler::processFrame(const BufferedFrames& frames)
{
    assert(!frames.empty());
    //Only consider the first frame, we shouldn't have multiple view here anyway.
    const BufferedFrame& frame = frames.front();
    
    ///Writers render to scale 1 always
    RenderScale scale;
    scale.x = scale.y = 1.;
    
    U64 hash = _effect->getHash();
    
    bool isProjectFormat;
    RectD rod;
    RectI roi;
    
    std::list<Natron::ImageComponents> components;
    Natron::ImageBitDepthEnum imageDepth;
    _effect->getPreferredDepthAndComponents(-1, &components, &imageDepth);
    
    const double par = _effect->getPreferredAspectRatio();
    
    Natron::SequentialPreferenceEnum sequentiallity = _effect->getSequentialPreference();
    bool canOnlyHandleOneView = sequentiallity == Natron::eSequentialPreferenceOnlySequential || sequentiallity == Natron::eSequentialPreferencePreferSequential;
    
    for (BufferedFrames::const_iterator it = frames.begin(); it != frames.end(); ++it) {
        ignore_result(_effect->getRegionOfDefinition_public(hash,it->time, scale, it->view, &rod, &isProjectFormat));
        rod.toPixelEnclosing(0, par, &roi);
        
        ParallelRenderArgsSetter frameRenderArgs(_effect->getApp()->getProject().get(),
                                                 it->time,
                                                 it->view,
                                                 false,  // is this render due to user interaction ?
                                                 canOnlyHandleOneView, // is this sequential ?
                                                 true, //canAbort
                                                 0, //renderAge
                                                 _effect, //viewer
                                                 0, //texture index
                                                 _effect->getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false,
                                                 false,
                                                 false);
        
        RenderingFlagSetter flagIsRendering(_effect->getNode().get());
        
        ImagePtr inputImage = boost::dynamic_pointer_cast<Natron::Image>(it->frame);
        assert(inputImage);
        
        EffectInstance::InputImagesMap inputImages;
        inputImages[0].push_back(inputImage);
        Natron::EffectInstance::RenderRoIArgs args(frame.time,
                                                   scale,0,
                                                   it->view,
                                                   true, // for writers, always by-pass cache for the write node only @see renderRoiInternal
                                                   roi,
                                                   rod,
                                                   components,
                                                   imageDepth,
                                                   _effect,
                                                   inputImages);
        try {
            ImageList planes;
            EffectInstance::RenderRoIRetCode retCode = _effect->renderRoI(args,&planes);
            if (retCode != EffectInstance::eRenderRoIRetCodeOk) {
                notifyRenderFailure("");
            }
        } catch (const std::exception& e) {
            notifyRenderFailure(e.what());
        }

    }
    
}

void
DefaultScheduler::timelineStepOne(OutputSchedulerThread::RenderDirectionEnum direction)
{
    if (direction == OutputSchedulerThread::eRenderDirectionForward) {
        _effect->incrementCurrentFrame();
    } else {
        _effect->decrementCurrentFrame();
    }
}

void
DefaultScheduler::timelineGoTo(int time)
{
    _effect->setCurrentFrame(time);
}

int
DefaultScheduler::timelineGetTime() const
{
    return _effect->getCurrentFrame();
}

void
DefaultScheduler::getFrameRangeToRender(int& first,int& last) const
{
    first = _effect->getFirstFrame();
    last = _effect->getLastFrame();
}


void
DefaultScheduler::handleRenderFailure(const std::string& errorMessage)
{
    std::cout << errorMessage << std::endl;
}

Natron::SchedulingPolicyEnum
DefaultScheduler::getSchedulingPolicy() const
{
    Natron::SequentialPreferenceEnum sequentiallity = _effect->getSequentialPreference();
    if (sequentiallity == Natron::eSequentialPreferenceNotSequential) {
        return Natron::eSchedulingPolicyFFA;
    } else {
        return Natron::eSchedulingPolicyOrdered;
    }
}


void
DefaultScheduler::aboutToStartRender()
{
    int first,last;
    getFrameRangeRequestedToRender(first, last);
    
    _effect->setFirstFrame(first);
    _effect->setLastFrame(last);
    
    if (getDirectionRequestedToRender() == eRenderDirectionForward) {
        _effect->setCurrentFrame(first);
    } else {
        _effect->setCurrentFrame(last);
    }
    
    bool isBackGround = appPTR->isBackground();
    
    if (!isBackGround) {
        _effect->setKnobsFrozen(true);
    } else {
        appPTR->writeToOutputPipe(kRenderingStartedLong, kRenderingStartedShort);
    }
    
    std::string cb = _effect->getNode()->getBeforeRenderCallback();
    if (!cb.empty()) {
        std::vector<std::string> args;
        std::string error;
        Natron::getFunctionArguments(cb, &error, &args);
        if (!error.empty()) {
            _effect->getApp()->appendToScriptEditor("Failed to run beforeRender callback: " + error);
            return;
        }
        
        std::string signatureError;
        signatureError.append("The after render callback supports the following signature(s):\n");
        signatureError.append("- callback(thisNode, app)");
        if (args.size() != 2) {
            _effect->getApp()->appendToScriptEditor("Failed to run beforeRender callback: " + signatureError);
            return;
        }
        
        if (args[0] != "thisNode" || args[1] != "app" ) {
            _effect->getApp()->appendToScriptEditor("Failed to run beforeRender callback: " + signatureError);
            return;
        }
        
        
        std::string script(cb + "(");
        runCallbackWithVariables(script.c_str());
    }
}

void
DefaultScheduler::onRenderStopped(bool aborted)
{
    bool isBackGround = appPTR->isBackground();
    if (!isBackGround) {
        _effect->setKnobsFrozen(false);
    } else {
        _effect->notifyRenderFinished();
    }
    
    std::string cb = _effect->getNode()->getAfterRenderCallback();
    if (!cb.empty()) {
        
        std::vector<std::string> args;
        std::string error;
        Natron::getFunctionArguments(cb, &error, &args);
        if (!error.empty()) {
            _effect->getApp()->appendToScriptEditor("Failed to run afterRender callback: " + error);
            return;
        }
        
        std::string signatureError;
        signatureError.append("The after render callback supports the following signature(s):\n");
        signatureError.append("- callback(aborted, thisNode, app)");
        if (args.size() != 3) {
            _effect->getApp()->appendToScriptEditor("Failed to run afterRender callback: " + signatureError);
            return;
        }
        
        if (args[0] != "aborted" || args[1] != "thisNode" || args[2] != "app" ) {
            _effect->getApp()->appendToScriptEditor("Failed to run afterRender callback: " + signatureError);
            return;
        }

        
        std::string script(cb + "(");
        if (aborted) {
            script += "True,";
        } else {
            script += "False,";
        }
        runCallbackWithVariables(script.c_str());
    }

}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
//////////////////////// ViewerDisplayScheduler ////////////


ViewerDisplayScheduler::ViewerDisplayScheduler(RenderEngine* engine,ViewerInstance* viewer)
: OutputSchedulerThread(engine,viewer,eProcessFrameByMainThread) //< OpenGL rendering is done on the main-thread
, _viewer(viewer)
{
    
}

ViewerDisplayScheduler::~ViewerDisplayScheduler()
{
    
}


/**
 * @brief Called whenever there are images available to process in the buffer.
 * Once processed, the frame will be removed from the buffer.
 *
 * According to the ProcessFrameModeEnum given to the scheduler this function will be called either by the scheduler thread (this)
 * or by the application's main-thread (typically to do OpenGL rendering).
 **/
void
ViewerDisplayScheduler::processFrame(const BufferedFrames& frames)
{

    if (!frames.empty()) {
        for (BufferedFrames::const_iterator it = frames.begin(); it != frames.end(); ++it) {
            boost::shared_ptr<UpdateViewerParams> params = boost::dynamic_pointer_cast<UpdateViewerParams>(it->frame);
            assert(params);
            _viewer->updateViewer(params);
        }
    }
    _viewer->redrawViewer();
    
}

void
ViewerDisplayScheduler::timelineStepOne(OutputSchedulerThread::RenderDirectionEnum direction)
{
    assert(_viewer);
    if (direction == OutputSchedulerThread::eRenderDirectionForward) {
        _viewer->getTimeline()->incrementCurrentFrame();
    } else {
        _viewer->getTimeline()->decrementCurrentFrame();
    }
}

void
ViewerDisplayScheduler::timelineGoTo(int time)
{
    assert(_viewer);
    _viewer->getTimeline()->seekFrame(time, false, 0, Natron::eTimelineChangeReasonPlaybackSeek);
}

int
ViewerDisplayScheduler::timelineGetTime() const
{
    return _viewer->getTimeline()->currentFrame();
}

void
ViewerDisplayScheduler::getFrameRangeToRender(int &first, int &last) const
{
    ViewerInstance* leadViewer = _viewer->getApp()->getLastViewerUsingTimeline();
    ViewerInstance* viewer = leadViewer ? leadViewer : _viewer;
    assert(viewer);
    viewer->getTimelineBounds(&first, &last);
}


class ViewerRenderFrameRunnable : public RenderThreadTask
{
  
    ViewerInstance* _viewer;
    
public:
    
    ViewerRenderFrameRunnable(ViewerInstance* viewer,OutputSchedulerThread* scheduler)
    : RenderThreadTask(viewer,scheduler)
    , _viewer(viewer)
    {
        
    }
    
    virtual ~ViewerRenderFrameRunnable()
    {
        
    }
    
private:
    
    virtual void
    renderFrame(int time) {
        
        ///The viewer always uses the scheduler thread to regulate the output rate, @see ViewerInstance::renderViewer_internal
        ///it calls appendToBuffer by itself
        StatusEnum stat = eStatusReplyDefault;
        
        int viewsCount = _viewer->getRenderViewsCount();
        int view = viewsCount > 0 ? _viewer->getViewerCurrentView() : 0;
        U64 viewerHash = _viewer->getHash();
        boost::shared_ptr<ViewerArgs> args[2];
        
        Natron::StatusEnum status[2] = {
            eStatusFailed, eStatusFailed
        };
        
        for (int i = 0; i < 2; ++i) {
            args[i].reset(new ViewerArgs);
            status[i] = _viewer->getRenderViewerArgsAndCheckCache_public(time, true, true, view, i, viewerHash, NodePtr(), true, args[i].get());
        }
       
        if (status[0] == eStatusFailed && status[1] == eStatusFailed) {
            _imp->scheduler->notifyRenderFailure(std::string());
            return;
        } else if (status[0] == eStatusReplyDefault || status[1] == eStatusReplyDefault) {
            return;
        } else {
            BufferableObjectList toAppend;
            for (int i = 0; i < 2; ++i) {
                if (args[i] && args[i]->params && args[i]->params->ramBuffer) {
                    toAppend.push_back(args[i]->params);
                    args[i].reset();
                }
            }
            _imp->scheduler->appendToBuffer(time, view, boost::shared_ptr<TimeLapse>(), toAppend);
        }
        
        
        if ((args[0] && status[0] != eStatusFailed) || (args[1] && status[1] != eStatusFailed)) {
            try {
                stat = _viewer->renderViewer(view,false,true,viewerHash,true, NodePtr(), true,  args, boost::shared_ptr<RequestedFrame>());
            } catch (...) {
                stat = eStatusFailed;
            }
        } else {
            return;
        }
        
        if (stat == eStatusFailed) {
            ///Don't report any error message otherwise we will flood the viewer with irrelevant messages such as
            ///"Render failed", instead we let the plug-in that failed post an error message which will be more helpful.
            _imp->scheduler->notifyRenderFailure(std::string());
        } else {
            BufferableObjectList toAppend;
            for (int i = 0; i < 2; ++i) {
                if (args[i] && args[i]->params && args[i]->params->ramBuffer) {
                    toAppend.push_back(args[i]->params);
                }
            }
     
            _imp->scheduler->appendToBuffer(time, view, boost::shared_ptr<TimeLapse>(), toAppend);
            
        }

    }
};

RenderThreadTask*
ViewerDisplayScheduler::createRunnable()
{
    return new ViewerRenderFrameRunnable(_viewer,this);
}

void
ViewerDisplayScheduler::handleRenderFailure(const std::string& /*errorMessage*/)
{
    _viewer->disconnectViewer();
}

void
ViewerDisplayScheduler::onRenderStopped(bool /*/aborted*/)
{
    ///Refresh all previews in the tree
    _viewer->getNode()->refreshPreviewsRecursivelyUpstream(_viewer->getTimeline()->currentFrame());
    
    if (!_viewer->getApp() || _viewer->getApp()->isGuiFrozen()) {
        getEngine()->s_refreshAllKnobs();
    }
}

int
ViewerDisplayScheduler::getLastRenderedTime() const
{
    return _viewer->getLastRenderedTime();
}


////////////////////////// RenderEngine

struct RenderEnginePrivate
{
    QMutex schedulerCreationLock;
    OutputSchedulerThread* scheduler;
    
    Natron::OutputEffectInstance* output;
    
    mutable QMutex pbModeMutex;
    Natron::PlaybackModeEnum pbMode;
    
    ViewerCurrentFrameRequestScheduler* currentFrameScheduler;
    
    RenderEnginePrivate(Natron::OutputEffectInstance* output)
    : schedulerCreationLock()
    , scheduler(0)
    , output(output)
    , pbModeMutex()
    , pbMode(ePlaybackModeLoop)
    , currentFrameScheduler(0)
    {
        
    }
};

RenderEngine::RenderEngine(Natron::OutputEffectInstance* output)
: _imp(new RenderEnginePrivate(output))
{
    
}

RenderEngine::~RenderEngine()
{
    delete _imp->currentFrameScheduler;
    delete _imp->scheduler;
}

OutputSchedulerThread*
RenderEngine::createScheduler(Natron::OutputEffectInstance* effect)
{
    return new DefaultScheduler(this,effect);
}

void
RenderEngine::renderFrameRange(int firstFrame,int lastFrame,OutputSchedulerThread::RenderDirectionEnum forward)
{
    {
        QMutexLocker k(&_imp->schedulerCreationLock);
        if (!_imp->scheduler) {
            _imp->scheduler = createScheduler(_imp->output);
        }
    }
    
    _imp->scheduler->renderFrameRange(firstFrame, lastFrame, forward);
}

void
RenderEngine::renderFromCurrentFrame(OutputSchedulerThread::RenderDirectionEnum forward)
{
    
    {
        QMutexLocker k(&_imp->schedulerCreationLock);
        if (!_imp->scheduler) {
            _imp->scheduler = createScheduler(_imp->output);
        }
    }
    
    _imp->scheduler->renderFromCurrentFrame(forward);
}

void
RenderEngine::renderFromCurrentFrameUsingCurrentDirection()
{
    {
        QMutexLocker k(&_imp->schedulerCreationLock);
        if (!_imp->scheduler) {
            _imp->scheduler = createScheduler(_imp->output);
        }
    }
    
    _imp->scheduler->renderFromCurrentFrame( _imp->scheduler->getDirectionRequestedToRender());
}

void
RenderEngine::renderCurrentFrame(bool canAbort)
{
    assert(QThread::currentThread() == qApp->thread());
    
    ViewerInstance* isViewer = dynamic_cast<ViewerInstance*>(_imp->output);
    if ( !isViewer ) {
        qDebug() << "RenderEngine::renderCurrentFrame for a writer is unsupported";
        return;
    }
    
    
    ///If the scheduler is already doing playback, continue it
    if ( _imp->scheduler && _imp->scheduler->isWorking() ) {
        _imp->scheduler->abortRendering(false);
        _imp->scheduler->renderFromCurrentFrame( _imp->scheduler->getDirectionRequestedToRender() );
        return;
    }
    
    
    {
        QMutexLocker k(&_imp->schedulerCreationLock);
        if (!_imp->scheduler) {
            _imp->scheduler = createScheduler(_imp->output);
        }
    }
    
    if (!_imp->currentFrameScheduler) {
        _imp->currentFrameScheduler = new ViewerCurrentFrameRequestScheduler(isViewer);
    }
    
    _imp->currentFrameScheduler->renderCurrentFrame(canAbort);
}



void
RenderEngine::quitEngine()
{
    if (_imp->scheduler) {
        _imp->scheduler->quitThread();
    }
    
    if (_imp->currentFrameScheduler) {
        _imp->currentFrameScheduler->quitThread();
    }
}

bool
RenderEngine::isSequentialRenderBeingAborted() const
{
    if (!_imp->scheduler) {
        return false;
    }
    return _imp->scheduler->isBeingAborted();
}

bool
RenderEngine::hasThreadsAlive() const
{

    bool schedulerRunning = false;
    if (_imp->scheduler) {
        schedulerRunning = _imp->scheduler->isRunning();
    }
    bool currentFrameSchedulerRunning = false;
    if (_imp->currentFrameScheduler) {
        currentFrameSchedulerRunning = _imp->currentFrameScheduler->isRunning();
    }
    
    return schedulerRunning || currentFrameSchedulerRunning;
}

bool
RenderEngine::hasThreadsWorking() const
{
 
    
    bool schedulerWorking = false;
    if (_imp->scheduler) {
        schedulerWorking = _imp->scheduler->isWorking();
    }
    bool currentFrameSchedulerWorking = false;
    if (_imp->currentFrameScheduler) {
        currentFrameSchedulerWorking = _imp->currentFrameScheduler->hasThreadsWorking();
    }
    
    return schedulerWorking || currentFrameSchedulerWorking;

}

bool
RenderEngine::isDoingSequentialRender() const
{
    return _imp->scheduler ? _imp->scheduler->isWorking() : false;
}

bool
RenderEngine::abortRendering(bool blocking)
{
    ViewerInstance* viewer = dynamic_cast<ViewerInstance*>(_imp->output);
    if (viewer) {
        viewer->markAllOnRendersAsAborted();
        
    }
    if (_imp->scheduler && _imp->scheduler->isWorking()) {
        _imp->scheduler->abortRendering(blocking);
        return true;
    }
    
    return false;
}

void
RenderEngine::setPlaybackMode(int mode)
{
    QMutexLocker l(&_imp->pbModeMutex);
    _imp->pbMode = (Natron::PlaybackModeEnum)mode;
}

Natron::PlaybackModeEnum
RenderEngine::getPlaybackMode() const
{
    QMutexLocker l(&_imp->pbModeMutex);
    return _imp->pbMode;
}

void
RenderEngine::setDesiredFPS(double d)
{
    {
        QMutexLocker k(&_imp->schedulerCreationLock);
        if (!_imp->scheduler) {
            _imp->scheduler = createScheduler(_imp->output);
        }
    }
    _imp->scheduler->setDesiredFPS(d);
}

double
RenderEngine::getDesiredFPS() const
{
    
    return _imp->scheduler ? _imp->scheduler->getDesiredFPS() : 24;
}


void
RenderEngine::notifyFrameProduced(const BufferableObjectList& frames, const boost::shared_ptr<RequestedFrame>& request)
{
    _imp->currentFrameScheduler->notifyFrameProduced(frames, request);
}

OutputSchedulerThread*
ViewerRenderEngine::createScheduler(Natron::OutputEffectInstance* effect) 
{
    return new ViewerDisplayScheduler(this,dynamic_cast<ViewerInstance*>(effect));
}

////////////////////////ViewerCurrentFrameRequestScheduler////////////////////////



struct ViewerCurrentFrameRequestSchedulerPrivate
{
    
    ViewerInstance* viewer;
    
    QMutex requestsQueueMutex;
    std::list<boost::shared_ptr<RequestedFrame> > requestsQueue;
    QWaitCondition requestsQueueNotEmpty;
    
    QMutex producedQueueMutex;
    std::list<ProducedFrame> producedQueue;
    QWaitCondition producedQueueNotEmpty;
    
    
    bool processRunning;
    QWaitCondition processCondition;
    QMutex processMutex;
    
    bool mustQuit;
    mutable QMutex mustQuitMutex;
    QWaitCondition mustQuitCond;
    
    int abortRequested;
    QMutex abortRequestedMutex;

    /**
     * Single thread used by the ViewerCurrentFrameRequestScheduler when the global thread pool has reached its maximum
     * activity to keep the renders responsive even if the thread pool is choking.
     **/
    ViewerCurrentFrameRequestRendererBackup backupThread;
    
    ViewerCurrentFrameRequestSchedulerPrivate(ViewerInstance* viewer)
    : viewer(viewer)
    , requestsQueueMutex()
    , requestsQueue()
    , requestsQueueNotEmpty()
    , producedQueueMutex()
    , producedQueue()
    , producedQueueNotEmpty()
    , processRunning(false)
    , processCondition()
    , processMutex()
    , mustQuit(false)
    , mustQuitMutex()
    , mustQuitCond()
    , abortRequested(0)
    , abortRequestedMutex()
    , backupThread()
    {
        
    }
    
    bool checkForExit()
    {
        QMutexLocker k(&mustQuitMutex);
        if (mustQuit) {
            mustQuit = false;
            mustQuitCond.wakeAll();
            return true;
        }
        return false;
    }
    
    bool checkForAbortion()
    {
        QMutexLocker k(&abortRequestedMutex);
        if (abortRequested > 0) {
            abortRequested = 0;
            return true;
        }
        return false;
    }
    
    void notifyFrameProduced(const BufferableObjectList& frames,const boost::shared_ptr<RequestedFrame>& request, bool processRequest)
    {
        QMutexLocker k(&producedQueueMutex);
        ProducedFrame p;
        p.frames = frames;
        p.request = request;
        p.processRequest = processRequest;
        producedQueue.push_back(p);
        producedQueueNotEmpty.wakeOne();
    }
    
    void processProducedFrame(const BufferableObjectList& frames);

};


static void renderCurrentFrameFunctor(CurrentFrameFunctorArgs& args)
{
    
    ///The viewer always uses the scheduler thread to regulate the output rate, @see ViewerInstance::renderViewer_internal
    ///it calls appendToBuffer by itself
    StatusEnum stat;
    
    BufferableObjectList ret;
    try {
        if (!args.isRotoPaintRequest) {
            stat = args.viewer->renderViewer(args.view,QThread::currentThread() == qApp->thread(),false,args.viewerHash,args.canAbort,
                                             NodePtr(), true,args.args, args.request);
        } else {
            stat = args.viewer->getViewerArgsAndRenderViewer(args.time, args.canAbort, args.view, args.viewerHash, args.isRotoPaintRequest,
                                                             &args.args[0],&args.args[1]);
        }
    } catch (...) {
        stat = eStatusFailed;
    }
    
    if (stat == eStatusFailed) {
        ///Don't report any error message otherwise we will flood the viewer with irrelevant messages such as
        ///"Render failed", instead we let the plug-in that failed post an error message which will be more helpful.
        args.viewer->disconnectViewer();
        ret.clear();
    } else {
        for (int i = 0; i < 2; ++i) {
            if (args.args[i] && args.args[i]->params && args.args[i]->params->ramBuffer) {
                ret.push_back(args.args[i]->params);
            }
        }
    }
    
    if (args.request) {
        args.scheduler->notifyFrameProduced(ret, args.request, true);
    } else {
        
        assert(QThread::currentThread() == qApp->thread());
        args.scheduler->processProducedFrame(ret);
    }
    
    
}

ViewerCurrentFrameRequestScheduler::ViewerCurrentFrameRequestScheduler(ViewerInstance* viewer)
: QThread()
, _imp(new ViewerCurrentFrameRequestSchedulerPrivate(viewer))
{
    setObjectName("ViewerCurrentFrameRequestScheduler");
    QObject::connect(this, SIGNAL(s_processProducedFrameOnMainThread(BufferableObjectList)), this, SLOT(doProcessProducedFrameOnMainThread(BufferableObjectList)));
}

ViewerCurrentFrameRequestScheduler::~ViewerCurrentFrameRequestScheduler()
{
    
}


void
ViewerCurrentFrameRequestScheduler::run()
{
    boost::shared_ptr<RequestedFrame> firstRequest;
    for (;;) {
        
        if (_imp->checkForExit()) {
            return;
        }
        
        
        {
            QMutexLocker k(&_imp->requestsQueueMutex);
            if (!firstRequest && !_imp->requestsQueue.empty()) {
                firstRequest = _imp->requestsQueue.front();
                _imp->requestsQueue.pop_front();
            }
        }
        
        if (firstRequest) {
            
            ///Wait for the work to be done
            BufferableObjectList frames;
            {
                QMutexLocker k(&_imp->producedQueueMutex);
                
                std::list<ProducedFrame>::iterator found = _imp->producedQueue.end();
                for (std::list<ProducedFrame>::iterator it = _imp->producedQueue.begin(); it != _imp->producedQueue.end(); ++it) {
                    if (it->request == firstRequest) {
                        found = it;
                        break;
                    }
                }
                
                while (found == _imp->producedQueue.end()) {
					if (_imp->checkForExit()) {
						return;
					}
                    _imp->producedQueueNotEmpty.wait(&_imp->producedQueueMutex);
                    
                    for (std::list<ProducedFrame>::iterator it = _imp->producedQueue.begin(); it != _imp->producedQueue.end(); ++it) {
                        if (it->request == firstRequest) {
                            found = it;
                            break;
                        }
                    }
                }
                
                assert(found != _imp->producedQueue.end());
                found->request.reset();
                if (found->processRequest) {
                    firstRequest.reset();
                }
                frames = found->frames;
                _imp->producedQueue.erase(found);
            } // QMutexLocker k(&_imp->producedQueueMutex);
            if (_imp->checkForExit()) {
                return;
            }
            
            {
                _imp->viewer->setCurrentlyUpdatingOpenGLViewer(true);
                QMutexLocker processLocker(&_imp->processMutex);
                _imp->processRunning = true;
                Q_EMIT s_processProducedFrameOnMainThread(frames);
                
                while (_imp->processRunning && !_imp->checkForAbortion()) {
                    _imp->processCondition.wait(&_imp->processMutex);
                }
                _imp->viewer->setCurrentlyUpdatingOpenGLViewer(false);
            }
            
        } // if (firstRequest) {
        
        
        {
            QMutexLocker k(&_imp->requestsQueueMutex);
            while (!firstRequest && _imp->requestsQueue.empty()) {
                _imp->requestsQueueNotEmpty.wait(&_imp->requestsQueueMutex);
            }
        }
        
        ///If we reach here, we've been woken up because there's work to do
        
    } // for(;;)
}

void
ViewerCurrentFrameRequestScheduler::doProcessProducedFrameOnMainThread(const BufferableObjectList& frames)
{
    _imp->processProducedFrame(frames);
}

void
ViewerCurrentFrameRequestSchedulerPrivate::processProducedFrame(const BufferableObjectList& frames)
{
    assert(QThread::currentThread() == qApp->thread());
    
    //bool hasDoneSomething = false;
    for (BufferableObjectList::const_iterator it2 = frames.begin(); it2 != frames.end(); ++it2) {
        assert(*it2);
        boost::shared_ptr<UpdateViewerParams> params = boost::dynamic_pointer_cast<UpdateViewerParams>(*it2);
        assert(params);
        if (params && params->ramBuffer) {
            //hasDoneSomething = true;
            viewer->updateViewer(params);
        }
    }
    
    
    ///At least redraw the viewer, we might be here when the user removed a node upstream of the viewer.
    //if (hasDoneSomething) {
        viewer->redrawViewer();
    //}
    
    
    {
        QMutexLocker k(&processMutex);
        processRunning = false;
        processCondition.wakeOne();
    }
}

void
ViewerCurrentFrameRequestScheduler::abortRendering()
{
    if (!isRunning()) {
        return;
    }
    
    {
        QMutexLocker l2(&_imp->processMutex);
        _imp->processRunning = false;
        _imp->processCondition.wakeOne();
    }
    
    {
        QMutexLocker k(&_imp->abortRequestedMutex);
        ++_imp->abortRequested;
    }
}

void
ViewerCurrentFrameRequestScheduler::quitThread()
{
    if (!isRunning()) {
        return;
    }
    
    abortRendering();
    _imp->backupThread.quitThread();
    {
        QMutexLocker l2(&_imp->processMutex);
        _imp->processRunning = false;
        _imp->processCondition.wakeOne();
    }
    
    {
        QMutexLocker k(&_imp->mustQuitMutex);
        _imp->mustQuit = true;
        
        ///Push a fake request
        {
            QMutexLocker k(&_imp->requestsQueueMutex);
            _imp->requestsQueue.push_back(boost::shared_ptr<RequestedFrame>());
            _imp->notifyFrameProduced(BufferableObjectList(), boost::shared_ptr<RequestedFrame>(), true);
            _imp->requestsQueueNotEmpty.wakeOne();
        }
        while (_imp->mustQuit) {
            _imp->mustQuitCond.wait(&_imp->mustQuitMutex);
        }
    }
    wait();
    
    ///Clear all queues
    {
        QMutexLocker k(&_imp->requestsQueueMutex);
        _imp->requestsQueue.clear();
    }
    {
        QMutexLocker k(&_imp->producedQueueMutex);
        _imp->producedQueue.clear();
    }
}

bool
ViewerCurrentFrameRequestScheduler::hasThreadsWorking() const
{
    QMutexLocker k(&_imp->requestsQueueMutex);
    return _imp->requestsQueue.size() > 0;
}

void
ViewerCurrentFrameRequestScheduler::notifyFrameProduced(const BufferableObjectList& frames,const boost::shared_ptr<RequestedFrame>& request)
{
    _imp->notifyFrameProduced(frames, request, false);
}

void
ViewerCurrentFrameRequestScheduler::renderCurrentFrame(bool canAbort)
{
    int frame = _imp->viewer->getTimeline()->currentFrame();
    int viewsCount = _imp->viewer->getRenderViewsCount();
    int view = viewsCount > 0 ? _imp->viewer->getViewerCurrentView() : 0;
    U64 viewerHash = _imp->viewer->getHash();
    
    Natron::StatusEnum status[2] = {
        eStatusFailed, eStatusFailed
    };
    if (!_imp->viewer->getUiContext() || _imp->viewer->getApp()->isCreatingNode()) {
        return;
    }
    
    NodePtr isUserRotopainting = _imp->viewer->getApp()->getIsUserPainting();
    boost::shared_ptr<ViewerArgs> args[2];
    if (!isUserRotopainting) {
        for (int i = 0; i < 2; ++i) {
            args[i].reset(new ViewerArgs);
            status[i] = _imp->viewer->getRenderViewerArgsAndCheckCache_public(frame, false, canAbort, view, i, viewerHash,isUserRotopainting, true, args[i].get());
        }
        
        if (status[0] == eStatusFailed && status[1] == eStatusFailed) {
            _imp->viewer->disconnectViewer();
            return;
        } else if (status[0] == eStatusReplyDefault && status[1] == eStatusReplyDefault) {
            _imp->viewer->redrawViewer();
            return;
        }
        
        for (int i = 0; i < 2 ; ++i) {
            if (args[i]->params && args[i]->params->ramBuffer) {
                _imp->viewer->updateViewer(args[i]->params);
                args[i].reset();
            }
        }
        if ((!args[0] && !args[1]) ||
            (!args[0] && status[0] == eStatusOK && args[1] && status[1] == eStatusFailed) ||
            (!args[1] && status[1] == eStatusOK && args[0] && status[0] == eStatusFailed)) {
            _imp->viewer->redrawViewer();
            return;
        }
    } else {
        
    }
    CurrentFrameFunctorArgs functorArgs;
    functorArgs.viewer = _imp->viewer;
    functorArgs.time = frame;
    functorArgs.view = view;
    functorArgs.args[0] = args[0];
    functorArgs.args[1] = args[1];
    functorArgs.viewerHash = viewerHash;
    functorArgs.scheduler = _imp.get();
    functorArgs.canAbort = canAbort;
    functorArgs.isRotoPaintRequest = isUserRotopainting;
    
    if (appPTR->getCurrentSettings()->getNumberOfThreads() == -1) {
        renderCurrentFrameFunctor(functorArgs);
    } else {
        boost::shared_ptr<RequestedFrame> request(new RequestedFrame);
        request->id = 0;
        {
            QMutexLocker k(&_imp->requestsQueueMutex);
            _imp->requestsQueue.push_back(request);
            
            if (isRunning()) {
                _imp->requestsQueueNotEmpty.wakeOne();
            } else {
                start();
            }
        }
        functorArgs.request = request;
        
        /*
         * Let at least 1 free thread in the thread-pool to allow the renderer to use the thread pool if we use the thread-pool
         * with QtConcurrent::run
         */
        int maxThreads = QThreadPool::globalInstance()->maxThreadCount();
        
        //When painting, limit the number of threads to 1 to be sure strokes are painted in the right order
        if (_imp->viewer->getApp()->getIsUserPainting().get() != 0) {
            maxThreads = 1;
        }
        if (maxThreads == 1 || (QThreadPool::globalInstance()->activeThreadCount() >= maxThreads - 1)) {
            _imp->backupThread.renderCurrentFrame(functorArgs);
        } else {
            QtConcurrent::run(renderCurrentFrameFunctor,functorArgs);
        }
    }
    
}

struct ViewerCurrentFrameRequestRendererBackupPrivate
{
    QMutex requestsQueueMutex;
    std::list<CurrentFrameFunctorArgs> requestsQueue;
    QWaitCondition requestsQueueNotEmpty;
    
    bool mustQuit;
    mutable QMutex mustQuitMutex;
    QWaitCondition mustQuitCond;
    
    ViewerCurrentFrameRequestRendererBackupPrivate()
    : requestsQueueMutex()
    , requestsQueue()
    , requestsQueueNotEmpty()
    , mustQuit(false)
    , mustQuitMutex()
    , mustQuitCond()
    {
        
    }
    
    bool checkForExit()
    {
        QMutexLocker k(&mustQuitMutex);
        if (mustQuit) {
            mustQuit = false;
            mustQuitCond.wakeAll();
            return true;
        }
        return false;
    }

};

ViewerCurrentFrameRequestRendererBackup::ViewerCurrentFrameRequestRendererBackup()
: QThread()
, _imp(new ViewerCurrentFrameRequestRendererBackupPrivate())
{
    setObjectName("ViewerCurrentFrameRequestRendererBackup");
}

ViewerCurrentFrameRequestRendererBackup::~ViewerCurrentFrameRequestRendererBackup()
{
    
}

void
ViewerCurrentFrameRequestRendererBackup::renderCurrentFrame(const CurrentFrameFunctorArgs& args)
{
    {
        QMutexLocker k(&_imp->requestsQueueMutex);
        _imp->requestsQueue.push_back(args);
        
        if (isRunning()) {
            _imp->requestsQueueNotEmpty.wakeOne();
        } else {
            start();
        }
    }
}

void
ViewerCurrentFrameRequestRendererBackup::run()
{
    for (;;) {
        
        
        bool hasRequest = false;
        {
            CurrentFrameFunctorArgs firstRequest;
            {
                QMutexLocker k(&_imp->requestsQueueMutex);
                if (!_imp->requestsQueue.empty()) {
                    hasRequest = true;
                    firstRequest = _imp->requestsQueue.front();
                    if (!firstRequest.viewer) {
                        hasRequest = false;
                    }
                    _imp->requestsQueue.pop_front();
                }
            }
            
            if (_imp->checkForExit()) {
                return;
            }
            
            
            if (hasRequest) {
                renderCurrentFrameFunctor(firstRequest);
            }
        }
        
        {
            QMutexLocker k(&_imp->requestsQueueMutex);
            while (_imp->requestsQueue.empty()) {
                _imp->requestsQueueNotEmpty.wait(&_imp->requestsQueueMutex);
            }
        }
    }
}

void
ViewerCurrentFrameRequestRendererBackup::quitThread()
{
    if (!isRunning()) {
        return;
    }
    
    {
        QMutexLocker k(&_imp->mustQuitMutex);
        _imp->mustQuit = true;
        
        ///Push a fake request
        {
            QMutexLocker k(&_imp->requestsQueueMutex);
            _imp->requestsQueue.push_back(CurrentFrameFunctorArgs());
            _imp->requestsQueueNotEmpty.wakeOne();
        }
        
        while (_imp->mustQuit) {
            _imp->mustQuitCond.wait(&_imp->mustQuitMutex);
        }
    }
    wait();
    //clear all queues
    {
        QMutexLocker k(&_imp->requestsQueueMutex);
        _imp->requestsQueue.clear();
    }
}
