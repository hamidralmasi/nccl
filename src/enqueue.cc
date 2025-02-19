/*************************************************************************
 * Copyright (c) 2017-2021, NVIDIA CORPORATION. All rights reserved.
 *
 * See LICENSE.txt for license information
 ************************************************************************/

#include "enqueue.h"
#include "argcheck.h"
#include "coll_net.h"
#include "gdrwrap.h"
#include "bootstrap.h"

#include <cstring> // std::memcpy

// Only generate inline kernels for LL
#define NCCL_FUNC5(func, algo, devredop, dtype) \
  (void*)NCCL_KERN_NAME(func, algo, LL, devredop, dtype), \
  (void*)NCCL_KERN_NAME(func, algo, LL, devredop, dtype), \
  (void*)NCCL_KERN_NAME(func, algo, LL, devredop, dtype)

#define NCCL_FUNC4(func, devredop, type) \
  (void*)NCCL_FUNC5(func, TREE,    devredop, type), \
  (void*)NCCL_FUNC5(func, RING,    devredop, type), \
  (void*)NCCL_FUNC5(func, COLLNET, devredop, type)

#if defined(__CUDA_BF16_TYPES_EXIST__)
// Must be consistent with ncclDataType_t
#define NCCL_FUNCS3A(func, devredop) \
  (void*)NCCL_FUNC4(func, devredop, int8_t), \
  (void*)NCCL_FUNC4(func, devredop, uint8_t), \
  (void*)NCCL_FUNC4(func, devredop, int32_t), \
  (void*)NCCL_FUNC4(func, devredop, uint32_t), \
  (void*)NCCL_FUNC4(func, devredop, int64_t), \
  (void*)NCCL_FUNC4(func, devredop, uint64_t), \
  (void*)NCCL_FUNC4(func, devredop, half), \
  (void*)NCCL_FUNC4(func, devredop, float), \
  (void*)NCCL_FUNC4(func, devredop, double), \
  (void*)NCCL_FUNC4(func, devredop, __nv_bfloat16)
#define NCCL_FUNCS3B(func, devredop) \
  (void*)NCCL_FUNC4(func, devredop, int8_t), \
  (void*)NCCL_FUNC4(func, devredop, int8_t), \
  (void*)NCCL_FUNC4(func, devredop, int8_t), \
  (void*)NCCL_FUNC4(func, devredop, int8_t), \
  (void*)NCCL_FUNC4(func, devredop, int8_t), \
  (void*)NCCL_FUNC4(func, devredop, int8_t), \
  (void*)NCCL_FUNC4(func, devredop, int8_t), \
  (void*)NCCL_FUNC4(func, devredop, int8_t), \
  (void*)NCCL_FUNC4(func, devredop, int8_t), \
  (void*)NCCL_FUNC4(func, devredop, int8_t)
#else
// Must be consistent with ncclDataType_t
#define NCCL_FUNCS3A(func, devredop) \
  (void*)NCCL_FUNC4(func, devredop, int8_t), \
  (void*)NCCL_FUNC4(func, devredop, uint8_t), \
  (void*)NCCL_FUNC4(func, devredop, int32_t), \
  (void*)NCCL_FUNC4(func, devredop, uint32_t), \
  (void*)NCCL_FUNC4(func, devredop, int64_t), \
  (void*)NCCL_FUNC4(func, devredop, uint64_t), \
  (void*)NCCL_FUNC4(func, devredop, half), \
  (void*)NCCL_FUNC4(func, devredop, float), \
  (void*)NCCL_FUNC4(func, devredop, double)
#define NCCL_FUNCS3B(func, devredop) \
  (void*)NCCL_FUNC4(func, devredop, int8_t), \
  (void*)NCCL_FUNC4(func, devredop, int8_t), \
  (void*)NCCL_FUNC4(func, devredop, int8_t), \
  (void*)NCCL_FUNC4(func, devredop, int8_t), \
  (void*)NCCL_FUNC4(func, devredop, int8_t), \
  (void*)NCCL_FUNC4(func, devredop, int8_t), \
  (void*)NCCL_FUNC4(func, devredop, int8_t), \
  (void*)NCCL_FUNC4(func, devredop, int8_t), \
  (void*)NCCL_FUNC4(func, devredop, int8_t)
#endif

// Must be consistent with ncclDevRedOp_t -- but we only generate kernel for sums.
#define NCCL_FUNCS2A(func) \
  NCCL_FUNCS3A(func, Sum), /*Sum*/ \
  NCCL_FUNCS3A(func, Sum), /*Prod*/ \
  NCCL_FUNCS3A(func, Sum), /*Max*/ \
  NCCL_FUNCS3A(func, Sum), /*Min*/ \
  NCCL_FUNCS3A(func, Sum), /*PreMulSum*/ \
  NCCL_FUNCS3A(func, Sum)  /*SumPostDiv*/
#define NCCL_FUNCS2B(func) \
  NCCL_FUNCS3B(func, Sum), /*Sum*/ \
  NCCL_FUNCS3B(func, Sum), /*Prod*/ \
  NCCL_FUNCS3B(func, Sum), /*Max*/ \
  NCCL_FUNCS3B(func, Sum), /*Min*/ \
  NCCL_FUNCS3B(func, Sum), /*PreMulSum*/ \
  NCCL_FUNCS3B(func, Sum)  /*SumPostDiv*/

// Must be consistent with the ncclFuncSet enum
static void* const ncclKerns[1+ncclNumTypes+NCCL_NUM_FUNCTIONS*ncclNumDevRedOps*ncclNumTypes*NCCL_NUM_ALGORITHMS*NCCL_NUM_PROTOCOLS] = {
  (void*)NCCL_KERN_NAME(SendRecv, RING, SIMPLE, Sum, int8_t),
  // We don't bake special kernels for the one-rank reductions
  /*int8*/(void*)NCCL_KERN_NAME(SendRecv, RING, SIMPLE, Sum, int8_t),
  /*uint8*/(void*)NCCL_KERN_NAME(SendRecv, RING, SIMPLE, Sum, int8_t),
  /*int32*/(void*)NCCL_KERN_NAME(SendRecv, RING, SIMPLE, Sum, int8_t),
  /*uint32*/(void*)NCCL_KERN_NAME(SendRecv, RING, SIMPLE, Sum, int8_t),
  /*int64*/(void*)NCCL_KERN_NAME(SendRecv, RING, SIMPLE, Sum, int8_t),
  /*uint64*/(void*)NCCL_KERN_NAME(SendRecv, RING, SIMPLE, Sum, int8_t),
  /*half*/(void*)NCCL_KERN_NAME(SendRecv, RING, SIMPLE, Sum, int8_t),
  /*float*/(void*)NCCL_KERN_NAME(SendRecv, RING, SIMPLE, Sum, int8_t),
  /*double*/(void*)NCCL_KERN_NAME(SendRecv, RING, SIMPLE, Sum, int8_t),
  #if defined(__CUDA_BF16_TYPES_EXIST__)
    /*bfloat16*/(void*)NCCL_KERN_NAME(SendRecv, RING, SIMPLE, Sum, int8_t),
  #endif
  NCCL_FUNCS2B(Broadcast),
  NCCL_FUNCS2A(Reduce),
  NCCL_FUNCS2B(AllGather),
  NCCL_FUNCS2A(ReduceScatter),
  NCCL_FUNCS2A(AllReduce)
};

// Determine the maximum kernel stack size of all CUDA kernels
size_t ncclKernMaxLocalSize() {
  ncclResult_t res = ncclSuccess;
  int numNcclKerns = sizeof(ncclKerns)/sizeof(ncclKerns[0]);
  cudaFuncAttributes attr = {0};
  size_t max = 0;
  for (int i = 0; i < numNcclKerns; i++) {
    CUDACHECKGOTO(cudaFuncGetAttributes(&attr, ncclKerns[i]), res, error);
    if (attr.localSizeBytes > max) max = attr.localSizeBytes;
  }

error:
  return (res != ncclSuccess) ? 0 : max;
}

/*****************************************************************************/
/*       Launch system : synchronization and CUDA kernel launch              */
/*****************************************************************************/

