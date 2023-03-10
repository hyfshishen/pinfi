/*BEGIN_LEGAL 
Intel Open Source License 

Copyright (c) 2002-2017 Intel Corporation. All rights reserved.
 
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.  Redistributions
in binary form must reproduce the above copyright notice, this list of
conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.  Neither the name of
the Intel Corporation nor the names of its contributors may be used to
endorse or promote products derived from this software without
specific prior written permission.
 
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE INTEL OR
ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
END_LEGAL */
/*
 *
 * This tool collects an address trace of instructions that access memory
 * by filling a per-thread buffer. Each memory access of an instruction is recorded
 * as a MEMREF in the buffer (see MEMREF below).
 *

 * We do TRACE based instrumentation. At the top of the TRACE, we allocate
 * the maximum amount of space that might be needed for this trace in the
 * buffer. At each memory instruction, we just record the MEMREF at a
 * pre-determined offset in the buffer. If there are early exits from a trace,
 * then MEMREF records will not be placed in the buffer for INSs that follow the
 * early exit - We recognize these by initializing the buffer to EMPTY_ENTRY - so that the
 * entries in the buffer which were not filled in will have invalid values for both
 * pc and ea of any MEMREF
 *
 * We check if the buffer is full at the top of the trace. If it is full, we
 * process the buffer and reset the buffer pointer.
 *
 * Each application thread has it's own buffer - so multiple application threads do NOT
 * block each other on buffer accesses
 *
 */

#include <cstdio>
#include <set>
#include <list>
#include <iostream>
#include <fstream>
#include "pin.H"

namespace WIND
{
#include <windows.h>
}
using WIND::DWORD;

/*
 * Knobs for tool
 */

/*
 * Emit the address trace to the output file
 */
KNOB<BOOL>   KnobProcessBuffer(KNOB_MODE_WRITEONCE, "pintool", "process_buffs", "1", "process the filled buffers");

// 256*4096=1048576 - same size buffer in memtrace_simple, membuffer_simple, membuffer_multi
KNOB<UINT32> KnobNumBytesInBuffer(KNOB_MODE_WRITEONCE, "pintool", "num_bytes_in_buffer", "1048576", "number of bytes in buffer");
KNOB<UINT32> KnobNumBuffersPerAppThread(KNOB_MODE_WRITEONCE, "pintool", "num_buffers_per_app_thread", "3", "number of buffers per thread");
KNOB<UINT32> KnobNumProcessingThreads(KNOB_MODE_OVERWRITE, "pintool", "num_processing_threads", "3", "number of processing threads");
KNOB<BOOL> KnobStatistics(KNOB_MODE_WRITEONCE, "pintool", "statistics", "0", "gather statistics");
KNOB<BOOL> KnobLiteStatistics(KNOB_MODE_WRITEONCE, "pintool", "lite_statistics", "0", "gather lite statistics");
KNOB<string> KnobStatisticsOutputFile(KNOB_MODE_WRITEONCE, "pintool", "stat_file", "memtrace_threadpoolstats.out", "output file");
KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool", "o", "", "specify trace file name");
extern "C" UINT64 ReadProcessorCycleCounter();

std::ostream* TraceFile = 0;

/* Struct of memory reference written to the buffer
 */
struct MEMREF
{
    ADDRINT pc;  // pc (ip) of the instruction doing the memory reference
    ADDRINT ea;  // the address of the memory being referenced
};

// the Pin TLS slot that an application-thread will use to hold the APP_THREAD_BUFFER_HANDLER
// object that it owns
TLS_KEY appThreadRepresentitiveKey;

// Set of UIDs of all internal-tool threads
// We use std::set to verify that each thread has a unique UID
set<PIN_THREAD_UID> uidSet;
set<PIN_THREAD_UID> uidThreadStartedSet;

const int EMPTY_ENTRY = 0;


/* Pin registers that this tool allocates (per-thread) to manage the writing
 * to the per-thread buffer
 */
REG endOfTraceInBufferReg;
REG endOfBufferReg;

#include "threadpool_statistics.h"


/*
 * APP_THREAD_REPRESENTITVE
 * Each application thread, creates an object of this class and saves it in it's Pin TLS
 * slot (appThreadRepresentitiveKey).
 * This object is used when the BufferFull function is called. It provides the functionality
 * of:
 * 1) Allocating the buffers to be used
 * 2) Enquening a full buffer on the global full buffers list (fullBuffersListManager) so it
 *    will be processed by one of the internal-tool buffer processing threads.
 * 3) If there is no internal-tool buffer processing thread running yet
 *    then the ProcessBuffer is used to process the buffer by the application
 *    thread. It cannot wait for processing thread to start running
 *    because this may cause deadlock - because this app thread may be holding some OS
 *    resource that the processing thread needs in order to start running - e.g. the LoaderLock
 */
class APP_THREAD_REPRESENTITVE
{