ncclResult_t ncclLaunchCooperativeKernelMultiDevice(struct cudaLaunchParams *paramsList, int* cudaDevs, int numDevices, int cgMode) {
#if CUDART_VERSION >= 9000
  if (cgMode & 0x01) {
    CUDACHECK(cudaLaunchCooperativeKernelMultiDevice(paramsList, numDevices,
            // These flags are to reduce the latency of using this API
            cudaCooperativeLaunchMultiDeviceNoPreSync|cudaCooperativeLaunchMultiDeviceNoPostSync));
    return ncclSuccess;
  }
#endif
  int savedDev;
  CUDACHECK(cudaGetDevice(&savedDev));
  for (int i = 0; i < numDevices; i++) {
    struct cudaLaunchParams* params = paramsList+i;
    CUDACHECK(cudaSetDevice(cudaDevs[i]));
    CUDACHECK(cudaLaunchKernel(params->func, params->gridDim, params->blockDim, params->args, params->sharedMem, params->stream));
  }
  CUDACHECK(cudaSetDevice(savedDev));
  return ncclSuccess;
}

static ncclResult_t getNextOp(struct ncclChannel* channel, struct ncclWork** work, struct ncclWorkElem* base) {
  if (channel->workCount == NCCL_MAX_OPS) {
    WARN("Too many aggregated operations on channel %d (%d max)", channel->id, NCCL_MAX_OPS);
    return ncclInvalidUsage;
  }
  int opIndex = channel->workFifoTail%NCCL_MAX_OPS;
  struct ncclWork* w = channel->workFifo+opIndex;
  struct ncclWorkElem* e = w->elems;
  volatile uint8_t* activePtr = (volatile uint8_t*)&e->active;
  while (activePtr[0] != 0) sched_yield();
  memset(w, 0, sizeof(struct ncclWork));
  // Initialize with work elem if provided
  if (base) memcpy(e, base, sizeof(struct ncclWorkElem));
  e->active = 1;
  channel->workFifoTail++;
  channel->workCount++;
  if (work) *work = w;
  return ncclSuccess;
}

static ncclResult_t setupLaunch(struct ncclQueueInfo* eqInfo, int usingCudaGraph) {
  ncclComm_t comm = eqInfo->comm;
  struct cudaLaunchParams* params = comm->myParams;

  // Only launch blocks where we have work to do.
  // This is not supported when we are in cudaGraph mode.
  // Because in cudaGraph mode the launch param needs to be determined
  // at capture time instead of launch time.
  if (!usingCudaGraph) {
    int nChannels = std::max(comm->nChannels, comm->p2pnChannels);
    for (int c=0; c<nChannels; c++) {
      if (comm->channels[c].workCount) params->gridDim.x = c+1;
    }
    eqInfo->maxChannels = params->gridDim.x;
  }

  // Set active = 2 for the last operation and add a no-op on empty channels (p2p case).
  for (int c=0; c<eqInfo->maxChannels; c++) {
    struct ncclChannel* channel = comm->channels+c;
    if (channel->workCount == 0) {
      struct ncclWork* w;
      NCCLCHECK(getNextOp(channel, &w, NULL));
      struct ncclWorkElem* e = w->elems;
      e->comm = comm->devComm;
      e->funcIndex = FUNC_INDEX_P2P;
      e->p2p.nThreads = 0;
    }
    channel->workFifo[(channel->workFifoTail-1)%NCCL_MAX_OPS].elems[0].active = 2;

    if (c == 0) {
      // As we inline the first coll directly, we can free it immediately.
      // Except P2P or aggregation or registration cases
      struct ncclWork* work = channel->workFifo+((channel->workFifoTail-channel->workCount)%NCCL_MAX_OPS);
      struct ncclWorkElem* elem = work->elems;
      if (elem->funcIndex != FUNC_INDEX_P2P && eqInfo->elemList->count() == 1 && elem->regUsed == 0)
        elem->active = 0;
    }

    if (channel->gdrMemDesc) {
      // GDRCOPY support
      uint64_t first = (channel->workFifoTail-channel->workCount)%NCCL_MAX_OPS;
      uint64_t nelems = channel->workCount;
      TRACE(NCCL_INIT, "GDRCOPY : copy workFifo %p to %p first %ld nelems %zi",
            channel->workFifo, channel->workFifoGdr, first, nelems);

      for (int i = 0; i < nelems; i++) {
        int elem = (first+i) % NCCL_MAX_OPS;
        // Copy Host workFifo to CUDA workFifo via the GDRCOPY mapping
        NCCLCHECK(ncclGdrCudaCopy(channel->gdrMemDesc, channel->workFifoGdr+elem, channel->workFifo+elem, 1));
      }
    }
  }

  return ncclSuccess;
}

ncclResult_t ncclCpuBarrierIn(struct ncclComm* comm, int* isLast) {
  volatile int* ptr = (volatile int*)(comm->intraBarrier+comm->intraPhase);
  int val = *ptr;
  bool done = false;
  while (done == false) {
    if (val >= comm->intraRanks) {
      WARN("Trying to launch too many work elements, max is %d", NCCL_MAX_OPS);
      return ncclInvalidUsage;
    }
    if (val+1 == comm->intraRanks) {
      // Reset the barrier.
      comm->intraBarrier[comm->intraPhase^1] = 0;
      *isLast = 1;
      return ncclSuccess;
    }
    done = __sync_bool_compare_and_swap(ptr, val, val+1);
    val++;
  }
  *isLast = 0;
  return ncclSuccess;
}

ncclResult_t ncclCpuBarrierLast(struct ncclComm* comm) {
  volatile int* ptr = (volatile int*)(comm->intraBarrier+comm->intraPhase);
  int val = *ptr;
  if (__sync_bool_compare_and_swap(ptr, val, val+1) != true) {
    WARN("Trying to launch too many work elements, max is %d", NCCL_MAX_OPS);
    return ncclInternalError;
  }
  return ncclSuccess;
}

ncclResult_t ncclCpuBarrierOut(struct ncclComm* comm) {
  volatile int* ptr = (volatile int*)(comm->intraBarrier+comm->intraPhase);
  while (*ptr < comm->intraRanks) pthread_yield();
  comm->intraPhase ^= 1;
  return ncclSuccess;
}

ncclResult_t ncclLaunchBarrier(struct ncclComm* comm) {
  struct cudaLaunchParams* params = comm->myParams;
  if (params->gridDim.x == 0) return ncclSuccess;

  // Use internal NCCL stream for CGMD/GROUP launch if required or if the user stream is NULL
  if (comm->launchMode == ncclComm::GROUP &&
      (comm->groupCudaStream ||
       comm->userStream == cudaStreamDefault ||
       comm->userStream == cudaStreamLegacy ||
       comm->userStream == cudaStreamPerThread)) {
    // Enqueue event in user stream
    CUDACHECK(cudaEventRecord(comm->intDoneEvent, comm->userStream));
    // Create dependency between user stream and internal NCCL stream
    CUDACHECK(cudaStreamWaitEvent(comm->groupStream, comm->intDoneEvent, 0));
    params->stream = comm->groupStream;
  } else {
    if (comm->userStream != params->stream && !comm->usingCudaGraph) {
      // Stream changed from last call, create dependency against last NCCL kernel launch
      CUDACHECK(cudaStreamWaitEvent(comm->userStream, comm->doneEvent, 0));
    }
    params->stream = comm->userStream;
  }

  if (comm->launchMode == ncclComm::GROUP) {
    int isLast = 0;
    NCCLCHECK(ncclCpuBarrierIn(comm, &isLast));
    if (isLast) {
      // I'm the last. Launch all operations.
      NCCLCHECK(ncclLaunchCooperativeKernelMultiDevice(comm->intraParams, comm->intraCudaDevs, comm->intraRanks, *comm->intraCGMode));
      NCCLCHECK(ncclCpuBarrierLast(comm));
    }
  }
  return ncclSuccess;
}

ncclResult_t ncclLaunchKernel(ncclComm_t comm) {
  struct cudaLaunchParams *params = comm->myParams;
  if (params->gridDim.x == 0) return ncclSuccess;

  // We can't print the CG mode before the first barrier happened.
  if (comm->rank == 0 && *comm->intraCGMode & 0x10) {
    *comm->intraCGMode ^= 0x10;
    INFO(NCCL_INIT,"Launch mode %s%s%s",
        comm->launchMode == ncclComm::GROUP ? "Group" : "Parallel",
        *comm->intraCGMode ? "/CGMD" : "",
        (comm->launchMode == ncclComm::GROUP && comm->groupCudaStream) ? "/Stream" : "");
  }

  if (comm->launchMode == ncclComm::GROUP) {
    NCCLCHECK(ncclCpuBarrierOut(comm));
  } else {
    CUDACHECK(cudaLaunchKernel(params->func, params->gridDim, params->blockDim, params->args, params->sharedMem, params->stream));
  }

  return ncclSuccess;
}