  public:
    APP_THREAD_REPRESENTITVE(THREADID tid);
    ~APP_THREAD_REPRESENTITVE();

    /*
     * Pointer to beginning of the buffer
     */
    VOID * CurBuffer() { return _curBuffer; }

    /*
     * Pointer to end of the buffer
     */
    VOID * CurBufferEnd() { return ((CHAR *)_curBuffer) + KnobNumBytesInBuffer.Value(); }

    VOID * EnqueueFullAndGetNextToFill(VOID *endOfTraceInBuffer, ADDRINT *endOfBuffer);

    VOID * GetFreeBuffer();
    VOID ReturnFreeBuffer(VOID *buf, THREADID tid);

    /*
     * Analysis routine to record a MEMREF (pc, ea) in the buffer
     */
    static VOID PIN_FAST_ANALYSIS_CALL RecordMEMREFInBuffer(CHAR * endOfTraceInBuffer, ADDRINT offsetFromEndOfTrace, ADDRINT pc, ADDRINT ea);

    /*
     * Analysis routine called at beginning of each trace - it is the IF part of the IF THEN analysis routine pair
     * Return 0 if there is room in the buffer for the current trace
     *
     * @param[in]   endOfPreviousTraceInBuffer     Pointer to next entry in the buffer
     * @param[in]   bufferEnd                      Pointer to end of the buffer
     * @param[in]   sizeNeededForThisTraceInBuffer Number of bytes required by this TRACE
     */
    static ADDRINT PIN_FAST_ANALYSIS_CALL CheckIfNoSpaceForTraceInBuffer(CHAR * endOfPreviousTraceInBuffer, CHAR * bufferEnd, ADDRINT sizeNeededForThisTraceInBuffer);


    /*
     * Analysis routine called when the above IF part returns 1 - this the THEN part
     * The buffer does not have room for this trace, enques the buffer for processing
     * and gets a buffer from the free list to be used as the next buffer to fill
     */
    static VOID * PIN_FAST_ANALYSIS_CALL BufferFull(VOID *endOfTraceInBuffer, ADDRINT *endOfBuffer, ADDRINT tid);

    /*
     * Analysis routine called at beginning of each trace (after the IF-THEN)-
     * moves the endOfPreviousTraceInBuffer further down in the buffer to allocate space for all
     * the possible MEMREF elements that may be written by this trace
     */
    static VOID * PIN_FAST_ANALYSIS_CALL  AllocateSpaceForTraceInBuffer(CHAR * endOfPreviousTraceInBuffer,
                                                                        ADDRINT sizeNeededForThisTraceInBuffer);

    APP_THREAD_STATISTICS * Statistics() { return (&_appThreadStatistics);}


  private:

    static CHAR *AllocateBuffer();

    VOID AllocateFreeBuffers();
    VOID ReclaimFreeBuffers();

    /*
     * Return true if position in the buffer is empty
     */
    static BOOL EmptyEntry(CHAR * curMEMREFElement)
    {
        return *reinterpret_cast<ADDRINT *>(curMEMREFElement) == EMPTY_ENTRY;
    }


    VOID * _curBuffer;  // this is the current buffer being filled
    THREADID _myTid;

    WIND::HANDLE _freeBufferSem;

    APP_THREAD_STATISTICS _appThreadStatistics;
};

/*
 * ANALYSIS_CALL_INFO
 *
 * Analysis calls that must be inserted at an INS in the trace are recorded in an
 * ANALYSIS_CALL_INFO object
 *
 */
class ANALYSIS_CALL_INFO
{
  public:
      ANALYSIS_CALL_INFO(INS ins, UINT32 offsetFromTraceStartInBuffer, UINT32 memop) :
      _ins(ins),
     _offsetFromTraceStartInBuffer(offsetFromTraceStartInBuffer),
     _memop (memop)
    {
    }

    void InsertAnalysisCall(INT32 sizeofTraceInBuffer)
    {
        /* the place in the buffer where the MEMREF of this _ins should be recorded is
           computed by: (the value in endOfTraceInBufferReg)
                        -sizeofTraceInBuffer +  _offsetFromTraceStartInBuffer(of this _ins)
        */
        INS_InsertCall(_ins, IPOINT_BEFORE, AFUNPTR(APP_THREAD_REPRESENTITVE::RecordMEMREFInBuffer),
                       IARG_FAST_ANALYSIS_CALL,
                       IARG_REG_VALUE, endOfTraceInBufferReg,
                       IARG_ADDRINT, ADDRINT(_offsetFromTraceStartInBuffer - sizeofTraceInBuffer),
                       IARG_INST_PTR,
                       IARG_MEMORYOP_EA, _memop,
                       IARG_END);
    }

  private:
    INS _ins;
    INT32 _offsetFromTraceStartInBuffer;
    UINT32 _memop;
};




/*
 * TRACE_ANALYSIS_CALLS_NEEDED
 *
 * Information about what analysis calls must be inserted at INSs in the trace are recorded here
 * This info is held in a vector of ANALYSIS_CALL_INFO objects
 *
 */