static ncclResult_t ncclLaunchProxy(struct ncclQueueInfo* eqInfo) {
  // Start the network proxies as soon as the kernel has been launched. We can't
  // perform any CUDA call between the two or having a cudaFree between the CUDA
  // launch and the ncclProxyStart call could cause a deadlock.
  // Also, starting the proxies after the CUDA launch seems to be better for
  // performance (latency).
  ncclComm_t comm = eqInfo->comm;
  if (eqInfo->maxChannels == 0) return ncclSuccess;

  for (int r=0; r<eqInfo->maxChannels; r++) {
    struct ncclChannel* channel = comm->channels+r;
    channel->workCount = 0;
    channel->totalSize = 0;
  }
  comm->lastChannel = 0;
  NCCLCHECK(ncclProxyStart(comm));
  return ncclSuccess;
}

ncclResult_t ncclRecordEvents(ncclComm_t comm) {
  struct cudaLaunchParams *params = comm->myParams;

  // Enqueue event after NCCL kernel (only in non-graph mode)
  if (!comm->usingCudaGraph) CUDACHECK(cudaEventRecord(comm->doneEvent, params->stream));
  // Use internal NCCL stream for CGMD/GROUP launch if required or if the user stream is NULL
  if (comm->launchMode == ncclComm::GROUP &&
      (comm->groupCudaStream ||
       comm->userStream == cudaStreamDefault ||
       comm->userStream == cudaStreamLegacy ||
       comm->userStream == cudaStreamPerThread)) {
    CUDACHECK(cudaEventRecord(comm->intDoneEvent, params->stream));
    // Create dependency between NCCL internal stream and user stream
    CUDACHECK(cudaStreamWaitEvent(comm->userStream, comm->intDoneEvent, 0));
  }
  return ncclSuccess;
}

ncclResult_t ncclLaunchReset(ncclComm_t comm) {
  comm->userStreamSet = false;

  // We are finishing capture of the current launch
  // But we need to keep the current enqueue info for CUDA graph
  // Thus we need to creating a new enqueue info for the next run
  if (comm->usingCudaGraph) {
    NCCLCHECK(ncclCreateQueueInfo(&comm->enqueueInfo, comm));
  } else {
    // If not in CUDA graph mode, we reuse the same info space
    NCCLCHECK(ncclResetQueueInfo(comm->enqueueInfo));
  }

  struct cudaLaunchParams *params = comm->myParams;
  params->gridDim.x = params->blockDim.x = 0;
  params->func = NULL;

  // Reset launch mode to GROUP if changed
  if (comm->launchMode == ncclComm::GROUP_GRAPH) comm->launchMode = ncclComm::GROUP;
  comm->usingCudaGraph = 0;

  return ncclSuccess;
}

/*****************************************************************************/
/* Enqueueing system : computation of kernel and proxy operations parameters */
/*****************************************************************************/

static inline ncclResult_t getCollNetSupport(struct ncclInfo* info, int* collNetTypeSupport) {
  if (info->comm->collNetSupport > 0) {
    ncclRedOp_t netOp = info->op == ncclAvg || info->op >= ncclNumOps ? ncclSum : info->op;
    NCCLCHECK(collNetReduceSupport(info->datatype, netOp, collNetTypeSupport));
  } else {
    *collNetTypeSupport = 0;
  }
  return ncclSuccess;
}

static ncclResult_t getAlgoInfo(struct ncclInfo* info, int collNetTypeSupport, int numPipeOps) {
  struct ncclComm* comm = info->comm;
  if (comm->nRanks == 1) {
    info->algorithm = NCCL_ALGO_RING;
    info->protocol = NCCL_PROTO_SIMPLE;
  }
  else {
    float minTime = 3600000000.0; // Hopefully no operation will take an hour to complete.
    // Find algorithm / protocol.
    info->algorithm = -1;
    info->protocol = -1;
    int nAlgos = NCCL_NUM_ALGORITHMS;
    for (int a=0; a<nAlgos; a++) {
      if (a == NCCL_ALGO_COLLNET && collNetTypeSupport != 1) continue;
      for (int p=0; p<NCCL_NUM_PROTOCOLS; p++) {
        float time;
        NCCLCHECK(ncclTopoGetAlgoTime(info, a, p, numPipeOps, &time));
        if (time >= 0 && time < minTime) {
          info->algorithm = a;
          info->protocol = p;
          minTime = time;
        }
      }
    }
    if (info->algorithm == -1 || info->protocol == -1) {
      WARN("Error : no algorithm/protocol available");
      return ncclInternalError;
    }
    //if (comm->rank == 0) INFO(NCCL_TUNING, "%ld Bytes -> Algo %d proto %d time %f", info->nBytes, info->algorithm, info->protocol, minTime);
    TRACE(NCCL_COLL, "%ld Bytes -> Algo %d proto %d time %f", info->nBytes, info->algorithm, info->protocol, minTime);
  }

  int nc = (info->nChannels > 0) ? info->nChannels : comm->nChannels;
  int nt = comm->maxThreads[info->algorithm][info->protocol];
  int threadThreshold = comm->threadThresholds[info->algorithm][info->protocol];
  if (info->algorithm == NCCL_ALGO_COLLNET) {
    int ncSwitch = 16;
    bool flag = true;
    while (ncSwitch >= 1 && flag) {
      while ((flag = info->nBytes < nc*nt*info->comm->channels[0].collTree.nHeads*threadThreshold) && nc > ncSwitch) {
        if (nc == ncSwitch+ncSwitch/2) threadThreshold /= 2;
        nc--;
      }
      ncSwitch /= 2;
    }
  } else {
    while (info->nBytes < nc*nt*threadThreshold) {
      if (nc >= 2) nc--;
      else if ((nt % 128) == 0) nt/=2;
      else break;
    }
  }
  if (info->protocol == NCCL_PROTO_SIMPLE) {
    nt += WARP_SIZE; // Extra warp for sync
    if (info->algorithm == NCCL_ALGO_TREE) nt += 3*WARP_SIZE;
    if (info->algorithm == NCCL_ALGO_COLLNET) nt += 3*WARP_SIZE;
  }
  info->nChannels = nc;
  info->nThreads = nt;
  return ncclSuccess;
}

static ncclResult_t getPatternInfo(struct ncclInfo* info) {
  switch (info->coll) {
    case ncclFuncBroadcast:
      info->pattern = info->algorithm == NCCL_ALGO_TREE ? ncclPatternTreeDown : ncclPatternPipelineFrom; break;
    case ncclFuncReduce:
      info->pattern = info->algorithm == NCCL_ALGO_TREE ? ncclPatternTreeUp : ncclPatternPipelineTo; break;
    case ncclFuncReduceScatter:
    case ncclFuncAllGather:
      info->pattern = ncclPatternRing; break;
    case ncclFuncAllReduce:
      info->pattern = info->algorithm == NCCL_ALGO_COLLNET ? ncclPatternCollTreeUpDown : info->algorithm == NCCL_ALGO_TREE ? ncclPatternTreeUpDown : ncclPatternRingTwice; break;
    default:
      WARN("Unknown pattern for collective %d algorithm %d", info->coll, info->algorithm);
      return ncclInternalError;
  }
  return ncclSuccess;
}

static ncclResult_t getLoopInfo(struct ncclInfo* info) {
  switch (info->pattern) {
    case ncclPatternTreeUp:
    case ncclPatternTreeDown:
    case ncclPatternTreeUpDown:
    case ncclPatternPipelineFrom:
    case ncclPatternPipelineTo:
      info->nstepsPerLoop = info-> nchunksPerLoop = 1; break;
    case ncclPatternCollTreeUpDown:
      info->nstepsPerLoop = 1; info->nchunksPerLoop = info->comm->channels[0].collTree.nHeads; break;
    case ncclPatternRing:
      info->nstepsPerLoop = info->comm->nRanks-1; info->nchunksPerLoop = info->comm->nRanks; break;
    case ncclPatternRingTwice:
      info->nstepsPerLoop = 2*(info->comm->nRanks-1); info->nchunksPerLoop = info->comm->nRanks; break;
    default:
      WARN("Unknown pattern %d", info->pattern);
      return ncclInternalError;
  }
  return ncclSuccess;
}

static ncclResult_t computeColl(struct ncclInfo* info /* input */, struct ncclWorkElem* work, struct ncclProxyArgs* proxyArgs /* output */) {
  work->comm = info->comm->devComm;

  int collNetTypeSupport = 0;
  // Check whether algo and proto have been preset
  if (info->nChannels > 0 && info->nThreads > 0) goto comp_next;
  NCCLCHECK(getCollNetSupport(info, &collNetTypeSupport));
  NCCLCHECK(getAlgoInfo(info, collNetTypeSupport, 1));

comp_next:
  // Set nstepsPerLoop and nchunksPerLoop
  NCCLCHECK(getPatternInfo(info));
  NCCLCHECK(getLoopInfo(info));

  work->sendbuff = info->sendbuff;
  work->recvbuff = info->recvbuff;
  work->coll.root = info->root;
  work->coll.count = info->count;
  work->coll.nChannels = info->nChannels;
  work->nThreads = info->nThreads;
  work->coll.redOpArg = info->opFull.scalarArg;
  work->redOpArgIsPtr = info->opFull.scalarArgIsPtr;

  if (info->comm->nRanks == 1) {
    // one-rank reduce index
    work->funcIndex = 1 + int(info->datatype);
    return ncclSuccess;
  }

  work->funcIndex = FUNC_INDEX(info->coll, info->opFull.op, info->datatype, info->algorithm, info->protocol);

  int stepSize   = info->comm->buffSizes[info->protocol]/NCCL_STEPS;
  int chunkSteps = (info->protocol == NCCL_PROTO_SIMPLE && info->algorithm == NCCL_ALGO_RING) ? info->chunkSteps : 1;
  int sliceSteps = (info->protocol == NCCL_PROTO_SIMPLE && info->algorithm == NCCL_ALGO_RING) ? info->sliceSteps : 1;
  int chunkSize  = stepSize*chunkSteps;

  // Compute lastChunkSize
  if (info->algorithm == NCCL_ALGO_TREE && info->protocol == NCCL_PROTO_SIMPLE) {
    if (info->pattern == ncclPatternTreeUpDown) {
      // Optimize chunkSize / nSteps
      while (info->nBytes / (info->nChannels*chunkSize) < info->comm->channels[0].tree.depth*8 && chunkSize > 131072) chunkSize /= 2;
      while (info->nBytes / (info->nChannels*chunkSize) < info->comm->channels[0].tree.depth*4 && chunkSize > 65536) chunkSize /= 2;
      while (info->nBytes / (info->nChannels*chunkSize) < info->comm->channels[0].tree.depth && chunkSize > 32768) chunkSize /= 2;
    }
    // Use lastChunkSize as chunkSize
    work->coll.lastChunkSize = chunkSize / ncclTypeSize(info->datatype);
  } else if (info->algorithm == NCCL_ALGO_COLLNET && info->protocol == NCCL_PROTO_SIMPLE) {
    // Optimize chunkSize / nSteps
    while (info->nBytes / (info->nChannels*info->comm->channels[0].collTree.nHeads*chunkSize) < info->comm->channels[0].collTree.depth*64 && chunkSize > 131072) chunkSize /= 2;
    while (info->nBytes / (info->nChannels*info->comm->channels[0].collTree.nHeads*chunkSize) < info->comm->channels[0].collTree.depth*8 && chunkSize > 65536) chunkSize /= 2;
    while (info->nBytes / (info->nChannels*info->comm->channels[0].collTree.nHeads*chunkSize) < info->comm->channels[0].collTree.depth*8 && chunkSize > 32768) chunkSize /= 2;
    // Use lastChunkSize as chunkSize
    work->coll.lastChunkSize = chunkSize / ncclTypeSize(info->datatype);
    // Set direct direction for broadcast-gather (read or write)
    work->direct = (info->nBytes / info->nChannels <= 1024*1024) ? NCCL_DIRECT_WRITE : NCCL_DIRECT_READ;
  } else if (info->protocol == NCCL_PROTO_LL) {
    const ssize_t sliceSize = stepSize*sizeof(uint64_t)/sizeof(union ncclLLFifoLine);
    const ssize_t loopSize = info->nChannels*info->nchunksPerLoop*(ssize_t)sliceSize;
    work->coll.lastChunkSize = DIVUP((info->nBytes-(info->nBytes/loopSize)*loopSize), info->nChannels*info->nchunksPerLoop);
    ALIGN_SIZE(work->coll.lastChunkSize, info->nThreads*sizeof(uint64_t));
    work->coll.lastChunkSize /= ncclTypeSize(info->datatype);
  } else if (info->algorithm == NCCL_ALGO_TREE && info->protocol == NCCL_PROTO_LL128) {
    int nNodes = info->comm->nNodes;
    float ppn = info->comm->nRanks / (float)nNodes;
    float nstepsLL128 = 1+log2i(nNodes) + 0.1*ppn;
    while (info->nBytes / (info->nChannels*chunkSize) < nstepsLL128*64/ppn && chunkSize > 131072) chunkSize /= 2;
    while (info->nBytes / (info->nChannels*chunkSize) < nstepsLL128*16/ppn && chunkSize > 32768) chunkSize /= 2;
    // Use lastChunkSize as chunkSize
    work->coll.lastChunkSize = chunkSize*NCCL_LL128_DATAELEMS/(NCCL_LL128_LINEELEMS*ncclTypeSize(info->datatype));
  }

  // Compute nSteps for proxies
  int chunkEffectiveSize = chunkSize;
  if (info->protocol == NCCL_PROTO_LL) chunkEffectiveSize /= 2;
  if (info->protocol == NCCL_PROTO_LL128) chunkEffectiveSize = (chunkSize / NCCL_LL128_LINEELEMS) * NCCL_LL128_DATAELEMS;
  //if (info->comm->rank == 0) printf("Coll %d, size %ld -> %dx%d, chunkSize %d (algo %d proto%d)\n", info->coll, info->nBytes, info->nChannels, info->nThreads, chunkSize, info->algorithm, info->protocol);
  int nLoops = (int)(DIVUP(info->nBytes, (((size_t)(info->nChannels))*info->nchunksPerLoop*chunkEffectiveSize)));
  proxyArgs->subs[0].nsteps = info->nstepsPerLoop * nLoops * chunkSteps;
  proxyArgs->sliceSteps = sliceSteps;
  proxyArgs->chunkSteps = chunkSteps;
  proxyArgs->chunkSize = chunkSize;
  proxyArgs->protocol = info->protocol;
  proxyArgs->dtype = info->datatype;
  proxyArgs->redOp = info->algorithm != NCCL_ALGO_COLLNET ? ncclNumOps : // Only set redOp when using CollNet
                     info->opFull.op==ncclDevPreMulSum || info->opFull.op==ncclDevSumPostDiv ? ncclSum : // Network sees avg as sum
                     info->op;
  proxyArgs->pattern = info->pattern;
  proxyArgs->root = info->root;
  // This is used by P2P to reduce the receive buffer size. We don't use it in collectives
  // because some protocols need to transmit more than the total size, plus they sometimes
  // round up
  proxyArgs->subs[0].recvbytes = stepSize*proxyArgs->sliceSteps;

  TRACE(NCCL_COLL,"OpCount %lx slicesteps %d spl %d cpl %d nbytes %zi -> protocol %d nchannels %d nthreads %d, nloops %d nsteps %d chunksize %d comm %p",
      proxyArgs->opCount, sliceSteps, info->nstepsPerLoop, info->nchunksPerLoop, info->nBytes, info->protocol, info->nChannels, info->nThreads,
      nLoops, proxyArgs->subs[0].nsteps, chunkSize, info->comm);
  return ncclSuccess;
}

static ncclResult_t checkSetStream(struct ncclInfo* info) {
 if (info->comm->userStreamSet == false) {
    info->comm->userStream = info->stream;
    info->comm->userStreamSet = true;
  } else if (info->stream != info->comm->userStream) {
    WARN("Error : mixing different streams within a group call is not supported.");
    return ncclInvalidUsage;
  }
  return ncclSuccess;
}

struct ncclBuffRegHandle {
  cudaIpcMemHandle_t sendBuffIpc;
  cudaIpcMemHandle_t recvBuffIpc;
  ssize_t sendBuffOffset;
  ssize_t recvBuffOffset;
};