class TRACE_ANALYSIS_CALLS_NEEDED
{

  public:
    TRACE_ANALYSIS_CALLS_NEEDED() : _currentOffsetFromTraceStartInBuffer(0),  _numAnalysisCallsNeeded(0)
    {}

    UINT32 NumAnalysisCallsNeeded() const { return _numAnalysisCallsNeeded; }

    UINT32 TotalSizeOccupiedByTraceInBuffer() const { return _currentOffsetFromTraceStartInBuffer; }


    /*
     * Record a call to store an address in the log
     */
    VOID RecordAnalysisCallNeeded(INS ins, UINT32 memop);

    /*
     * InsertAnalysisCall all the recorded necessary analysis calls into the trace
     */
    VOID InsertAnalysisCalls();



    private:

    INT32 _currentOffsetFromTraceStartInBuffer;
    INT32 _numAnalysisCallsNeeded;
    vector<ANALYSIS_CALL_INFO> _analysisCalls;
};


/*
 * BUFFER_LIST_MANAGER
 * This class implements buffer list management, both for the global fullBuffers list
 * and for the global free buffers list
 */
class BUFFER_LIST_MANAGER
{
  public:
    BUFFER_LIST_MANAGER(BOOL notifyExitRequired = FALSE);
    ~BUFFER_LIST_MANAGER();

    BOOL   PutBufferOnList(VOID *buf, VOID *endOfLastTraceInfBuffer,
                           // the application thread that puts this buffer on the list
                           APP_THREAD_REPRESENTITVE *appThreadRepresentitive,
                           /* thread Id of the thread making the call */
                           THREADID tid);
    VOID * GetBufferFromList(VOID **endOfLastTraceInfBuffer,
                             // the application thread that puts this buffer on the list
                             APP_THREAD_REPRESENTITVE **appThreadRepresentitive,
                             /* thread Id of the thread making the call */
                             THREADID tid);
    VOID NotifyExit();
    BUFFER_LIST_STATISTICS *Statistics() {return &_bufferListStatistics;}

  private:

    // structure of an element of the buffer list
    struct BUFFER_LIST_ELEMENT
    {
        VOID *buf;
        VOID *endOfLastTraceInfBuffer;
        // the application thread that puts this buffer on the list
        APP_THREAD_REPRESENTITVE *appThreadRepresentitive;
    };

    class SCOPED_LOCK {
        PIN_LOCK * _lock;
      public:
        SCOPED_LOCK(PIN_LOCK * lock, THREADID tid) : _lock(lock)
        {
            ASSERTX(_lock != NULL);
            PIN_GetLock(_lock, tid + 1);
        }
        ~SCOPED_LOCK()
        {
            PIN_ReleaseLock(_lock);
        }
    };

    WIND::HANDLE _exitEvent;
    WIND::HANDLE _bufferSem;
    PIN_LOCK _bufferListLock;
    list<BUFFER_LIST_ELEMENT> _bufferList;

    BUFFER_LIST_STATISTICS _bufferListStatistics;
};

// all full buffers are placed on this list by the app threads.
// the internal-tool threads pick them up from here,
// process them, and put them on the free list
BUFFER_LIST_MANAGER * fullBuffersListManager = NULL;

// all free buffers are placed on this list
// the app threads pick them up from here, set them as the buffer to be filled and when they become full
// process them on the full list
BUFFER_LIST_MANAGER freeBuffersListManager;

/*
 * Returns single instance of global buffer list manager that supports synchronization
 * and notified on process exit
 */
BUFFER_LIST_MANAGER * GetFullBuffersListManager()
{
    static BUFFER_LIST_MANAGER buffersListManager(TRUE);
    return &buffersListManager;
}

/*
 * Process the buffer
 */
VOID ProcessBuffer(VOID *curBuf, VOID *endOfTraceInBuffer, APP_THREAD_REPRESENTITVE *associatedAppThread)
{
    if (!KnobProcessBuffer)
    {
        return;
    }

    if (KnobStatistics)
    {
        associatedAppThread->Statistics()->StartCyclesProcessingBuffer();
    }

    struct MEMREF * memref = reinterpret_cast<struct MEMREF*>(curBuf);
    struct MEMREF * firstMemref = memref;
    UINT32 i = 0;
    while (memref < reinterpret_cast<struct MEMREF*>(endOfTraceInBuffer))
    {
        if (memref->pc!=0)
        {
           i++;
           firstMemref->pc += memref->pc + memref->ea;
           memref->pc = 0;
        }
        memref++;
    }

    associatedAppThread->Statistics()->AddNumElementsProcessed((UINT32)i);
    if (KnobStatistics)
    {
        associatedAppThread->Statistics()->UpdateCyclesProcessingBuffer();
    }
}

/*********** APP_THREAD_REPRESENTITVE implementation *******/