// Register input and output buffers
// Exchange with ranks on the same host
static ncclResult_t ncclRegBuffAndExchange(struct ncclInfo* info, struct ncclBuffRegInfo* regInfo) {
  ncclComm_t comm = info->comm;
  if (comm->localRanks == 1) return ncclSuccess;
  if (comm->pfnCuMemGetAddressRange == NULL) return ncclSuccess;  // CUDA toolkit or driver version too old

  struct ncclBuffRegHandle regHandles[NCCL_MAX_INTRA_RANKS];
  // Get IPC handles
  // Note: the handle only corresponds to the base address of the allocation
  CUDACHECK(cudaIpcGetMemHandle(&regHandles[comm->intraNodeRank].sendBuffIpc, (void*)info->sendbuff));
  CUDACHECK(cudaIpcGetMemHandle(&regHandles[comm->intraNodeRank].recvBuffIpc, (void*)info->recvbuff));
  // Get offset of user buffer within allocation
  void* baseAddr;
  size_t size;
  CUDACHECK(comm->pfnCuMemGetAddressRange(&baseAddr, &size, (void*)info->sendbuff));
  regHandles[comm->intraNodeRank].sendBuffOffset = (char*)info->sendbuff - (char*)baseAddr;
  CUDACHECK(comm->pfnCuMemGetAddressRange(&baseAddr, &size, (void*)info->recvbuff));
  regHandles[comm->intraNodeRank].recvBuffOffset = (char*)info->recvbuff - (char*)baseAddr;
  TRACE(NCCL_COLL, "Base %p size %lu offset %ld", baseAddr, size, regHandles[comm->intraNodeRank].recvBuffOffset);

  // Exchange handles within node
  NCCLCHECK(bootstrapIntraNodeAllGather(comm->bootstrap, comm->intraNodeGlobalRanks, comm->intraNodeRank, comm->localRanks, regHandles, sizeof(struct ncclBuffRegHandle)));
  // Open handles at local process
  for (int i=0; i<comm->localRanks; i++) {
    if (i == comm->intraNodeRank) {
      regInfo->sendbuffsBase[i] = regInfo->recvbuffsBase[i] = NULL;
      continue;
    }
    CUDACHECK(cudaIpcOpenMemHandle(regInfo->sendbuffsBase+i, regHandles[i].sendBuffIpc, cudaIpcMemLazyEnablePeerAccess));
    CUDACHECK(cudaIpcOpenMemHandle(regInfo->recvbuffsBase+i, regHandles[i].recvBuffIpc, cudaIpcMemLazyEnablePeerAccess));
    // Get real address of buffer
    regInfo->sendbuffs[i] = (char*)regInfo->sendbuffsBase[i] + regHandles[i].sendBuffOffset;
    regInfo->recvbuffs[i] = (char*)regInfo->recvbuffsBase[i] + regHandles[i].recvBuffOffset;
  }
  regInfo->nBuffs = comm->localRanks;
  TRACE(NCCL_COLL, "Rank %d exchanged %d buffers", comm->rank, regInfo->nBuffs);
  return ncclSuccess;
}

// Compute enqueue element, save it in list
// Compute CUDA launch parameters
// Capture time code in view of CUDA graph
static ncclResult_t ncclSetupCollKernel(struct ncclInfo* info) {
  ncclComm_t comm = info->comm;
  if (comm->nRanks == 1 &&
      // User-defined reduction ops may need alter the data even for unitary reductions
      info->op < ncclNumOps) {
    if (info->sendbuff != info->recvbuff)
      CUDACHECK(cudaMemcpyAsync(info->recvbuff, info->sendbuff, info->nBytes, cudaMemcpyDeviceToDevice, info->stream));
    return ncclSuccess;
  }

  // Compute cuda kernel arg and proxy arg templates
  struct ncclQueueElem* eqElem;
  NCCLCHECK(comm->enqueueInfo->elemList->getNewElem(&eqElem));
  struct ncclWorkElem* work = &eqElem->work;
  eqElem->proxyArgs.nsubs = 1;
  NCCLCHECK(computeColl(info, work, &eqElem->proxyArgs));

  // Determine grid size
  struct cudaLaunchParams* params = comm->myParams;
  params->gridDim.x += info->nChannels;
  params->gridDim.x = std::min<unsigned>(params->gridDim.x, comm->nChannels);
  params->blockDim.x = std::max<unsigned>(params->blockDim.x, info->nThreads);
  comm->enqueueInfo->maxChannels = params->gridDim.x;  // params may be varied by a second graph hence we need to capture it here

  // Inline the first kernel
  if (params->func == NULL) {
    params->func = ncclKerns[work->funcIndex];
    memcpy(&comm->args, work, sizeof(struct ncclWorkElem));
    comm->args.coll.bid = 0;  // Only inline for channel 0
    comm->args.active = 2;    // I am so far the last element; may be changed later in aggregation mode
  }

  // Register and exchange input and output buffers
  if (comm->usingCudaGraph &&                   // only in CUDA graph mode
      comm->graphRegister == 1 &&               // when registration is enabled
      info->algorithm == NCCL_ALGO_COLLNET &&   // limited to CollNet for now
      comm->intraHighestTransportType == TRANSPORT_P2P && // only when all ranks can p2p each other
      comm->intraRanks == 1) {                  // only in multi-process mode
    NCCLCHECK(ncclRegBuffAndExchange(info, &eqElem->buffRegInfo));
    // Disable inline argument because we need kernel to copy the entire ncclWork from workFifo
    // because the registered addresses are in ncclWork
    if (eqElem->buffRegInfo.nBuffs > 0) comm->args.active = 0;
    comm->enqueueInfo->nRegBuffs += eqElem->buffRegInfo.nBuffs;
  }

  return ncclSuccess;
}

static inline int findShortestChannel(ncclComm_t comm) {
  size_t minSize = SIZE_MAX;
  int minC = 0;
  for (int c=0; c<comm->nChannels; c++) {
    struct ncclChannel* channel = comm->channels+c;
    if (channel->totalSize < minSize) {
      minSize = channel->totalSize;
      minC = c;
    }
  }
  return minC;
}

static inline int getNextChannel(ncclComm_t comm, int aggMode) {
  int nextChannel = 0;
  if (aggMode && comm->asyncAllocMode == ncclComm::SHORTEST_QUEUE) {
    nextChannel = findShortestChannel(comm);
  } else {
    nextChannel = comm->lastChannel % comm->nChannels;
    comm->lastChannel++;
  }
  return nextChannel;
}

ncclResult_t ncclSetupAsyncKernels(ncclComm_t comm) {
  if (comm->asyncOpCount == 0) {
    return ncclSuccess;
  } else if (comm->asyncOpCount == 1) {
    // No aggregation
    struct ncclInfo* info = comm->asyncOps;
    info->nChannels = 0;
    NCCLCHECK(ncclSetupCollKernel(info));
  } else {
    // Aggregation
    size_t channelSize;
    if (comm->channelSize > 0) {
      channelSize = comm->channelSize;
    } else if (comm->collNetSupport && comm->asyncOps[0].coll == ncclFuncAllReduce) {
      channelSize = 256 * 1024;
    } else {
      channelSize = NCCL_AGG_CHANNEL_SIZE * std::min(16, comm->nRanks);  // scale channel size based on nranks as latency increases
    }
    // Reduce the per-channel size if we cannot fully utilize the channels
    while (comm->asyncTotalSize < channelSize * comm->nChannels && channelSize > NCCL_MIN_CHANNEL_SIZE) channelSize /= 2;
    int channelUsed = 0;
    int homogeneous = 1;
    int allCollNetSupport = comm->collNetSupport;
    for (int c = 0; c < comm->asyncOpCount; c++) {
      struct ncclInfo* info = comm->asyncOps+c;
      info->nChannels = std::min(std::max(1, (int)DIVUP(info->nBytes, channelSize)), comm->nChannels); // assign number of channels
      channelUsed += info->nChannels;
      // We can use fast path if all collectives are the same
      homogeneous &= info->coll == comm->asyncOps[0].coll &&
                     info->opFull.op == comm->asyncOps[0].opFull.op &&
                     info->datatype == comm->asyncOps[0].datatype;
      if (allCollNetSupport > 0) NCCLCHECK(getCollNetSupport(info, &allCollNetSupport));
    }
    // Compute algo, proto, nthreads for the entire kernel
    struct ncclInfo total;
    total.comm = comm;
    total.coll = comm->asyncOps[0].coll;
    total.nBytes = comm->asyncTotalSize;
    total.nChannels = std::min(channelUsed, comm->nChannels);
    int perChannelOps = DIVUP(channelUsed, total.nChannels);
    if (homogeneous) NCCLCHECK(getAlgoInfo(&total, allCollNetSupport, perChannelOps));
    for (int c = 0; c < comm->asyncOpCount; c++) {
      struct ncclInfo* info = comm->asyncOps+c;
      if (homogeneous) {
        info->algorithm = total.algorithm;
        info->protocol = total.protocol;
        info->nThreads = total.nThreads;
      }
      NCCLCHECK(ncclSetupCollKernel(info));
    }
    comm->args.active = 0;  // disable inline argument
  }
  // Reset counters
  comm->asyncOpCount = 0;
  comm->asyncTotalSize = 0;
  return ncclSuccess;
}

static ncclResult_t ncclSaveAsyncColl(struct ncclInfo* info) {
  ncclComm_t comm = info->comm;
  if (comm->asyncOpCount >= NCCL_MAX_OPS) {
    WARN("Too many async operations in progress, max is %d", NCCL_MAX_OPS);
    return ncclInvalidUsage;
  }
  memcpy(comm->asyncOps+comm->asyncOpCount, info, sizeof(struct ncclInfo));
  comm->asyncOpCount++;
  comm->asyncTotalSize += info->nBytes;
  return ncclSuccess;
}

// Save p2p operations in comm->p2pSends and p2pRecvs. Operations will be posted to channels
// during ncclGroupEnd()
static ncclResult_t ncclSaveP2p(struct ncclInfo* info) {
  struct ncclComm* comm = info->comm;
  int peer = info->root;
  ssize_t nBytes = info->count*ncclTypeSize(info->datatype);
  if (info->opName[0] == 'S') { // Send
    if (peer != comm->rank) {
      int delta = (comm->nRanks - (comm->rank-peer)) % comm->nRanks;
      for (int c=0; c<comm->p2pnChannelsPerPeer; c++) {
        int channelId = (delta+comm->p2pChannels[c]) % comm->p2pnChannels;
        if (comm->channels[channelId].peers[peer].send[0].connected == 0) { // P2P uses only 1 connector
          comm->connectSend[peer] |= (1<<channelId);
          comm->connect = 1;
        }
      }
    }
    NCCLCHECK(ncclSaveP2pInfo(comm->p2pSends[info->root], (void*)info->sendbuff, nBytes));
    comm->p2pSendCount++;
  } else {
    if (peer != comm->rank) {
      int delta = (comm->nRanks + (comm->rank-peer)) % comm->nRanks;
      for (int c=0; c<comm->p2pnChannelsPerPeer; c++) {
        int channelId = (delta+comm->p2pChannels[c]) % comm->p2pnChannels;
        if (comm->channels[channelId].peers[peer].recv[0].connected == 0) { // P2P uses only 1 connector
          comm->connectRecv[peer] |= (1<<channelId);
          comm->connect = 1;
        }
      }
    }
    NCCLCHECK(ncclSaveP2pInfo(comm->p2pRecvs[info->root], info->recvbuff, nBytes));
    comm->p2pRecvCount++;
  }
  return ncclSuccess;
}

enum { RingTree_Segment=0, P2P_Segment=1, CollNet_Segment=2 };
static int getSegment(int type, int delta, struct ncclWork* work) {
  // Current ncclWork is full
  if (work->elems[NCCL_MAX_WORK_ELEMENTS-1].active != 0) return -1;

  if (type == P2P_Segment) {  // P2P
    // Do not mix P2P and collective ops
    if (work->elems[0].funcIndex != FUNC_INDEX_P2P) return -1;
    for (int s=0; s<NCCL_MAX_WORK_ELEMENTS && work->elems[s].p2p.delta != delta; s++) {
      if (work->elems[s].active == 0) return s;
    }
  } else if (type == CollNet_Segment) { // CollNet
    for (int s=0; s<NCCL_MAX_WORK_ELEMENTS; s+=NCCL_REG_ELEM_FACTOR) {
      if (work->elems[s].active == 0) return s;
    }
  } else {  // Ring or Tree
    for (int s=0; s<NCCL_MAX_WORK_ELEMENTS; s++) {
      if (work->elems[s].active == 0) return s;
    }
  }
  return -1;
}

static ncclResult_t computeP2pWorkElem(struct ncclInfo* info /* input */, struct ncclWorkElem* elem /* output */) {
  elem->comm = info->comm->devComm;
  elem->funcIndex = FUNC_INDEX_P2P;
  elem->nThreads = NCCL_MAX_NTHREADS;
  elem->sendbuff = info->sendbuff;
  elem->recvbuff = info->recvbuff;
  elem->p2p.sendCount = info->sendbytes;
  elem->p2p.recvCount = info->recvbytes;
  elem->p2p.sendChunkSize = info->sendChunkSize;
  elem->p2p.recvChunkSize = info->recvChunkSize;
  elem->p2p.delta = info->delta;
  return ncclSuccess;
}

static ncclResult_t enqueueSegOp(int type, struct ncclWorkElem* elem /* input */, struct ncclWork* work, int s,
    struct ncclBuffRegInfo* regInfo, struct ncclChannel* channel, struct ncclComm* comm) {
  // Copy element into corresponding segment of ncclWork
  memcpy(work->elems+s, elem, sizeof(struct ncclWorkElem));
  work->elems[s].active = 1;

  // Determine nThreads at dynamic time
  if (type == P2P_Segment) {
    const int nsegments = s+1;
    int nThreads = 512;
    while (nsegments*nThreads > 512) nThreads /= 2;
    if (nThreads >= 128) nThreads += WARP_SIZE;
    for (int i=0; i<nsegments; i++) work->elems[i].p2p.nThreads = nThreads;
  }

  // Copy registered buffer addresses into ncclWork
  if (regInfo->nBuffs > 0) {
    struct ncclWorkRegElem* regElem = (struct ncclWorkRegElem*)(work->elems+s);
    // For CollNet
    for (int i=0; i<NCCL_MAX_DIRECT_ARITY; i++) {
      int peer = channel->collTree.down[i];
      if (peer == -1) break;
      int j = comm->rankToIntraNodeRank[peer];
      if (j < 0) {
        WARN("Invalid intra-node rank %d for peer %d", j, peer);
        return ncclInternalError;
      }
      regElem->dnInputs[i] = regInfo->sendbuffs[j];
      regElem->dnOutputs[i] = regInfo->recvbuffs[j];
    }
    for (int i=0; i<NCCL_MAX_DIRECT_ARITY; i++) {
      int peer = channel->collTree.up[i];
      if (peer == -1) break;
      int j = comm->rankToIntraNodeRank[peer];
      if (j < 0) {
        WARN("Invalid intra-node rank %d for peer %d", j, peer);
        return ncclInternalError;
      }
      regElem->upOutputs[i] = regInfo->recvbuffs[j];
    }
    work->elems[s].regUsed = 1;
  }
  return ncclSuccess;
}

ncclResult_t ncclEnqueueP2pKernel(struct ncclComm* comm, struct ncclQueueElem* eqElem) {
  struct ncclWorkElem* workElem = &eqElem->work;
  struct ncclProxyArgs* proxyArgs = &eqElem->proxyArgs;

  // Try to reuse last p2p operation if not full yet
  struct ncclChannel* channel = proxyArgs->subs[0].channel;
  int opIndex = (channel->workFifoTail-1+NCCL_MAX_OPS)%NCCL_MAX_OPS;
  struct ncclWork* w = channel->workFifo+opIndex;
  int segment = -1;
  if (channel->workCount) {
    // Try to pack more segments into a single operation
    segment = getSegment(P2P_Segment, workElem->p2p.delta, w);
  }
  if (segment == -1) {
    NCCLCHECK(getNextOp(channel, &w, NULL));
    segment = 0;
  }

  // store work element into FIFO
  NCCLCHECK(ncclProxySaveP2p(comm, proxyArgs));
  NCCLCHECK(enqueueSegOp(P2P_Segment, workElem, w, segment, &eqElem->buffRegInfo, channel, comm));
  return ncclSuccess;
}

ncclResult_t ncclSetupP2pKernel(struct ncclInfo* info) {
  ncclComm* comm = info->comm;
  // Compute cuda kernel arg and proxy arg templates
  struct ncclQueueElem* eqElem;
  NCCLCHECK(comm->enqueueInfo->elemList->getNewElem(&eqElem));
  // The proxy code will set and tune the send/recv chunk size, make sure to run it first.
  NCCLCHECK(ncclProxyComputeP2p(info, &eqElem->proxyArgs));
  NCCLCHECK(computeP2pWorkElem(info, &eqElem->work));

  int channelId = info->channelId;
  struct cudaLaunchParams* params = comm->myParams;
  params->gridDim.x = std::max<unsigned>(params->gridDim.x, channelId+1);
  params->blockDim.x = std::max<unsigned>(params->blockDim.x, eqElem->work.nThreads);
  comm->enqueueInfo->maxChannels = params->gridDim.x;  // params may be varied by a second graph hence we need to capture it here

  // Record the first kernel to launch
  // Just for CUDA kernel to know this is a P2P operation
  // The CUDA kernel does not use the inlined first work element as fastpath argument
  if (params->func == NULL) {
    params->func = ncclKerns[eqElem->work.funcIndex];
    comm->args.comm = eqElem->work.comm;
    comm->args.active = 0;
  }
  return ncclSuccess;
}