APP_THREAD_REPRESENTITVE::APP_THREAD_REPRESENTITVE(THREADID myTid)
    : _curBuffer(NULL), _myTid(myTid), _freeBufferSem(NULL)
{
    AllocateFreeBuffers();
}

APP_THREAD_REPRESENTITVE::~APP_THREAD_REPRESENTITVE()
{
    ReclaimFreeBuffers();
}


VOID PIN_FAST_ANALYSIS_CALL APP_THREAD_REPRESENTITVE::RecordMEMREFInBuffer(CHAR * endOfTraceInBuffer, ADDRINT offsetFromEndOfTrace, ADDRINT pc, ADDRINT ea)
{
    *reinterpret_cast<ADDRINT*>(endOfTraceInBuffer+offsetFromEndOfTrace) = pc;
    *reinterpret_cast<ADDRINT*>(endOfTraceInBuffer+offsetFromEndOfTrace+sizeof(ADDRINT)) = ea;
}

ADDRINT PIN_FAST_ANALYSIS_CALL APP_THREAD_REPRESENTITVE::CheckIfNoSpaceForTraceInBuffer(CHAR * endOfPreviousTraceInBuffer, CHAR * bufferEnd, ADDRINT sizeNeededForThisTraceInBuffer)
{
    return (endOfPreviousTraceInBuffer + sizeNeededForThisTraceInBuffer >= bufferEnd);
}

void * PIN_FAST_ANALYSIS_CALL  APP_THREAD_REPRESENTITVE::BufferFull(VOID *endOfTraceInBuffer, ADDRINT *endOfBuffer, ADDRINT tid)
{
    APP_THREAD_REPRESENTITVE * appThreadRepresentitive
        = static_cast<APP_THREAD_REPRESENTITVE*>(PIN_GetThreadData(appThreadRepresentitiveKey, tid));
    ASSERTX(appThreadRepresentitive != NULL);
    return (appThreadRepresentitive->EnqueueFullAndGetNextToFill(endOfTraceInBuffer, endOfBuffer));
}

void * PIN_FAST_ANALYSIS_CALL  APP_THREAD_REPRESENTITVE::AllocateSpaceForTraceInBuffer(CHAR * endOfPreviousTraceInBuffer,
                                                                        ADDRINT sizeNeededForThisTraceInBuffer)
{
    return (endOfPreviousTraceInBuffer + sizeNeededForThisTraceInBuffer);
}

VOID * APP_THREAD_REPRESENTITVE::EnqueueFullAndGetNextToFill(VOID *endOfTraceInBuffer, ADDRINT *endOfBuffer)
{
    _appThreadStatistics.IncrementNumBuffersFilled();

    // under some conditions the buffer is processed in this app thread
    if ( (fullBuffersListManager == NULL) // cannot wait for processing thread to start running
                                   // this may cause deadlock - because this app thread
                                   // may be holding some OS resource that the processing
                                   // needs to obtain in order to start - e.g. the LoaderLock
        // heuristic - no available free buffer, so process by this app thread

         // Otherwise put the fullBuf on the full buffers list, one the internal-tool processing
         // threads will pick it from there, process it, and then put it on this app-thread's
         // free buffer list
         || !fullBuffersListManager->PutBufferOnList(_curBuffer, endOfTraceInBuffer, this, _myTid)
         // Full buffers manager may not take the buffer if process exit is started.
       )
    { // process buffer in this app thread
        _appThreadStatistics.IncrementNumBuffersProcessedInAppThread();
        ProcessBuffer(_curBuffer, endOfTraceInBuffer, this);
        return _curBuffer;
    }

    _curBuffer = GetFreeBuffer();
    ASSERTX(_curBuffer != NULL);
    *endOfBuffer = (ADDRINT)CurBufferEnd();

    return _curBuffer;
}


CHAR * APP_THREAD_REPRESENTITVE::AllocateBuffer()
{
    return (new CHAR[KnobNumBytesInBuffer.Value()]);
}

VOID * APP_THREAD_REPRESENTITVE::GetFreeBuffer()
{
    WIND::DWORD status = WIND::WaitForSingleObject (_freeBufferSem, 10000);
    ASSERTX(status == WAIT_OBJECT_0);

    VOID *endOfTraceInBufferDummy;
    APP_THREAD_REPRESENTITVE *appThreadRepresentitiveDummy;
    VOID * buf = freeBuffersListManager.GetBufferFromList(&endOfTraceInBufferDummy, &appThreadRepresentitiveDummy, _myTid);
    ASSERTX(buf != NULL);
    return buf;
}

VOID APP_THREAD_REPRESENTITVE::ReturnFreeBuffer(VOID *buf, THREADID tid)
{
    freeBuffersListManager.PutBufferOnList(buf, NULL, this, tid);
    WIND::ReleaseSemaphore(_freeBufferSem, 1, NULL);
}