// Dynamic enqueue function for collective kernels
// Supports both aggregated and non-aggregated modes
ncclResult_t ncclEnqueueCollKernel(struct ncclComm* comm, struct ncclQueueElem* eqElem, int aggMode) {
  struct ncclWorkElem* work = &eqElem->work;
  struct ncclProxyArgs* proxyArgs = &eqElem->proxyArgs;

  int nChannels = work->coll.nChannels;
  size_t channelSize = work->coll.count*ncclTypeSize(proxyArgs->dtype)/work->coll.nChannels;
  int segmentType = proxyArgs->redOp == ncclNumOps ? RingTree_Segment : CollNet_Segment;  // redOp is only set when using CollNet

  for (int bid=0; bid<nChannels; bid++) {
    int channelId = getNextChannel(comm, aggMode);
    struct ncclChannel* channel = comm->channels+channelId;

    // Proxy
    proxyArgs->subs[0].channel = channel;
    proxyArgs->opCount = comm->collOpCount;
    proxyArgs->commOpCount = comm->opCount;
    if (proxyArgs->subs[0].nsteps) NCCLCHECK(ncclProxySaveColl(proxyArgs, comm->nRanks));

    work->coll.bid = bid % nChannels;
    struct ncclWork* w = NULL;
    int segment = -1;
    if (aggMode && channel->workCount) {
      // Try to pack more segments into a single operation
      int opIndex = (channel->workFifoTail-1+NCCL_MAX_OPS)%NCCL_MAX_OPS;
      w = channel->workFifo+opIndex;
      // All elems in work must have same (funcIndex,nThreads),
      // see "src/collectives/device/common.h"
      if (w->elems[0].funcIndex == work->funcIndex &&
          w->elems[0].nThreads == work->nThreads) {
        segment = getSegment(segmentType, 0, w);
      }
    }
    if (segment == -1) {
      NCCLCHECK(getNextOp(channel, &w, NULL));
      segment = 0;
    }

    // store work element into FIFO
    NCCLCHECK(enqueueSegOp(segmentType, work, w, segment, &eqElem->buffRegInfo, channel, comm));
    channel->totalSize += channelSize;
  }
  comm->collOpCount++;
  return ncclSuccess;
}

template<int USING_CUDA_GRAPH>
void CUDART_CB ncclEnqueueHostSetup(void* arg) {
  ncclResult_t ret;
  struct ncclQueueInfo* eqInfo = (struct ncclQueueInfo*)arg;
  ncclComm_t comm = eqInfo->comm;
  int aggMode = eqInfo->elemList->count() > 1 ? 1 : 0;

  // Iterate through the element list
  struct ncclQueueElem* eqElem = eqInfo->elemList->begin();
  while (eqElem != NULL) {
    if (eqElem->work.funcIndex == FUNC_INDEX_P2P) {
      NCCLCHECKGOTO(ncclEnqueueP2pKernel(comm, eqElem), ret, cb_end);
    } else {
      NCCLCHECKGOTO(ncclEnqueueCollKernel(comm, eqElem, aggMode), ret, cb_end);
    }
    eqElem = eqInfo->elemList->getNext();
  }

  NCCLCHECKGOTO(setupLaunch(eqInfo, USING_CUDA_GRAPH), ret, cb_end);
  NCCLCHECKGOTO(ncclLaunchProxy(eqInfo), ret, cb_end);

cb_end:
  if (ret != ncclSuccess) {
    WARN("Failure in host setup : %s", ncclGetErrorString(ret));
  }
  eqInfo->ret = ret;
}

template void CUDART_CB ncclEnqueueHostSetup<0>(void*);
template void CUDART_CB ncclEnqueueHostSetup<1>(void*);

void* graphHelperFunc(void *args) {
  struct ncclGraphHelperResources* res = (struct ncclGraphHelperResources*)args;
  if (res == NULL) {
    WARN("CUDA Graph helper resource is null");
    return NULL;
  }
  int dev = res->comm->cudaDev;
  CUDACHECKIGNORE(cudaSetDevice(dev));
  INFO(NCCL_COLL, "CUDA Graph helper thread created for device %d", dev);

  volatile enum helperThreadState* state = &res->threadState;
  volatile int* ipcTail = &res->ipcTail;
  while (1) {
    int ipcTailMark = *ipcTail;
    int ipcCount = 0;
    while (res->ipcHead != ipcTailMark) {
      if (res->ipcBases[res->ipcHead] != NULL)
        CUDACHECKIGNORE(cudaIpcCloseMemHandle(res->ipcBases[res->ipcHead]));
      res->ipcBases[res->ipcHead] = NULL;
      res->ipcHead = (res->ipcHead+1)%NCCL_IPC_POOL_SIZE;
      ipcCount++;
    }
    TRACE(NCCL_COLL, "CUDA Graph helper thread closed %d IPC handles", ipcCount);
    pthread_mutex_lock(&res->threadLock);
    while (res->ipcHead == *ipcTail && *state != ThreadStop) {
      pthread_cond_wait(&res->threadCond, &res->threadLock);
    }
    pthread_mutex_unlock(&res->threadLock);
    if (*state == ThreadStop) {
      INFO(NCCL_COLL, "CUDA Graph helper thread for device %d returning", dev);
      return NULL;
    }
  }
}

ncclResult_t ncclGetCudaGraph(ncclComm_t comm, cudaGraph_t* graph) {
  comm->usingCudaGraph = 0;
#if CUDART_VERSION >= 11030
  cudaStreamCaptureStatus captureStatus;
  unsigned long long cudaGraphId;
  if (comm->driverVersion < 11030) {
    CUDACHECK(cudaStreamIsCapturing(comm->userStream, &captureStatus));
    if (captureStatus != cudaStreamCaptureStatusNone) {
      WARN("The installed CUDA driver is older than the minimum version (R465) required for NCCL's CUDA Graphs support");
      return ncclInvalidUsage;
    }
    return ncclSuccess;
  }
  CUDACHECK(cudaStreamGetCaptureInfo_v2(comm->userStream, &captureStatus, &cudaGraphId, graph, NULL, NULL));
  if (captureStatus == cudaStreamCaptureStatusActive) {
    if (cudaGraphId != comm->lastCudaGraphId) {
      INFO(NCCL_COLL, "stream is being captured by a new graph, id %llu", cudaGraphId);
      // We are in a new graph, hence need to forget the last setup node so that
      // the first setup node in the new graph will not have a dependency
      comm->lastCudaGraphId = cudaGraphId;
      comm->lastSetupNode = NULL;
    }
    if (comm->launchMode == ncclComm::GROUP) comm->launchMode = ncclComm::GROUP_GRAPH;
    comm->usingCudaGraph = 1;

    // Create helper thread that closes IPC handles during graph destruction
    // Only create this thread when buffer registration is enabled
    if ((!comm->graphHelperThread) && comm->graphRegister == 1 && comm->disableGraphHelper == 0) {
      pthread_mutex_init(&comm->graphHelperResources->threadLock, NULL);
      pthread_cond_init(&comm->graphHelperResources->threadCond, NULL);
      comm->graphHelperResources->threadState = ThreadStart;
      pthread_create(&comm->graphHelperThread, NULL, graphHelperFunc, comm->graphHelperResources);
    }
  }
#endif
  return ncclSuccess;
}

ncclResult_t ncclCudaGraphHostSetup(ncclComm_t comm, cudaGraph_t graph) {
#if CUDART_VERSION >= 11030
  struct ncclQueueInfo* eqInfo = comm->enqueueInfo;
  // Create a CUDA object to wrap around the argument space
  // which CUDA graph would manage lifetime of
  cudaUserObject_t object;
  CUDACHECK(cudaUserObjectCreate(&object, eqInfo, ncclDestroyQueueInfo, 1/*initialRefcount*/, cudaUserObjectNoDestructorSync));
  CUDACHECK(cudaGraphRetainUserObject(graph, object, 1, cudaGraphUserObjectMove));

  cudaHostFn_t fn = ncclEnqueueHostSetup<1>;
  // Add a CPU node to the graph
  cudaGraphNode_t setupNode;
  cudaHostNodeParams setupNodeParams = {fn, eqInfo};
  int numDependencies = comm->lastSetupNode == NULL ? 0 : 1;
  CUDACHECK(cudaGraphAddHostNode(&setupNode, graph, &comm->lastSetupNode, numDependencies, &setupNodeParams));
  CUDACHECK(cudaStreamUpdateCaptureDependencies(comm->userStream, &setupNode, 1, cudaStreamAddCaptureDependencies));
  comm->lastSetupNode = setupNode;
  return ncclSuccess;
#else
  WARN("NCCL does not support this CUDA version for CUDA graph feature");
  return ncclInternalError;
#endif
}

static ncclResult_t hostToDevRedOp(
    ncclDevRedOpFull *opFull, ncclRedOp_t op, ncclDataType_t datatype, ncclComm *comm
  ) {
  union {
    int8_t i8;
    uint8_t u8;
    int32_t i32;
    uint32_t u32;
    int64_t i64;
    uint64_t u64;
    half f16;
    #if defined(__CUDA_BF16_TYPES_EXIST__)
      __nv_bfloat16 bf16;
    #endif
    float f32;
    double f64;
    void *ptr;
  };
  u64 = 0;
  opFull->scalarArgIsPtr = false;
  switch (int(op)) {
  case ncclSum:  opFull->op = ncclDevSum;  break;
  case ncclProd: opFull->op = ncclDevProd; break;
  case ncclMax:  opFull->op = ncclDevMax;  break;
  case ncclMin:  opFull->op = ncclDevMin;  break;
  case ncclAvg:
    switch ((int)datatype) {
    case ncclInt8:  case ncclInt32:  case ncclInt64:
    case ncclUint8: case ncclUint32: case ncclUint64:
      opFull->op = ncclDevSumPostDiv;
      u64 = comm->nRanks;
      break;
    case ncclFloat16:
      opFull->op = ncclDevPreMulSum;
      f16 = __float2half(float(1.0/comm->nRanks)); // __double2half not supported pre CUDA 11.x
      break;
    #if defined(__CUDA_BF16_TYPES_EXIST__)
    case ncclBfloat16:
      opFull->op = ncclDevPreMulSum;
      bf16 = __float2bfloat16(float(1.0/comm->nRanks));
      break;
    #endif
    case ncclFloat32:
      opFull->op = ncclDevPreMulSum;
      f32 = float(1.0/comm->nRanks);
      break;
    case ncclFloat64:
      opFull->op = ncclDevPreMulSum;
      f64 = 1.0/comm->nRanks;
      break;
    }
    opFull->scalarArgIsPtr = false;
    opFull->scalarArg = u64;
    break;
  default: // user created
    int ix = int(ncclUserRedOpMangle(comm, op)) - int(ncclNumOps);
    ncclUserRedOp *user = &comm->userRedOps[ix];
    if (datatype != user->datatype) {
      WARN("Data type supplied to user-created ncclRedOp_t does not match type "
           "given to reduction operation");
      return ncclInvalidArgument;
    }
    *opFull = user->opFull;
    break;
  }
  return ncclSuccess;
}

ncclResult_t ncclEnqueueCheck(struct ncclInfo* info) {
  ncclResult_t ret = ncclSuccess;
  bool isAsync = ncclAsyncMode();
  int savedDev = -1;
  // Check arguments
  NCCLCHECK(PtrCheck(info->comm, info->opName, "comm"));
  if (isAsync && info->comm->checkPointers) {
    CUDACHECKGOTO(cudaGetDevice(&savedDev), ret, end);
    CUDACHECKGOTO(cudaSetDevice(info->comm->cudaDev), ret, end);
  }
  NCCLCHECKGOTO(ArgsCheck(info), ret, end);

  // Copy reduction op state from op handle into info struct here since the
  // op handle may be destroyed before ncclGroupEnd().
  NCCLCHECKGOTO(hostToDevRedOp(&info->opFull, info->op, info->datatype, info->comm), ret, end);

  // Launch asynchronously if needed
  if (isAsync) {
    // Always register comm even in case of error to make sure ncclGroupEnd
    // cleans it up.
    NCCLCHECKGOTO(ncclAsyncColl(info->comm), ret, end);
    NCCLCHECKGOTO(checkSetStream(info), ret, end);

    INFO(NCCL_COLL,"%s: OpCount %lx sendbuff %p recvbuff %p count %zi datatype %d op %d root %d comm %p [nranks=%d] stream %p",
        info->opName, info->comm->opCount, info->sendbuff, info->recvbuff, info->count,
        info->datatype, info->op, info->root, info->comm, info->comm->nRanks, info->stream);

    if (info->coll == ncclFuncSendRecv) { //p2p stored separately
      NCCLCHECKGOTO(ncclSaveP2p(info), ret, end);
    } else {
      NCCLCHECKGOTO(ncclSaveAsyncColl(info), ret, end);
    }
  } else {
    NCCLCHECKGOTO(checkSetStream(info), ret, end);

    INFO(NCCL_COLL,"%s: OpCount %lx sendbuff %p recvbuff %p count %zi datatype %d op %d root %d comm %p [nranks=%d] stream %p",
        info->opName, info->comm->opCount, info->sendbuff, info->recvbuff, info->count,
        info->datatype, info->op, info->root, info->comm, info->comm->nRanks, info->stream);

    // Check whether we are in cuda graph mode
    cudaGraph_t graph;
    ncclComm_t comm = info->comm;
    NCCLCHECKGOTO(ncclGetCudaGraph(comm, &graph), ret, end);

    // Common part between graph mode and non-graph mode
    NCCLCHECKGOTO(ncclSetupCollKernel(info), ret, end);

    // Host setup
    if (comm->usingCudaGraph) {
      NCCLCHECKGOTO(ncclCudaGraphHostSetup(comm, graph), ret, end);
    } else {
      ncclEnqueueHostSetup<0>(comm->enqueueInfo);
      NCCLCHECKGOTO(comm->enqueueInfo->ret, ret, end);
    }

    // Common part between graph mode and non-graph mode
    NCCLCHECKGOTO(ncclLaunchBarrier(comm), ret, end);
    NCCLCHECKGOTO(ncclLaunchKernel(comm), ret, end);
    NCCLCHECKGOTO(ncclRecordEvents(comm), ret, end);
    NCCLCHECKGOTO(ncclLaunchReset(comm), ret, end);
  }
end:
  if (isAsync && savedDev != -1) CUDACHECK(cudaSetDevice(savedDev));
  if (isAsync) ncclAsyncErrCheck(ret);
  return ret;
}

NCCL_API(ncclResult_t, ncclRedOpCreatePreMulSum, ncclRedOp_t *op, void *scalar, ncclDataType_t datatype, ncclScalarResidence_t residence, ncclComm_t comm);
ncclResult_t ncclRedOpCreatePreMulSum(ncclRedOp_t *op, void *scalar, ncclDataType_t datatype, ncclScalarResidence_t residence, ncclComm_t comm) {
  if (comm->userRedOpFreeHead == comm->userRedOpCapacity) {
    // double capacity and resize
    int cap = 2*comm->userRedOpCapacity;
    if (cap < 4) cap = 4;
    ncclUserRedOp *ops = new ncclUserRedOp[cap];
    std::memcpy(ops, comm->userRedOps, comm->userRedOpCapacity*sizeof(ncclUserRedOp));
    for(int ix=comm->userRedOpCapacity; ix < cap; ix++)
      ops[ix].freeNext = ix + 1;
    delete[] comm->userRedOps;
    comm->userRedOps = ops;
    comm->userRedOpCapacity = cap;
  }
  // pop from free list
  int ix = comm->userRedOpFreeHead;
  ncclUserRedOp *user = &comm->userRedOps[ix];
  comm->userRedOpFreeHead = user->freeNext;

  user->freeNext = -1; // allocated
  user->datatype = datatype;
  user->opFull.op = ncclDevPreMulSum;
  if (residence == ncclScalarHostImmediate) {
    user->opFull.scalarArgIsPtr = false;
    std::memcpy(&user->opFull.scalarArg, scalar, ncclTypeSize(datatype));
  } else {
    user->opFull.scalarArgIsPtr = true;
    user->opFull.scalarArg = reinterpret_cast<uint64_t>(scalar);
  }
  *op = ncclRedOp_t(int(ncclNumOps) + ix);
  *op = ncclUserRedOpMangle(comm, *op);
  return ncclSuccess;
}

NCCL_API(ncclResult_t, ncclRedOpDestroy, ncclRedOp_t op, ncclComm_t comm);
ncclResult_t ncclRedOpDestroy(ncclRedOp_t op, ncclComm_t comm) {
  if (0 <= int(op) && int(op) < int(ncclNumOps)) {
    WARN("ncclRedOpDestroy : operator is a NCCL builtin.");
    return ncclInvalidArgument;
  }
  if (int(op) < 0 || int(ncclMaxRedOp) < int(op)) {
    WARN("ncclRedOpDestroy :  operator is garbage.");
    return ncclInvalidArgument;
  }
  int ix = int(ncclUserRedOpMangle(comm, op)) - int(ncclNumOps);
  if (comm->userRedOpCapacity <= ix || comm->userRedOps[ix].freeNext != -1) {
    WARN("ncclRedOpDestroy : operator unknown to this communicator.");
    return ncclInvalidArgument;
  }
  // push to free list
  comm->userRedOps[ix].freeNext = comm->userRedOpFreeHead;
  comm->userRedOpFreeHead = ix;
  return ncclSuccess;
}