VOID APP_THREAD_REPRESENTITVE::AllocateFreeBuffers()
{
    // First allocate and assign initial buffer.
    _curBuffer = AllocateBuffer();

    // Now allocate rest of the KnobNumBuffersPerAppThread buffers and put them on the global free list
    for (int i = 0; i < KnobNumBuffersPerAppThread - 1; i++)
    {
        freeBuffersListManager.PutBufferOnList(AllocateBuffer(), NULL, this, _myTid);
    }
    _freeBufferSem = WIND::CreateSemaphore (NULL, KnobNumBuffersPerAppThread - 1, 0x7fffffff, NULL);
}

VOID APP_THREAD_REPRESENTITVE::ReclaimFreeBuffers()
{
    // Reclaim allocated buffers from global free list
    for (int i = 0; i < KnobNumBuffersPerAppThread - 1; i++)
    {
        VOID *endOfTraceInBufferDummy;
        APP_THREAD_REPRESENTITVE *appThreadRepresentitiveDummy;
        VOID * buf = freeBuffersListManager.GetBufferFromList(&endOfTraceInBufferDummy, &appThreadRepresentitiveDummy, _myTid);
        ASSERTX(buf != NULL);
        delete [] buf;

        WIND::DWORD status = WIND::WaitForSingleObject (_freeBufferSem, 10000);
        ASSERTX(status == WAIT_OBJECT_0);
    }
    WIND::CloseHandle(_freeBufferSem);
    _freeBufferSem = NULL;

    // Free current buffer.
    delete [] _curBuffer;
    _curBuffer = NULL;
}


/*********** BUFFER_LIST_MANAGER implementation *******/

BUFFER_LIST_MANAGER::BUFFER_LIST_MANAGER(BOOL notifyExitRequired) :
    _exitEvent(NULL)
{
    PIN_InitLock(&_bufferListLock);
    _bufferSem = WIND::CreateSemaphore (NULL, 0, 0x7fffffff, NULL);
    ASSERTX(_bufferSem != NULL);
    if (notifyExitRequired)
    {
        // Create manually reset event in non-signaled state.
        _exitEvent = WIND::CreateEvent(NULL, TRUE, FALSE, NULL);
        ASSERTX(_exitEvent != NULL);
    }
}

BUFFER_LIST_MANAGER::~BUFFER_LIST_MANAGER()
{
    if (_exitEvent != NULL)
    {
        WIND::CloseHandle(_exitEvent);
    }
    WIND::CloseHandle(_bufferSem);
}


BOOL  BUFFER_LIST_MANAGER::PutBufferOnList(VOID *buf, VOID *endOfLastTraceInfBuffer,
                                             /* the app thread that is placing the buffer on the list */
                                             APP_THREAD_REPRESENTITVE *appThreadRepresentitive,
                                             /* thread Id of the thread making the call */
                                             THREADID tid)
{
    if ((_exitEvent != NULL) &&
        (WIND::WaitForSingleObject(_exitEvent, 0) == WAIT_OBJECT_0))
    {
        // Exit event signaled. Do not add new buffers.
        return FALSE;
    }

    BUFFER_LIST_ELEMENT bufferListElement;

    bufferListElement.buf = buf;
    bufferListElement.endOfLastTraceInfBuffer = endOfLastTraceInfBuffer;
    bufferListElement.appThreadRepresentitive = appThreadRepresentitive;

    {
        SCOPED_LOCK lock(&_bufferListLock, tid);
        _bufferList.push_back(bufferListElement);
    }

    BOOL success = WIND::ReleaseSemaphore(_bufferSem, 1, NULL);

    return TRUE;
}

VOID *  BUFFER_LIST_MANAGER::GetBufferFromList(VOID **endOfLastTraceInfBuffer,
                                               /* the app thread that placed the buffer on the list */
                                               APP_THREAD_REPRESENTITVE **appThreadRepresentitive,
                                               /* thread Id of the thread making the call */
                                               THREADID tid)
{
    if (KnobStatistics)
    {
        if (_bufferList.empty())
        {
            _bufferListStatistics.IncrementNumTimesWaited();
        }
        _bufferListStatistics.StartCyclesWaitingForBuffer();
    }

    WIND::DWORD status;
    if (_exitEvent != NULL)
    {
        // Process buffers even after exit notification until the list is empty.
        WIND::HANDLE handles[2] = { _bufferSem, _exitEvent };
        status = WIND::WaitForMultipleObjects (2, handles, FALSE, INFINITE);
        if (status == (WAIT_OBJECT_0 + 1))
        {
            // Process exit flow started and there is no pending buffers to process.
            return NULL;
        }
    }
    else
    {
        status = WIND::WaitForSingleObject (_bufferSem, INFINITE);
    }
    // Otherwise wait should exit due to semaphore.
    if (status != WAIT_OBJECT_0)
    {
        *TraceFile << " WAIT returned " << status << ", last error " << WIND::GetLastError() << endl << flush;
        ASSERTX(status == WAIT_OBJECT_0);
    }

    if (KnobStatistics)
    {
        _bufferListStatistics.UpdateCyclesWaitingForBuffer();
    }

    {
        SCOPED_LOCK lock(&_bufferListLock, tid);
        ASSERTX(!_bufferList.empty());
        const BUFFER_LIST_ELEMENT &bufferListElement = (_bufferList.front());
        VOID* buf = bufferListElement.buf;
        *endOfLastTraceInfBuffer = bufferListElement.endOfLastTraceInfBuffer;
        *appThreadRepresentitive = bufferListElement.appThreadRepresentitive;
        _bufferList.pop_front();
        return buf;
    }
}

VOID BUFFER_LIST_MANAGER::NotifyExit()
{
    if (_exitEvent != NULL)
    {
        WIND::SetEvent(_exitEvent);
    }
}

/*********** BUFFER_LIST_MANAGER implementation END *******/


/*********** TRACE_ANALYSIS_CALLS_NEEDED implementation *******/

/*
 * We determined all the required instrumentation, insert the calls
 */
VOID TRACE_ANALYSIS_CALLS_NEEDED::InsertAnalysisCalls()
{
    for (vector<ANALYSIS_CALL_INFO>::iterator c = _analysisCalls.begin();
         c != _analysisCalls.end();
         c++)
    {
        c->InsertAnalysisCall(TotalSizeOccupiedByTraceInBuffer());
    }
}

/*
 * Record that we need to insert an analysis call to gather the MEMREF info for this ins
 */
void TRACE_ANALYSIS_CALLS_NEEDED::RecordAnalysisCallNeeded(INS ins, UINT32 memop)
{
    _analysisCalls.push_back(ANALYSIS_CALL_INFO(ins, _currentOffsetFromTraceStartInBuffer, memop));
    _currentOffsetFromTraceStartInBuffer += sizeof(MEMREF);
    _numAnalysisCallsNeeded++;
}


/*
  Called by the Trace instrumentation routine
 */
VOID DetermineBBLAnalysisCalls(BBL bbl, TRACE_ANALYSIS_CALLS_NEEDED * traceAnalysisCallsNeeded)
{
    // Log every memory references of the instruction
    for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins))
    {
        UINT32 memOperands = INS_MemoryOperandCount(ins);

        // Iterate over each memory operand of the instruction.
        for (UINT32 memOp = 0; memOp < memOperands; memOp++)
        {
            traceAnalysisCallsNeeded->RecordAnalysisCallNeeded(ins, memOp);
        }
    }
}

/*
  Trace instrumentation routine
 */
VOID TraceAnalysisCalls(TRACE trace, VOID *)
{
    // Go over all BBLs of the trace and for each BBL determine and record the INSs which need
    // to be instrumented - i.e. the ins requires and analysis call
    //
    TRACE_ANALYSIS_CALLS_NEEDED traceAnalysisCallsNeeded;
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
    {
        DetermineBBLAnalysisCalls(bbl, &traceAnalysisCallsNeeded);
    }

    // No addresses in this trace
    if (traceAnalysisCallsNeeded.NumAnalysisCallsNeeded() == 0)
    {
        return;
    }

    // Now we know how bytes the analysis calls of this trace will insert into the buffer
    //   Each analysis call inserts a MEMREF into the buffer

    // APP_THREAD_REPRESENTITVE::CheckIfNoSpaceForTraceInBuffer will determine if there are NOT enough available bytes in the buffer.
    // If there are NOT then it returns TRUE and the BufferFull function is called
    TRACE_InsertIfCall(trace, IPOINT_BEFORE, AFUNPTR(APP_THREAD_REPRESENTITVE::CheckIfNoSpaceForTraceInBuffer),
                       IARG_FAST_ANALYSIS_CALL,
                       IARG_REG_VALUE, endOfTraceInBufferReg, // previous trace
                       IARG_REG_VALUE, endOfBufferReg,
                       IARG_UINT32, traceAnalysisCallsNeeded.TotalSizeOccupiedByTraceInBuffer(),
                       IARG_END);
    TRACE_InsertThenCall(trace, IPOINT_BEFORE, AFUNPTR(APP_THREAD_REPRESENTITVE::BufferFull),
                         IARG_FAST_ANALYSIS_CALL,
                         IARG_REG_VALUE, endOfTraceInBufferReg,
                         IARG_REG_REFERENCE, endOfBufferReg,
                         IARG_THREAD_ID,
                         IARG_RETURN_REGS, endOfTraceInBufferReg,
                         IARG_END);
    TRACE_InsertCall(trace, IPOINT_BEFORE,  AFUNPTR(APP_THREAD_REPRESENTITVE::AllocateSpaceForTraceInBuffer),
                     IARG_FAST_ANALYSIS_CALL,
                     IARG_REG_VALUE, endOfTraceInBufferReg,
                     IARG_UINT32, traceAnalysisCallsNeeded.TotalSizeOccupiedByTraceInBuffer(),
                     IARG_RETURN_REGS, endOfTraceInBufferReg,
                     IARG_END);

    // Insert Analysis Calls for each INS on the trace that was recorded as needing one
    //   i.e. each INS that reads and/or writes memory
    traceAnalysisCallsNeeded.InsertAnalysisCalls();
}

VOID ThreadStart(THREADID tid, CONTEXT *ctxt, INT32 flags, VOID *v)
{
    // There is a new APP_THREAD_REPRESENTITVE for every thread
    APP_THREAD_REPRESENTITVE * appThreadRepresentitive = new APP_THREAD_REPRESENTITVE(tid);
    // All free buffers were allocated

    // A thread will need to look up its APP_THREAD_REPRESENTITVE, so save pointer in TLS
    PIN_SetThreadData(appThreadRepresentitiveKey, appThreadRepresentitive, tid);

    // Initialize endOfTraceInBufferReg to point at beginning of buffer
    PIN_SetContextReg(ctxt, endOfTraceInBufferReg, reinterpret_cast<ADDRINT>(appThreadRepresentitive->CurBuffer()));

    // Initialize endOfBufferReg to point at end of buffer
    PIN_SetContextReg(ctxt, endOfBufferReg, reinterpret_cast<ADDRINT>(appThreadRepresentitive->CurBufferEnd()));

    *TraceFile << "Thread " << tid << "started" << endl << flush;
}

VOID ThreadFini(THREADID tid, const CONTEXT *ctxt, INT32 code, VOID *v)
{
    APP_THREAD_REPRESENTITVE * appThreadRepresentitive
        = static_cast<APP_THREAD_REPRESENTITVE*>(PIN_GetThreadData(appThreadRepresentitiveKey, tid));
    ASSERTX(appThreadRepresentitive != NULL);

    appThreadRepresentitive->Statistics()->DumpNumBuffersFilled();
    overallStatistics.AccumulateAppThreadStatistics(appThreadRepresentitive->Statistics(), FALSE);
    if (KnobStatistics)
    {
        appThreadRepresentitive->Statistics()->Dump();
    }

    delete appThreadRepresentitive;

    PIN_SetThreadData(appThreadRepresentitiveKey, 0, tid);

    *TraceFile << "Thread " << tid << "finished" << endl << flush;
}

/*!
 * Process exit callback (unlocked).
 */
static VOID PrepareForFini(VOID *v)
{
    *TraceFile << "PrepareForFini" << endl << flush;

    if (fullBuffersListManager == NULL)
    {
        *TraceFile << "Internal threads were not yet started." << endl << flush;
        // Ensure that global buffer manager exists on process exit,
        // otherwise it could be created later without exit notification.
        fullBuffersListManager = GetFullBuffersListManager();
    }
    // Notify full buffers manager of application exit.
    // It will cause all internal threads to exit once they finish processing of pending buffers.
    fullBuffersListManager->NotifyExit();

    // Wait until all internal threads exit
    for (set<PIN_THREAD_UID>::iterator it = uidSet.begin(); it != uidSet.end(); ++it)
    {
        if (uidThreadStartedSet.find(*it) == uidThreadStartedSet.end())
        {
            *TraceFile << "Tool's thread " << *it << " did not started" << endl << flush;
            continue;
        }
        *TraceFile << "Waiting for exit of thread uid " << *it << " ." << endl << flush;
        INT32 threadExitCode;
        BOOL waitStatus = PIN_WaitForThreadTermination(*it, PIN_INFINITE_TIMEOUT, &threadExitCode);
        if (!waitStatus)
        {
            *TraceFile << "PIN_WaitForThreadTermination(secondary thread) failed" << endl << flush;
        }
        if (threadExitCode != 0)
        {
            *TraceFile << "Tool's thread exited abnormally" << endl << flush;
       }
    }
}


VOID Fini(INT32 code, VOID *v)
{
    overallStatistics.DumpNumBuffersFilled();
    if (fullBuffersListManager != NULL)
    {
        overallStatistics.IncorporateBufferStatistics(fullBuffersListManager->Statistics(), TRUE);
        overallStatistics.IncorporateBufferStatistics(fullBuffersListManager->Statistics(), FALSE);
    }
    if (KnobStatistics)
    {
        overallStatistics.Dump();
    }
}

/*!
 * Record the thread's uid
 */
static VOID RecordToolThreadCreated(PIN_THREAD_UID threadUid)
{
    BOOL insertStatus;
    insertStatus  =  (uidSet.insert(threadUid)).second;
    if (!insertStatus)
    {
        std::cerr << "UID is not unique" << endl << flush;
        PIN_ExitProcess(-1);
    }
}


/*
  Buffer Processing Thread's routine
*/
static VOID BufferProcessingThread(VOID * arg)
{
    THREADID myThreadId = PIN_ThreadId();

    BOOL insertStatus;
    insertStatus = (uidThreadStartedSet.insert(myThreadId)).second;
    if (!insertStatus)
    {
        std::cerr << "UID is not unique" << endl << flush;
        PIN_ExitProcess(-1);
    }

    fullBuffersListManager = GetFullBuffersListManager();

    for (;;)
    {
        VOID *endOfLastTraceInBuffer;
        APP_THREAD_REPRESENTITVE *appThreadRepresentitive;
        *TraceFile << "BufferProcessingThread tid " << myThreadId << "  GetBufferFromList" << endl << flush;
        VOID *buf = fullBuffersListManager->GetBufferFromList(&endOfLastTraceInBuffer,
                                                  &appThreadRepresentitive, myThreadId);
        if (buf == NULL)
        {
            *TraceFile << "BufferProcessingThread tid " << myThreadId << "  is exiting" << endl << flush;
            PIN_ExitThread(0);  // Doesn't return
            return;
        }
        *TraceFile << "BufferProcessingThread tid " << myThreadId << "  ProcessBuffer " << buf  << endl << flush;
        ProcessBuffer(buf, endOfLastTraceInBuffer, appThreadRepresentitive);
        *TraceFile << "BufferProcessingThread tid " << myThreadId << "  return buffer " << buf << " to appThreadRepresentitive " << appThreadRepresentitive  << endl << flush;

        appThreadRepresentitive->ReturnFreeBuffer(buf, myThreadId);
    }
}


INT32 Usage()
{
    std::cerr << "This tool demonstrates advanced pin-tool buffer managing in conjunction with internal-tool threads" << endl << flush;
    std::cerr << "The following command line options are available:" << endl << flush;
    std::cerr << "-num_buffers_per_app_thread <num>  :number of buffers to allocate per application thread,        default       3" << endl << flush;
    std::cerr << "-num_bytes_in_buffer <num>         :number of bytes allocated for each buffer,                   default 1048576n" << endl << flush;
    std::cerr << "-process_buffs <0 or 1>            :specify 0 to disable processing of the buffers,              default       1" << endl << flush;
    std::cerr << "-num_processing_threads <num>      :number of internal-tool buffer processing threads to create, default       3" << endl << flush;
    std::cerr << "-lite_statistics  <0 or 1>         :specify 1 to enable lite statistics gathering,               default       0" << endl << flush;
    std::cerr << "-heavy_statistics <0 or 1>         :specify 1 to enable heavy statistics gathering,              default       0" << endl << flush;

    return -1;
}

int main(int argc, char * argv[])
{
    // Initialize PIN library. Print help message if -h(elp) is specified
    // in the command line or the command line is invalid
    if( PIN_Init(argc,argv) )
    {
        return Usage();
    }

    if (KnobNumBuffersPerAppThread < 2)
    {
        *TraceFile << "Value of knob num_buffers_per_app_thread should be greater than 1" << endl << flush;
        return 2;
    }

    // Write to a file since cout and cerr maybe closed by the application
    TraceFile = KnobOutputFile.Value().empty() ? &cout : new std::ofstream(KnobOutputFile.Value().c_str());

    appThreadRepresentitiveKey = PIN_CreateThreadDataKey(0);

    // get the registers to be used in each thread for managing the
    // per-thread buffer
    endOfTraceInBufferReg = PIN_ClaimToolRegister();
    endOfBufferReg = PIN_ClaimToolRegister();

    if (! (REG_valid(endOfTraceInBufferReg) && REG_valid(endOfBufferReg)) )
    {
        *TraceFile << "Cannot allocate a scratch register." << endl << flush;
        return 1;
    }

    TRACE_AddInstrumentFunction(TraceAnalysisCalls, 0);
    PIN_AddThreadStartFunction(ThreadStart, 0);
    PIN_AddThreadFiniFunction(ThreadFini, 0);
    PIN_AddFiniFunction(Fini, 0);
    PIN_AddPrepareForFiniFunction(PrepareForFini, 0);

    /* It is safe to create internal threads in the tool's main procedure and spawn new
     * internal threads from existing ones. All other places, like Pin callbacks and
     * analysis routines in application threads, are not safe for creating internal threads.
    */
    // Spawn the tool's internal threads.
    for (int i = 0; i < KnobNumProcessingThreads; i++)
    {
        PIN_THREAD_UID threadUid;
        THREADID threadId
            = PIN_SpawnInternalThread(BufferProcessingThread,
                                      NULL,
                                      0,
                                      &threadUid);
        if (threadId == INVALID_THREADID)
        {
            std:cerr << "PIN_SpawnInternalThread(BufferProcessingThread) failed" << endl << flush;
            PIN_ExitProcess(-1);
        }
        *TraceFile << "created internal-tool BufferProcessingThread" << threadId << ", uid =  " << threadUid  << endl << flush;
        RecordToolThreadCreated(threadUid);
    }

    *TraceFile << "buffer size " << KnobNumBytesInBuffer.Value() << endl << flush;
    overallStatistics.Init();

    // Start the program, never returns
    PIN_StartProgram();

    return 0;
}
