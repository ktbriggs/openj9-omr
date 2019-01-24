/*******************************************************************************
 * Copyright (c) 2018, 2018 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include "thrtypes.h"

#include "CollectorLanguageInterface.hpp"
#include "EnvironmentStandard.hpp"
#include "Evacuator.hpp"
#include "EvacuatorController.hpp"
#include "EvacuatorDelegate.hpp"
#include "ForwardedHeader.hpp"
#include "GCExtensionsBase.hpp"
#include "IndexableObjectScanner.hpp"
#include "Math.hpp"
#include "MemcheckWrapper.hpp"
#include "ObjectModel.hpp"
#include "ScavengerStats.hpp"
#include "SlotObject.hpp"
#include "SublistFragment.hpp"

extern "C" {
	uintptr_t allocateMemoryForSublistFragment(void *vmThreadRawPtr, J9VMGC_SublistFragment *fragmentPrimitive);
}

MM_Evacuator *
MM_Evacuator::newInstance(uintptr_t workerIndex, MM_EvacuatorController *controller, GC_ObjectModel *objectModel, uintptr_t maxStackDepth, uintptr_t maxInsideCopySize, uintptr_t maxInsideCopyDistance, MM_Forge *forge)
{
	MM_Evacuator *evacuator = (MM_Evacuator *)forge->allocate(sizeof(MM_Evacuator), OMR::GC::AllocationCategory::FIXED, OMR_GET_CALLSITE());
	if(NULL != evacuator) {
		new(evacuator) MM_Evacuator(workerIndex, controller, objectModel, maxStackDepth, maxInsideCopySize, maxInsideCopyDistance, forge);
		if (!evacuator->initialize()) {
			evacuator->kill();
			evacuator = NULL;
		}
	}
	return evacuator;
}

void
MM_Evacuator::kill()
{
	tearDown();
	_forge->free(this);
}

bool
MM_Evacuator::initialize()
{
	/* enable spinning for monitor-enter own evacuator worklist to put/take work */
	if (0 != omrthread_monitor_init_with_name(&_mutex, 0, "MM_Evacuator::_mutex")) {
		return false;
	}
	/* disable spinning for monitor-try-enter other evacuator worklist mutex to pull work */
	_mutex->flags &= ~J9THREAD_MONITOR_TRY_ENTER_SPIN;

	/* initialize the delegate */
	if (!_delegate.initialize(this, _forge, _controller)) {
		return false;
	}

	return true;
}

void
MM_Evacuator::tearDown()
{
	/* tear down delegate */
	_delegate.tearDown();

	/* tear down mutex */
	if (NULL != _mutex) {
		omrthread_monitor_destroy(_mutex);
		_mutex = NULL;
	}

	/* free forge memory bound to arrays instantiated in constructor */
	_forge->free(_stackBottom);
	_forge->free(_copyspace);
	_forge->free(_whiteList);

	/* free the freelist */
	_freeList.reset();
}

/**
 * Per gc cycle setup. This binds the evacuator instance to a gc worker thread for the duration of the cycle.
 *
 * @param env the environment for the gc worker thread that is bound to this evacuator instance
 */
void
MM_Evacuator::bindWorkerThread(MM_EnvironmentStandard *env, uintptr_t tenureMask, uint8_t *heapBounds[][2], uint64_t copiedBytesReportingDelta)
{
	Debug_MM_true(0 == *(_workList.volume()));

	omrthread_monitor_enter(_mutex);

	/* bind evacuator and delegate to executing gc thread */
	_env = env;
	_env->setEvacuator(this);
	_tenureMask = tenureMask;
	_copiedBytesReportingDelta = copiedBytesReportingDelta;

	for (uintptr_t regionIndex = (uintptr_t)survivor; regionIndex <= ((uintptr_t)evacuate); regionIndex += 1) {
		_heapBounds[regionIndex][0] = heapBounds[regionIndex][0]; /* lower bound for heap region address range */
		_heapBounds[regionIndex][1] = heapBounds[regionIndex][1]; /* upper bound for heap region address range */
	}

	_delegate.cycleStart();

	/* clear worker gc stats */
	_stats = &_env->_scavengerStats;
	_stats->clear(true);
	_stats->_gcCount = _env->getExtensions()->scavengerStats._gcCount;

	/* Reset the local remembered set fragment */
	_env->_scavengerRememberedSet.count = 0;
	_env->_scavengerRememberedSet.fragmentCurrent = NULL;
	_env->_scavengerRememberedSet.fragmentTop = NULL;
	_env->_scavengerRememberedSet.fragmentSize = (uintptr_t)OMR_SCV_REMSET_FRAGMENT_SIZE;
	_env->_scavengerRememberedSet.parentList = &_env->getExtensions()->rememberedSet;

	/* clear cycle stats */
	_splitArrayBytesToScan = 0;
	_copiedBytesDelta[survivor] = _copiedBytesDelta[tenure] = 0;
	_largeObjectCounter[survivor] = _largeObjectCounter[tenure] = 0;
	_scannedBytesDelta = 0;
	_completedScan = true;
	_abortedCycle = false;

	/* set up whitespaces for the cycle */
#if !defined(EVACUATOR_DEBUG)
	_whiteList[survivor].bind(NULL, _env, _workerIndex, _controller->getMemorySubspace(survivor), false);
	_whiteList[tenure].bind(NULL, _env, _workerIndex, _controller->getMemorySubspace(tenure), true);
#else
	_whiteList[survivor].bind(&_controller->_debugger, _env, _workerIndex, _controller->getMemorySubspace(survivor), false);
	_whiteList[tenure].bind(&_controller->_debugger, _env, _workerIndex, _controller->getMemorySubspace(tenure), true);
#endif /* defined(EVACUATOR_DEBUG) */

	/* load some empty work packets into the freelist -- each evacuator retains forge memory between gc cycles to back this up */
	_freeList.reload();

	/* this evacuator has no work at this point so will set a low work release threshold */
	_workReleaseThreshold = _controller->calculateOptimalWorkPacketSize(getVolumeOfWork());

	omrthread_monitor_exit(_mutex);
}

/**
 * Per gc cycle cleanup. This unbinds the evacuator instance from its gc worker thread.
 *
 * @param env the environment for the gc worker thread that is bound to this evacuator instance
 */
void
MM_Evacuator::unbindWorkerThread(MM_EnvironmentStandard *env)
{
	omrthread_monitor_enter(_mutex);

	/* flush to freelist any abandoned work from the worklist  */
	_workList.flush(&_freeList);

	/* reset the freelist to free any underflow chunks allocated from system memory */
	_freeList.reset();

	/* merge GC stats */
	_controller->mergeThreadGCStats(_env);

	/* unbind delegate from gc thread */
	_delegate.cycleEnd();

	/* unbind evacuator from gc thread */
	_env->setEvacuator(NULL);
	_env = NULL;

	omrthread_monitor_exit(_mutex);
}

omrobjectptr_t
MM_Evacuator::evacuateRootObject(MM_ForwardedHeader *forwardedHeader, bool breadthFirst)
{
	Debug_MM_true(NULL == _scanStackFrame);
	Debug_MM_true(0 == _stackBottom->getWorkSize());

	/* failure to evacuate will return original pointer to evacuate region */
	omrobjectptr_t forwardedAddress = forwardedHeader->getObject();

	if (!forwardedHeader->isForwardedPointer()) {

		if (!isAbortedCycle()) {

			/* slot object must be evacuated -- determine before and after object size and which evacuation region should receive this object */
			uintptr_t slotObjectSizeAfterCopy = 0, slotObjectSizeBeforeCopy = 0, hotFieldAlignmentDescriptor = 0;
			_objectModel->calculateObjectDetailsForCopy(_env, forwardedHeader, &slotObjectSizeBeforeCopy, &slotObjectSizeAfterCopy, &hotFieldAlignmentDescriptor);
			EvacuationRegion const evacuationRegion = isNurseryAge(_objectModel->getPreservedAge(forwardedHeader)) ? survivor : tenure;

			/* can this object be spun out on the stack? */
			_scanStackFrame = !breadthFirst && !isBreadthFirst() ? reserveRootCopyspace(evacuationRegion, slotObjectSizeAfterCopy) : NULL;
			if (NULL != _scanStackFrame) {
				Debug_MM_true(_scanStackFrame == _whiteStackFrame[evacuationRegion]);

				omrobjectptr_t const copyHead = (omrobjectptr_t)_scanStackFrame->getCopyHead();

				/* copy and forward object to bottom frame (we don't know the referring address so just say NULL) */
				forwardedAddress = copyForward(forwardedHeader, NULL, _scanStackFrame, slotObjectSizeBeforeCopy, slotObjectSizeAfterCopy);

				/* if object was evacuated to bottom frame consider it pushed */
				if (forwardedAddress == copyHead) {
#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
					_scanStackFrame->activated();
					debugStack("R push");
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

					/* try to pull remaining whitespace up to next stack frame to force push for referents scanned in root object */
					MM_EvacuatorScanspace * const nextFrame = nextStackFrame(evacuationRegion, _scanStackFrame + 1);
					if (NULL != nextFrame) {
						nextFrame->pullWhitespace(_scanStackFrame);
						_whiteStackFrame[evacuationRegion] = nextFrame;
					}

					/* scan evacuated root object, copy and scan reachable nursery objects until both outside copyspaces are empty */
					scan();

					Debug_MM_true(NULL == _scanStackFrame);
					Debug_MM_true(0 == _stackBottom->getWorkSize());

				} else {

					/* object not copied by this evacuator */
					_scanStackFrame = NULL;
				}

			} else {

				/* copy object to outside copyspace */
				forwardedAddress = copyOutside(evacuationRegion, forwardedHeader, NULL, slotObjectSizeBeforeCopy, slotObjectSizeAfterCopy);
			}
		}

	} else {

		/* return forwarded object address */
		forwardedAddress = forwardedHeader->getForwardedObject();
	}

	return forwardedAddress;
}

bool
MM_Evacuator::evacuateRememberedObject(omrobjectptr_t objectptr)
{
	Debug_MM_true((NULL != objectptr) && isInTenure(objectptr));
	Debug_MM_true(_objectModel->getRememberedBits(objectptr) < (uintptr_t)0xc0);

	bool rememberObject = false;

	GC_ObjectScannerState objectScannerState;
	GC_ObjectScanner *objectScanner = _delegate.getObjectScanner(objectptr, &objectScannerState, GC_ObjectScanner::scanRoots);
	if (NULL != objectScanner) {
		GC_SlotObject *slotPtr;

		/* scan remembered object for referents in evacuate space */
		while (NULL != (slotPtr = objectScanner->getNextSlot())) {
			if (evacuateRootObject(slotPtr)) {
				rememberObject = true;
			}
		}
	}

	/* the remembered state of the object may also depends on its indirectly related objects */
	if (_objectModel->hasIndirectObjectReferents((CLI_THREAD_TYPE*)_env->getLanguageVMThread(), objectptr)) {
		if (_delegate.scanIndirectObjects(objectptr)) {
			rememberObject = true;
		}
	}

	return rememberObject;
}

void
MM_Evacuator::evacuateThreadSlot(volatile omrobjectptr_t *objectPtrIndirect)
{
	if (!_env->getExtensions()->isConcurrentScavengerEnabled()) {
		omrobjectptr_t objectPtr = *objectPtrIndirect;
		if (NULL != objectPtr) {
			/* auto-remember stack- and thread-referenced objects */
			if (isInEvacuate(objectPtr)) {

				if (!evacuateRootObject(objectPtrIndirect)) {
					Debug_MM_true(isInTenure(*objectPtrIndirect));
					Trc_MM_ParallelScavenger_copyAndForwardThreadSlot_deferRememberObject(_env->getLanguageVMThread(), *objectPtrIndirect);
					/* the object was tenured while it was referenced from the stack. Undo the forward, and process it in the rescan pass. */
					_controller->setEvacuatorFlag(MM_EvacuatorController::rescanThreadSlots, true);
					*objectPtrIndirect = objectPtr;
				}

			} else if (isInTenure(objectPtr)) {

				Debug_MM_true(_objectModel->getRememberedBits(objectPtr) < (uintptr_t)0xc0);
				if (_objectModel->atomicSwitchReferencedState(objectPtr, OMR_TENURED_STACK_OBJECT_RECENTLY_REFERENCED, OMR_TENURED_STACK_OBJECT_CURRENTLY_REFERENCED)) {
					Trc_MM_ParallelScavenger_copyAndForwardThreadSlot_renewingRememberedObject(_env->getLanguageVMThread(), objectPtr, OMR_TENURED_STACK_OBJECT_RECENTLY_REFERENCED);
				}
				Debug_MM_true(_objectModel->getRememberedBits(objectPtr) < (uintptr_t)0xc0);

			} else {

				Debug_MM_true(isInSurvivor(objectPtr));
			}
		}
	}
}

void
MM_Evacuator::rescanThreadSlot(omrobjectptr_t *objectPtrIndirect)
{
	omrobjectptr_t objectPtr = *objectPtrIndirect;
	if (isInEvacuate(objectPtr)) {
		/* the slot is still pointing at evacuate memory. This means that it must have been left unforwarded
		 * in the first pass so that we would process it here.
		 */
		MM_ForwardedHeader forwardedHeader(objectPtr);
		omrobjectptr_t tenuredObjectPtr = forwardedHeader.getForwardedObject();
		*objectPtrIndirect = tenuredObjectPtr;

		Debug_MM_true(NULL != tenuredObjectPtr);
		Debug_MM_true(isInTenure(tenuredObjectPtr));

		rememberObject(tenuredObjectPtr);
		_objectModel->setRememberedBits(tenuredObjectPtr, OMR_TENURED_STACK_OBJECT_CURRENTLY_REFERENCED);
	}
}

bool
MM_Evacuator::evacuateHeap()
{
	/* TODO: deprecate this calling pattern, forced by MM_RootScanner; root scanners should not call scanHeap() directly  */
	scanHeap();

	return !isAbortedCycle();
}

bool
MM_Evacuator::setAbortedCycle()
{
	/* assume controller doesn't know that the cycle is aborting */
	bool isIdempotent = true;
	if (!_abortedCycle) {
		isIdempotent = _controller->setAborting();
		_abortedCycle = true;
	}
	return isIdempotent;
}

bool
MM_Evacuator::isAbortedCycle()
{
	if (!_abortedCycle) {
		_abortedCycle = _controller->isAborting();
	}
	return _abortedCycle;
}

bool
MM_Evacuator::isBreadthFirst()
{
	return _controller->isEvacuatorFlagSet(MM_EvacuatorController::breadthFirstScan);
}

void
MM_Evacuator::flushRememberedSet()
{
	if (0 != _env->_scavengerRememberedSet.count) {
		_env->flushRememberedSet();
	}
}

void
MM_Evacuator::flushForWaitState()
{
	flushRememberedSet();
	_delegate.flushForWaitState();
}

uintptr_t
MM_Evacuator::flushWhitespace(MM_Evacuator::EvacuationRegion region)
{
	/* large whitespace fragements may be recycled back into the memory pool */
	uintptr_t flushed = _whiteList[region].flush();

	/* whitelist should be empty, top(0) should be NULL */
	Debug_MM_true(NULL == _whiteList[region].top(0));

	return flushed;
}

void
MM_Evacuator::workThreadGarbageCollect(MM_EnvironmentStandard *env)
{
	Debug_MM_true(_env == env);
	Debug_MM_true(_env->getEvacuator() == this);
	Debug_MM_true(_controller->isBoundEvacuator(_workerIndex));
	Debug_MM_true(NULL == _whiteStackFrame[survivor]);
	Debug_MM_true(NULL == _whiteStackFrame[tenure]);

	/* reset scan stack and set pointers to stack bounds  */
	for (_scanStackFrame = _stackBottom; _scanStackFrame < _stackCeiling; _scanStackFrame += 1) {
		_scanStackFrame->clear();
	}
	_stackLimit = isBreadthFirst() ? _stackBottom : _stackCeiling;
	_scanStackFrame = NULL;

	/* scan roots and remembered set and objects depending from these */
	scanRemembered();
	scanRoots();
	scanHeap();

	/* trick to obviate a write barrier -- objects tenured this cycle are always remembered */
	if (!isAbortedCycle() && _controller->isEvacuatorFlagSet(MM_EvacuatorController::rescanThreadSlots)) {
		_delegate.rescanThreadSlots();
		flushRememberedSet();
	}

	/* scan clearable objects -- this may involve 0 or more phases, with evacuator threads joining in scanHeap() at end of each phase */
	while (!isAbortedCycle() && _delegate.hasClearable()) {

		/* java root clearer has repeated phases involving *deprecated* evacuateHeap() and its delegated scanClearable() leaves no unscanned work */
		if (scanClearable()) {
			/* other language runtimes should use evacuateObject() only in delegated scanClearble() and allow scanHeap() to complete each delegated phase */
			 scanHeap();
		}
	}
	_delegate.flushForEndCycle();

	/* clear whitespace from outside copytspaces and whitespace frames */
	for (intptr_t space = (intptr_t)survivor; space <= (intptr_t)tenure; space += 1) {

		/* clear whitespace from the last active whitespace frame */
		if (NULL != _whiteStackFrame[space]) {
			_whiteList[space].add(_whiteStackFrame[space]->trim());
			_whiteStackFrame[space] = NULL;
		}

		/* release unused whitespace from outside copyspaces */
		Debug_MM_true(isAbortedCycle() || (0 == _copyspace[space].getCopySize()));
		_whiteList[space].add(_copyspace[space].trim());
		_copyspace[space].reset();
	}

#if defined(EVACUATOR_DEBUG)
	_whiteList[survivor].verify();
	_whiteList[tenure].verify();
#endif /* defined(EVACUATOR_DEBUG) */

	/* reset large copyspace (it is void of work and whitespace at this point) */
	Debug_MM_true(0 == _largeCopyspace.getCopySize());
	Debug_MM_true(0 == _largeCopyspace.getWhiteSize());
	_largeCopyspace.reset();

	/* large survivor whitespace fragments on the whitelist may be recycled back into the memory pool */
	flushWhitespace(survivor);

	/* tenure whitespace is retained between completed nursery collections but must be flushed for backout */
	Debug_MM_true(0 == _env->_scavengerRememberedSet.count);
	if (!isAbortedCycle()) {
		/* prune remembered set to complete cycle */
		_controller->pruneRememberedSet(_env);
		Debug_MM_true(0 == getVolumeOfWork());
	} else {
		/* flush tenure whitelist before backout */
		flushWhitespace(tenure);
		/* set backout flag to abort cycle */
		_controller->setBackOutFlag(_env, backOutFlagRaised);
		_controller->completeBackOut(_env);
	}
}

void
MM_Evacuator::scanRoots()
{
#if defined(EVACUATOR_DEBUG)
	if (_controller->_debugger.isDebugCycle()) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
		omrtty_printf("%5lu %2llu %2llu:     roots; ", _controller->getEpoch()->gc, (uint64_t)_controller->getEpoch()->epoch, (uint64_t)_workerIndex);
		_controller->printEvacuatorBitmap(_env, "stalled", _controller->sampleStalledMap());
		_controller->printEvacuatorBitmap(_env, "; resuming", _controller->sampleResumingMap());
		omrtty_printf("; flags:%llx; vow:%llx\n", (uint64_t)_controller->sampleEvacuatorFlags(), getVolumeOfWork());
	}
#endif /* defined(EVACUATOR_DEBUG) */

	_delegate.scanRoots();
}

void
MM_Evacuator::scanRemembered()
{
#if defined(EVACUATOR_DEBUG)
	if (_controller->_debugger.isDebugCycle()) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
		omrtty_printf("%5lu %2llu %2llu:remembered; ", _controller->getEpoch()->gc, (uint64_t)_controller->getEpoch()->epoch, (uint64_t)_workerIndex);
		_controller->printEvacuatorBitmap(_env, "stalled", _controller->sampleStalledMap());
		_controller->printEvacuatorBitmap(_env, "; resuming", _controller->sampleResumingMap());
		omrtty_printf("; flags:%llx; vow:%llx\n", (uint64_t)_controller->sampleEvacuatorFlags(), getVolumeOfWork());
	}
#endif /* defined(EVACUATOR_DEBUG) */

	/* flush locally buffered remembered objects to aggregated list */
	_env->flushRememberedSet();

	/* scan aggregated remembered objects concurrently with other evacuators */
	_controller->scavengeRememberedSet(_env);
}

bool
MM_Evacuator::scanClearable()
{
#if defined(EVACUATOR_DEBUG)
	if (_controller->_debugger.isDebugCycle()) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
		omrtty_printf("%5lu %2llu %2llu: clearable; ", _controller->getEpoch()->gc, (uint64_t)_controller->getEpoch()->epoch, (uint64_t)_workerIndex);
		_controller->printEvacuatorBitmap(_env, "stalled", _controller->sampleStalledMap());
		_controller->printEvacuatorBitmap(_env, "; resuming", _controller->sampleResumingMap());
		omrtty_printf("; flags:%llx; vow:%llx\n", (uint64_t)_controller->sampleEvacuatorFlags(), getVolumeOfWork());
	}
#endif /* defined(EVACUATOR_DEBUG) */
	/* if there are more root or other unreachable objects to be evacuated they can be copied and forwarded here */
	_delegate.scanClearable();

	/* run scanHeap() if scanClearable() produced more work to be scanned */
	return !_controller->hasCompletedScan();
}

void
MM_Evacuator::scanHeap()
{
	Debug_MM_true(NULL == _scanStackFrame);
#if defined(EVACUATOR_DEBUG)
	if (_controller->_debugger.isDebugCycle()) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
		omrtty_printf("%5lu %2llu %2llu:      heap; ", _controller->getEpoch()->gc, (uint64_t)_controller->getEpoch()->epoch, (uint64_t)_workerIndex);
		_controller->printEvacuatorBitmap(_env, "stalled", _controller->sampleStalledMap());
		_controller->printEvacuatorBitmap(_env, "; resuming", _controller->sampleResumingMap());
		omrtty_printf("; flags:%llx; vow:%llx\n", (uint64_t)_controller->sampleEvacuatorFlags(), getVolumeOfWork());
	}
#endif /* defined(EVACUATOR_DEBUG) */

	/* mark start of scan cycle -- this will be set after this evacuator receives controller's end of scan or aborts */
	_completedScan = false;

	/* try to find some work from this or other evacuator's worklist to pull into the scan stack */
	MM_EvacuatorWorkPacket *work = getWork();
	while (NULL != work) {

		/* pull work onto bottom stack frame */
		pull(work);

		/* scan work packet, pull and scan work from outside copyspaces until both are empty */
		scan();

		/* find more work to pull and scan, until scanning is complete or cycle aborted */
		work = getWork();
	}

	Debug_MM_true(_completedScan);
}

void
MM_Evacuator::scanComplete()
{
#if defined(EVACUATOR_DEBUG)
	Debug_MM_true(NULL == _scanStackFrame);
	for (MM_EvacuatorScanspace *stackFrame = _stackBottom; stackFrame < _stackCeiling; stackFrame += 1) {
		Debug_MM_true(0 == stackFrame->getWorkSize());
		Debug_MM_true(0 == stackFrame->getWhiteSize() || (stackFrame == _whiteStackFrame[getEvacuationRegion(stackFrame->getBase())]));
	}
	Debug_MM_true(isAbortedCycle() || (0 == _copyspace[survivor].getCopySize()));
	Debug_MM_true(isAbortedCycle() || (0 == _copyspace[tenure].getCopySize()));
#endif /* defined(EVACUATOR_DEBUG) */

	/* reset stack  */
	_stackLimit = isBreadthFirst() ? _stackBottom : _stackCeiling;

	/* all done heap scan */
	Debug_MM_true(!_completedScan);
	_completedScan = true;
}

void
MM_Evacuator::addWork(MM_EvacuatorWorkPacket *work)
{
	omrthread_monitor_enter(_mutex);
	work = _workList.add(work);
	if (NULL != work) {
		_freeList.add(work);
	}
	omrthread_monitor_exit(_mutex);

	_controller->notifyOfWork(this);

	_workReleaseThreshold = _controller->calculateOptimalWorkPacketSize(getVolumeOfWork());
}

MM_EvacuatorWorkPacket *
MM_Evacuator::findWork()
{
	MM_EvacuatorWorkPacket *work = NULL;

	if (!isAbortedCycle()) {
		/* select prospective donor as the evacuator with greatest volume of work */
		MM_Evacuator *donor = this;
		for (MM_Evacuator *evacuator = _controller->getNextEvacuator(donor); this != evacuator; evacuator = _controller->getNextEvacuator(evacuator)) {
			if (_controller->isBoundEvacuator(evacuator->getWorkerIndex()) && (evacuator->getVolumeOfWork() > donor->getVolumeOfWork())) {
				donor = evacuator;
			}
		}

		if (0 < donor->getVolumeOfWork()) {

			/* try selected donor, if donor has no work poll for the next evacuator that can donate work or bail if poll wraps to max */
			MM_Evacuator * const max = donor;
			do {
				Debug_MM_true(this != donor);

				/* skip self and do not wait for donor if it is involved with its worklist */
				if (0 == omrthread_monitor_try_enter(donor->_mutex)) {

					if (0 < donor->getVolumeOfWork()) {

						/* take first available packet as current work */
						work = donor->_workList.next();
						uint64_t volume = _workList.volume(work);

						/* level this evacuator's volume of work up with donor */
						const MM_EvacuatorWorkPacket *next = donor->_workList.peek();
						while (NULL != next) {

							/* receiver must leave at least as much work in donor worklist as received */
							uint64_t nextVolume = _workList.volume(next);
							if ((volume + nextVolume) < (donor->getVolumeOfWork() - nextVolume)) {

								/* this evacuator holds the controller mutex so does not take its own worklist mutex here */
								MM_EvacuatorWorkPacket *free = _workList.add(donor->_workList.next());
								if (NULL != free) {
									_freeList.add(free);
								}
								volume += nextVolume;
								next = donor->_workList.peek();

							} else {

								/* done leveling up */
								break;
							}
						}

						/* receiver has one work packet (work) and less volume on worklist than donor unless both worklists are empty */
						Debug_MM_true(getVolumeOfWork() <= donor->getVolumeOfWork());
					}

					omrthread_monitor_exit(donor->_mutex);

					/* stop and return if any work received from donor */
					if (NULL != work) {

						/* tell the world if donor has any distributable work left */
						_controller->notifyOfWork(donor);

#if defined(EVACUATOR_DEBUG)
						if (_controller->_debugger.isDebugWork()) {
							OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
							omrtty_printf("%5lu %2llu %2llu: pull work; base:%llx; length:%llx; vow:%llx; donor:%llx; donor-vow:%llx; ", _controller->getEpoch()->gc, (uint64_t)_controller->getEpoch()->epoch,
									(uint64_t)_workerIndex, (uint64_t)work->base, (uint64_t)work->length, getVolumeOfWork(), (uint64_t)donor->_workerIndex, donor->getVolumeOfWork());
							_controller->printEvacuatorBitmap(_env, "stalled", _controller->sampleStalledMap());
							_controller->printEvacuatorBitmap(_env, "; resuming", _controller->sampleResumingMap());
							omrtty_printf("\n");
						}
#endif /* defined(EVACUATOR_DEBUG) */

						return work;
					}
				}

				/* no work received so continue donor enumeration, skipping evacuators with no work */
				do {

					/* bail with no work if donor wraps around to max */
					donor = _controller->getNextEvacuator(donor);

				} while ((max != donor) && (0 == donor->getVolumeOfWork()));

			} while (max != donor);
		}
	}

	return NULL;
}

MM_EvacuatorWorkPacket *
MM_Evacuator::getWork()
{
	Debug_MM_true(NULL == _scanStackFrame);

	MM_EvacuatorWorkPacket *work = NULL;
	if (!isAbortedCycle()) {

		/* try to take work from worklist */
		omrthread_monitor_enter(_mutex);
		work = _workList.next();
		omrthread_monitor_exit(_mutex);

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
		if (NULL != work) {
			uint64_t headerSize = (work->offset == 1) ? _objectModel->getHeaderSize(work->base) : 0;
			_stats->countWorkPacketSize(headerSize + _workList.volume(work), _controller->_maximumWorkspaceSize);
		}
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

		/* outside copyspaces must be cleared before engaging the controller to find other work */
		if (NULL == work) {

			/* prioritize tenure work */
			if (0 < _copyspace[tenure].getCopySize()) {
				work = _freeList.next();
				work->base = (omrobjectptr_t)_copyspace[tenure].rebase(&work->length);
			} else if (0 < _copyspace[survivor].getCopySize()) {
				work = _freeList.next();
				work->base = (omrobjectptr_t)_copyspace[survivor].rebase(&work->length);
			}

			Debug_MM_true(0 == _largeCopyspace.getCopySize());
		}
	}

	if (NULL == work) {

		/* worklist is empty, or aborting; try to pull work from other evacuators */
		if ((0 < _scannedBytesDelta) || (0 < _copiedBytesDelta[survivor]) || (0 < _copiedBytesDelta[tenure])) {
			_controller->reportProgress(this, _copiedBytesDelta, &_scannedBytesDelta);
		}
		flushForWaitState();

		_controller->acquireController();

		work = findWork();
		if (NULL == work) {
#if defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
			uint64_t waitStartTime = startWaitTimer();
#endif /* defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

			/* controller will release evacuator from stall when work is received or all other evacuators have stalled to complete or abort scan */
			while (_controller->isWaitingToCompleteStall(this, work)) {
				/* wait for work or until controller signals all evacuators to complete or abort scan */
				_controller->waitForWork();
				work = findWork();
			}

			/* continue scanning received work or complete or abort scan */
			_controller->continueAfterStall(this, work);

#if defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
			endWaitTimer(waitStartTime, work);
#endif /* defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
		}

		_controller->releaseController();

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
		if (NULL != work) {
			uint64_t headerSize = (work->offset == 1) ? _objectModel->getHeaderSize(work->base) : 0;
			_stats->countWorkPacketSize(headerSize + _workList.volume(work), _controller->_maximumWorkspaceSize);
		}
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

	} else {

		/* in case a stalled evacuator tried for this worklist while held by this evacuator above */
		_controller->notifyOfWork(this);
	}

	/* reset work packet release threshold to produce small packets until work volume increases */
	_workReleaseThreshold = _controller->calculateOptimalWorkPacketSize(getVolumeOfWork());

	/* if no work at this point heap scan completes (or aborts) */
	return work;
}

void
MM_Evacuator::pull(MM_EvacuatorWorkPacket *work)
{
	Debug_MM_true(NULL != work);
	Debug_MM_true(0 < work->length);
	Debug_MM_true(NULL != work->base);
	Debug_MM_true(NULL == _scanStackFrame);
	Debug_MM_true(0 == _stackBottom->getWorkSize());

	/* unconditionally clear whitespace from bottom frame */
	if (0 < _stackBottom->getWhiteSize()) {
		Debug_MM_true(_stackBottom == _whiteStackFrame[getEvacuationRegion(_stackBottom->getBase())]);

		/* find an empty frame to receive cleared whitespace */
		EvacuationRegion bottomRegion = getEvacuationRegion(_stackBottom->getBase());
		MM_EvacuatorScanspace *whiteFrame = nextStackFrame(bottomRegion, _stackBottom + 1);

		Debug_MM_true(0 == whiteFrame->getWorkSize());
		Debug_MM_true(0 == whiteFrame->getWhiteSize());

		/* pull whitespace into empty frame from bottom frame */
		whiteFrame->pullWhitespace(_stackBottom);
		_whiteStackFrame[bottomRegion] = whiteFrame;
	}

	/* set up the stack for scanning in work packet region */
	_scanStackFrame = _stackBottom;

	/* load work packet into bottom stack frame */
	if (isSplitArrayPacket(work)) {

		GC_IndexableObjectScanner *scanner = (GC_IndexableObjectScanner *)_scanStackFrame->getObjectScannerState();
		/* the object scanner must be set in the scanspace at this point for split arrays -- work offset/length are array indices in this case */
		_delegate.getSplitPointerArrayObjectScanner(work->base, scanner, work->offset - 1, work->length, GC_ObjectScanner::scanHeap);
		/* the work packet contains a split array segment -- set scanspace base = array, scan = copy = limit = end = base + object-size */
		_scanStackFrame->setSplitArrayScanspace((uint8_t *)work->base, ((uint8_t *)work->base + _objectModel->getConsumedSizeInBytesWithHeader(work->base)), scanner);
		/* split array segments are scanned only on the bottom frame of the stack -- save the length of segment for updating _scannedBytes when this segment has been scanned */
		_splitArrayBytesToScan = sizeof(fomrobject_t) * work->length;

	} else {

		/* the work packet contains a contiguous series of objects -- set scanspace base = scan, copy = limit = end = scan + length */
		_scanStackFrame->setScanspace((uint8_t *)work->base, (uint8_t *)work->base + work->length, work->length);
		/* object scanners will be instantiated and _scannedBytes updated as objects in scanspace are scanned */
		_splitArrayBytesToScan = 0;
	}

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	_scanStackFrame->activated();
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

#if defined(EVACUATOR_DEBUG)
	if (_controller->_debugger.isDebugWork()) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
		omrtty_printf("%5lu %2llu %2llu: push work; base:%llx; length:%llx; vow:%llx; sow:%llx; tow:%llx ", _controller->getEpoch()->gc, (uint64_t)_controller->getEpoch()->epoch, (uint64_t)_workerIndex,
				(uint64_t)work->base, (uint64_t)work->length, getVolumeOfWork(), (uint64_t)_copyspace[survivor].getCopySize(), (uint64_t)_copyspace[tenure].getCopySize());
		_controller->printEvacuatorBitmap(_env, "stalled", _controller->sampleStalledMap());
		_controller->printEvacuatorBitmap(_env, "; resuming", _controller->sampleResumingMap());
		omrtty_printf("\n");
	}
	debugStack("W pull");
#endif /* defined(EVACUATOR_DEBUG) */

	_freeList.add(work);
}

void
MM_Evacuator::scan()
{
	/* scan stack until empty */
	while (NULL != _scanStackFrame) {
		Debug_MM_true(_stackBottom <= _scanStackFrame);
		Debug_MM_true((_stackLimit > _scanStackFrame) || (_stackLimit == _stackBottom));
		Debug_MM_true(_stackLimit <= _stackCeiling);

		/* copy inside current frame until push or pop */
		copy();
	}

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	for (MM_EvacuatorScanspace *frame = _stackBottom; frame < _stackCeiling; ++frame) {
		Debug_MM_true(0 == frame->getWorkSize());
	}
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
}

void
MM_Evacuator::push(MM_EvacuatorScanspace * const nextStackFrame)
{
	if (_scanStackFrame != nextStackFrame) {

		/* push to next frame */
		_scanStackFrame = nextStackFrame;

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
		Debug_MM_true(_scanStackFrame < _stackLimit);
		_scanStackFrame->activated();
		debugStack("push");
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
	}
}

void
MM_Evacuator::pop()
{
	debugStack("pop");
	Debug_MM_true((_stackBottom <= _scanStackFrame) && (_scanStackFrame < _stackCeiling));
	Debug_MM_true((0 == _scanStackFrame->getWhiteSize()) || (_scanStackFrame == _whiteStackFrame[getEvacuationRegion(_scanStackFrame->getBase())]));
	Debug_MM_true((0 == _scanStackFrame->getWorkSize()) || isAbortedCycle());

	/* clear scanned work and work-related flags from scanspace */
	_scanStackFrame->rebase();

	/* pop stack frame leaving trailing whitespace where it is (in _whiteStackFrame[getEvacuationRegion(_scanStackFrame->getBase())]) */
	if (_stackBottom < _scanStackFrame) {
		/* pop to previous frame, will continue with whitespace in popped frame if next pushed object does not cross region boundary */
		_scanStackFrame -= 1;
	} else {
		/* pop to empty stack */
		_scanStackFrame = NULL;
	}

}

void
MM_Evacuator::copy()
{
	const uintptr_t sizeLimit = _maxInsideCopySize;
	MM_EvacuatorScanspace * const stackFrame = _scanStackFrame;
	uint8_t * const frameLimit = stackFrame->getBase() + _maxInsideCopyDistance;
	const bool isTenureFrame = (tenure == getEvacuationRegion(stackFrame->getBase()));
	Debug_MM_true(evacuate > getEvacuationRegion(stackFrame->getBase()));

	/* get the active object scanner without advancing slot */
	GC_ObjectScanner *objectScanner = nextObjectScanner(stackFrame, false);

	/* copy small stack region objects inside frame, large and other region objects outside, until push() or frame completed */
	while (NULL != objectScanner) {
		Debug_MM_true((objectScanner->getParentObject() == (omrobjectptr_t)stackFrame->getScanHead()) || (stackFrame->isSplitArraySegment() && (objectScanner->getParentObject() == (omrobjectptr_t)stackFrame->getBase())));

		/* loop through reference slots in current object at scan head */
		GC_SlotObject *slotObject = objectScanner->getNextSlot();
		while (NULL != slotObject) {

			const omrobjectptr_t object = slotObject->readReferenceFromSlot();
			omrobjectptr_t forwardedAddress = object;
			if (isInEvacuate(object)) {

				/* copy and forward the slot object */
				MM_ForwardedHeader forwardedHeader(object);
				if (!forwardedHeader.isForwardedPointer()) {

					/* slot object must be evacuated -- determine receiving region and before and after object size */
					uintptr_t slotObjectSizeBeforeCopy = 0, slotObjectSizeAfterCopy = 0, hotFieldAlignmentDescriptor = 0;
					const EvacuationRegion evacuationRegion = isNurseryAge(_objectModel->getPreservedAge(&forwardedHeader)) ? survivor : tenure;
					const uintptr_t whiteSize = (NULL != _whiteStackFrame[evacuationRegion]) ? _whiteStackFrame[evacuationRegion]->getWhiteSize() : 0;
					_objectModel->calculateObjectDetailsForCopy(_env, &forwardedHeader, &slotObjectSizeBeforeCopy, &slotObjectSizeAfterCopy, &hotFieldAlignmentDescriptor);

					/* copy stack overflow and large objects outside,  copy small objects inside the stack if sufficient whitespace remaining ... */
					if ((stackFrame < _stackLimit) && (sizeLimit >= slotObjectSizeAfterCopy) && ((whiteSize >= slotObjectSizeAfterCopy) ||
						/* or current whitespace remainder is discardable ... */
						((MM_EvacuatorBase::max_scanspace_remainder >= whiteSize) &&
							/* and whitespace can be refreshed from evacuation region */
							(NULL != reserveInsideCopyspace(evacuationRegion, whiteSize, slotObjectSizeAfterCopy))))
					) {

						/* copy inside frame holding evacuation region whitespace */
						MM_EvacuatorScanspace * const whiteFrame = _whiteStackFrame[evacuationRegion];
						uint8_t * const copyHead = whiteFrame->getCopyHead();
						forwardedAddress = copyForward(slotObject, whiteFrame, slotObjectSizeBeforeCopy, slotObjectSizeAfterCopy);

						/* if object was copied into different frame a push may be necessary */
						if (((uint8_t *)forwardedAddress == copyHead) && ((frameLimit < copyHead) || (whiteFrame != stackFrame))) {

							if (whiteFrame <= stackFrame) {

								/* if copied to frame below or to current frame past limit try to find frame above to pull it into */
								MM_EvacuatorScanspace * const nextFrame = nextStackFrame(evacuationRegion, stackFrame + 1);

								/* if no next frame just continue scanning in current frame, copied object will be scanned when stack pops to white frame  */
								if (NULL != nextFrame) {
									/* pull copied object and remaining whitespace from white frame into next frame */
									nextFrame->pullTail(whiteFrame, copyHead);
									/* next frame now holds copied object and whitespace for evacuator region */
									_whiteStackFrame[evacuationRegion] = nextFrame;
									/* push to scan copied object */
									push(nextFrame);
								}

							} else {

								/* push to scan copied object */
								push(whiteFrame);
							}
						}

					} else {

						/* copy to outside copyspace if not possible to copy inside stack */
						forwardedAddress = copyOutside(evacuationRegion, &forwardedHeader, slotObject->readAddressFromSlot(), slotObjectSizeBeforeCopy, slotObjectSizeAfterCopy);
					}

				} else {

					/* just get the forwarding address */
					forwardedAddress = forwardedHeader.getForwardedObject();
				}

				/* update the referring slot with the forwarding address */
				slotObject->writeReferenceToSlot(forwardedAddress);
			}

			/* if scanning a tenured object update its remembered state */
			if (isTenureFrame && (isInSurvivor(forwardedAddress) || isInEvacuate(forwardedAddress))) {
				Debug_MM_true(!isInEvacuate(forwardedAddress) || isAbortedCycle());
				stackFrame->updateRememberedState(true);
			}

			/* check for flush() or push() */
			if (setStackLimit() || (_scanStackFrame != stackFrame)) {
				/* return to continue in new frame (object scanner may have changed if stackFrame was flushed) */
				return;
			}

			/* continue with next scanned slot object */
			slotObject = objectScanner->getNextSlot();
		}

		/* scan next non-leaf object in frame, if any */
		objectScanner = nextObjectScanner(stackFrame);
	}

	Debug_MM_true(stackFrame->getScanHead() == stackFrame->getCopyHead());

	/* pop scan stack */
	pop();
}

void
MM_Evacuator::flush()
{
	const bool isTenureFrame = (tenure == getEvacuationRegion(_scanStackFrame->getBase()));
	Debug_MM_true(evacuate > getEvacuationRegion(_scanStackFrame->getBase()));
	debugStack("flush");

	/* get the active object scanner without advancing slot */
	GC_ObjectScanner *objectScanner = nextObjectScanner(_scanStackFrame, false);
	while (NULL != objectScanner) {

		/* flush slots from active object scanner */
		GC_SlotObject *slotObject = objectScanner->getNextSlot();
		while (NULL != slotObject) {

			/* evacuate slot object to outside copyspace if it is in evacuate space */
			omrobjectptr_t object = slotObject->readReferenceFromSlot();
			if (isInEvacuate(object)) {

				/* slot object must be evacuated */
				MM_ForwardedHeader forwardedHeader(object);
				if (!forwardedHeader.isForwardedPointer()) {

					/* copy slot object to outside copyspace */
					uintptr_t slotObjectSizeBeforeCopy = 0, slotObjectSizeAfterCopy = 0, hotFieldAlignmentDescriptor = 0;
					EvacuationRegion evacuationRegion = isNurseryAge(_objectModel->getPreservedAge(&forwardedHeader)) ? survivor : tenure;
					_objectModel->calculateObjectDetailsForCopy(_env, &forwardedHeader, &slotObjectSizeBeforeCopy, &slotObjectSizeAfterCopy, &hotFieldAlignmentDescriptor);

					object = copyOutside(evacuationRegion, &forwardedHeader, slotObject->readAddressFromSlot(), slotObjectSizeBeforeCopy, slotObjectSizeAfterCopy);

				} else {

					/* get the forwarding address */
					object = forwardedHeader.getForwardedObject();
				}

				/* update slot with forwarding address */
				slotObject->writeReferenceToSlot(object);
			}

			/* update remembered state of object being scanned */
			if (isTenureFrame && (isInSurvivor(object) || isInEvacuate(object))) {
				Debug_MM_true(!isInEvacuate(object) || isAbortedCycle());
				_scanStackFrame->updateRememberedState(true);
			}

			/* if stall condition cleared resume copying inside top frame */
			if (setStackLimit()) {
				return;
			}

			/* get next slot from active object scanner */
			slotObject = objectScanner->getNextSlot();
		}

		/* get scanner for next object at scan head */
		objectScanner = nextObjectScanner(_scanStackFrame);
	}

	/* current stack frame has been completely scanned */
	Debug_MM_true(_scanStackFrame->getScanHead() == _scanStackFrame->getCopyHead());
}

void
MM_Evacuator::flushWork()
{
	MM_EvacuatorScanspace *topFrame = _scanStackFrame;
	_scanStackFrame = _stackBottom;

	/* flush frames from the bottom up to and excluding top frame until stall condition clears */
	while (_scanStackFrame <= topFrame) {

		/* flush current frame to end or until stall condition clears */
		flush();

		if ((_stackLimit < _stackCeiling) && (_scanStackFrame < topFrame)) {

			/* flushed scan stack frame to end but stall condition remains so move up to next frame */
			_scanStackFrame += 1;

		} else {

			/* stall condition cleared or top frame flushed so resume scanning top frame */
			break;
		}
	}

	_scanStackFrame = topFrame;
}

bool
MM_Evacuator::setStackLimit()
{
	if (!isBreadthFirst()) {

		if (_controller->areAnyEvacuatorsStalled()) {

			/* when there >0 stalled evacuators force bread-first flushing to outside copyspaces until this evacuator fills work quota */
			if (_stackLimit == _stackCeiling) {

				/* recalculate work release threshold to reflect stalling context and worklist volume */
				_workReleaseThreshold = _controller->calculateOptimalWorkPacketSize(getVolumeOfWork());

#if defined(EVACUATOR_DEBUG)
				if (_controller->_debugger.isDebugWork()) {
					OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
					omrtty_printf("%5lu %2llu %2llu:     limit; release:%llx; vow:%llx; ", _controller->getEpoch()->gc, (uint64_t)_controller->getEpoch()->epoch,
							(uint64_t)_workerIndex, (uint64_t)_workReleaseThreshold, getVolumeOfWork());
					_controller->printEvacuatorBitmap(_env, "stalled", _controller->sampleStalledMap());
					_controller->printEvacuatorBitmap(_env, "; resuming", _controller->sampleResumingMap());
					omrtty_printf("\n");
				}
#endif /* defined(EVACUATOR_DEBUG) */

				/* notify other evacautors if this one has work in its worklist */
				if (0 < getVolumeOfWork()) {
					_controller->notifyOfWork(this);
				}

				/* limit stack to force flushing until stall condition clears */
				_stackLimit = _stackBottom;

				/* flush frames from bottom of stack up to but not including current frame until stall condition clears */
				flushWork();

				return true;
			}

		} else if (_stackLimit < _stackCeiling) {

			/* recalculate work release threshold to reflect worklist volume after stall condition clears */
			_workReleaseThreshold = _controller->calculateOptimalWorkPacketSize(getVolumeOfWork());

#if defined(EVACUATOR_DEBUG)
			if (_controller->_debugger.isDebugWork()) {
				OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
				omrtty_printf("%5lu %2llu %2llu:   unlimit; release:%llx; vow:%llx; ", _controller->getEpoch()->gc, (uint64_t)_controller->getEpoch()->epoch,
						(uint64_t)_workerIndex, (uint64_t)_workReleaseThreshold, getVolumeOfWork());
				_controller->printEvacuatorBitmap(_env, "stalled", _controller->sampleStalledMap());
				_controller->printEvacuatorBitmap(_env, "; resuming", _controller->sampleResumingMap());
				omrtty_printf("\n");
			}
#endif /* defined(EVACUATOR_DEBUG) */

			if (_controller->hasFulfilledWorkQuota(getVolumeOfWork())) {

				/* restore the stack limit to allow inside copying */
				_stackLimit = _stackCeiling;

				return true;
			}
		}
	}

	/* stack limit has not changed */
	return false;
}

GC_ObjectScanner *
MM_Evacuator::nextObjectScanner(MM_EvacuatorScanspace * const scanspace, bool finalizeObjectScan)
{
	uintptr_t scannedBytes = 0;

	/* get current object scanner, if any */
	GC_ObjectScanner *objectScanner = scanspace->getActiveObjectScanner();

	/* object scanner is finalized after it returns its last slot */
	if (finalizeObjectScan && (NULL != objectScanner)) {

		/* advance scan head past parent object or split array segment */
		if (scanspace->isSplitArraySegment()) {

			scannedBytes = _splitArrayBytesToScan;
			if (objectScanner->isHeadObjectScanner()) {
				/* indexable object header and trailing remainder scanned bytes are counted with first array segment scanned */
				scannedBytes += (_objectModel->getConsumedSizeInBytesWithHeader(objectScanner->getParentObject()) - ((GC_IndexableObjectScanner *)objectScanner)->getDataSizeInBytes());
			}
			_splitArrayBytesToScan = 0;

		} else {

			scannedBytes = _objectModel->getConsumedSizeInBytesWithHeader(objectScanner->getParentObject());
		}

		/* update remembered state for parent object */
		if (isInTenure(scanspace->getScanHead()) && scanspace->getRememberedState()) {
			rememberObject(objectScanner->getParentObject());
		}
		scanspace->clearRememberedState();

		/* move scan head over scanned object (if not split array where scan is preset to end) and drop its object scanner */
		objectScanner = scanspace->advanceScanHead(scannedBytes);

		/* active object scanner was NULLed in advanceScanHead() */
		Debug_MM_true((NULL == objectScanner) && (objectScanner == scanspace->getActiveObjectScanner()));
	}

	/* advance scan head over leaf objects and objects with no scanner to set up next object scanner */
	while ((NULL == objectScanner) && (scanspace->getScanHead() < scanspace->getCopyHead())) {

		omrobjectptr_t objectPtr = (omrobjectptr_t)scanspace->getScanHead();
		objectScanner = _delegate.getObjectScanner(objectPtr, scanspace->getObjectScannerState(), GC_ObjectScanner::scanHeap);
		if ((NULL == objectScanner) || objectScanner->isLeafObject()) {

			/* nothing to scan for object at scan head, drop object scanner and advance scan head to next object in scanspace */
			uintptr_t bytes = _objectModel->getConsumedSizeInBytesWithHeader(objectPtr);
			objectScanner = scanspace->advanceScanHead(bytes);
			scannedBytes += bytes;

			/* active object scanner was NULLed in advanceScanHead() */
			Debug_MM_true((NULL == objectScanner) && (objectScanner == scanspace->getActiveObjectScanner()));
		}
	}

	/* update evacuator progress for epoch reporting */
	if (0 < scannedBytes) {
		_scannedBytesDelta += scannedBytes;
		if ((_scannedBytesDelta >= _copiedBytesReportingDelta) || ((_copiedBytesDelta[survivor] + _copiedBytesDelta[tenure]) >= _copiedBytesReportingDelta)) {
			_controller->reportProgress(this, _copiedBytesDelta, &_scannedBytesDelta);
		}
	}

	return objectScanner;
}

MM_EvacuatorScanspace *
MM_Evacuator::nextStackFrame(const EvacuationRegion evacuationRegion, MM_EvacuatorScanspace *frame)
{
	if (frame == _whiteStackFrame[otherOutsideRegion(evacuationRegion)]) {
		frame += 1;
	}

	if (frame >= _stackLimit) {
		frame = NULL;
	}

	Debug_MM_true((NULL == frame) || (frame == _whiteStackFrame[evacuationRegion]) || (frame->getScanHead() == frame->getEnd()));

	return frame;
}

MM_EvacuatorScanspace *
MM_Evacuator::reserveInsideCopyspace(const EvacuationRegion evacuationRegion, const uintptr_t whiteSize, const uintptr_t slotObjectSizeAfterCopy)
{
	Debug_MM_true(evacuate > evacuationRegion);
	Debug_MM_true((0 == whiteSize) || (whiteSize == _whiteStackFrame[evacuationRegion]->getWhiteSize()));
	Debug_MM_true(slotObjectSizeAfterCopy > whiteSize);

	/* find next frame that can receive whitespace for evacuation region */
	MM_EvacuatorScanspace *nextFrame = nextStackFrame(evacuationRegion, _scanStackFrame + 1);

	/* if when scanning an array of pointers and element is too big to fit in nondiscardable remainder it will repeat so relax discard threshold */
	if (NULL != nextFrame) {

		/* object won't fit in remaining whitespace in receiving frame -- trim the remainder to the whitelist */
		if (0 < whiteSize) {
			_whiteList[evacuationRegion].add(_whiteStackFrame[evacuationRegion]->trim());
		}

		/* try to get whitespace from top of the whitelist */
		MM_EvacuatorWhitespace *whitespace = _whiteList[evacuationRegion].top(slotObjectSizeAfterCopy);
		if (NULL == whitespace) {

			/* try to allocate whitespace from the heap */
			whitespace = _controller->getInsideFreespace(this, evacuationRegion, slotObjectSizeAfterCopy);
			if (NULL == whitespace) {
#if defined(EVACUATOR_DEBUG)
				if (_controller->_debugger.isDebugAllocate()) {
					OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
					omrtty_printf("%5lu %2llu %2llu:stack fail; %s stack; required:%llx\n", _controller->getEpoch()->gc, (uint64_t)_controller->getEpoch()->epoch,
							(uint64_t)getWorkerIndex(), ((MM_Evacuator::survivor == evacuationRegion) ? "survivor" : "tenure"), (uint64_t)slotObjectSizeAfterCopy);
				}
#endif /* defined(EVACUATOR_DEBUG) */

				/* failover to outside copy */
				_whiteStackFrame[evacuationRegion] = NULL;
				return NULL;
			}
		}

		/* set whitespace into next stack frame */
		nextFrame->setScanspace((uint8_t *)whitespace, (uint8_t *)whitespace, whitespace->length(), whitespace->isLOA());
		_whiteStackFrame[evacuationRegion] = nextFrame;

		/* receiving frame is holding enough stack whitespace to copy object to evacuation region */
		Debug_MM_true(evacuationRegion == getEvacuationRegion(_whiteStackFrame[evacuationRegion]->getBase()));
		Debug_MM_true(slotObjectSizeAfterCopy <= _whiteStackFrame[evacuationRegion]->getWhiteSize());
	}

	return nextFrame;
}

MM_EvacuatorScanspace *
MM_Evacuator::reserveRootCopyspace(const EvacuationRegion& evacuationRegion, uintptr_t slotObjectSizeAfterCopy)
{
	MM_EvacuatorScanspace *rootFrame = nextStackFrame(evacuationRegion, _stackBottom);

	if (NULL != rootFrame) {

		/* ensure capacity in evacuation region stack whitespace */
		MM_EvacuatorWhitespace *whitespace = NULL;
		MM_EvacuatorScanspace *whiteFrame = _whiteStackFrame[evacuationRegion];
		uintptr_t whiteSize = (NULL != whiteFrame) ? whiteFrame->getWhiteSize() : 0;
		if (slotObjectSizeAfterCopy <= whiteSize) {

			/* pull whitespace into root stack frame */
			if (rootFrame != _whiteStackFrame[evacuationRegion]) {
				rootFrame->pullWhitespace(_whiteStackFrame[evacuationRegion]);
				_whiteStackFrame[evacuationRegion] = rootFrame;
			}

		} else if (MM_EvacuatorBase::max_scanspace_remainder >= whiteSize) {

			/* object won't fit in remaining whitespace in receiving frame -- trim the remainder to the whitelist */
			if (0 < whiteSize) {
				_whiteList[evacuationRegion].add(whiteFrame->trim());
			}

			/* try to get whitespace from top of the whitelist */
			whitespace = _whiteList[evacuationRegion].top(slotObjectSizeAfterCopy);
			if (NULL == whitespace) {
				/* try to allocate whitespace from the heap */
				whitespace = _controller->getInsideFreespace(this, evacuationRegion, slotObjectSizeAfterCopy);
			}

			/* set whitespace into root stack frame, or not */
			if (NULL != whitespace) {
				/* set up to evacuate root object to next stack frame */
				rootFrame->setScanspace((uint8_t*) (whitespace), (uint8_t*) (whitespace), whitespace->length(), whitespace->isLOA());
			} else {
				/* failover to outside copy */
				rootFrame = NULL;
			}
			_whiteStackFrame[evacuationRegion] = rootFrame;

		} else {

			/* force outside copy */
			rootFrame = NULL;
		}
	}

	/* receiving frame is holding enough stack whitespace to copy object to evacuation region */
	Debug_MM_true((NULL == rootFrame) || (rootFrame == _whiteStackFrame[evacuationRegion]));
	Debug_MM_true((NULL == rootFrame) || (evacuationRegion == getEvacuationRegion(rootFrame->getBase())));
	Debug_MM_true((NULL == rootFrame) || (slotObjectSizeAfterCopy <= rootFrame->getWhiteSize()));

	return rootFrame;
}

omrobjectptr_t
MM_Evacuator::copyOutside(EvacuationRegion evacuationRegion, MM_ForwardedHeader *forwardedHeader, fomrobject_t *referringSlotAddress, const uintptr_t slotObjectSizeBeforeCopy, const uintptr_t slotObjectSizeAfterCopy)
{
	/* reserve copy space */
	const bool isSplitable = isSplitablePointerArray(forwardedHeader, slotObjectSizeAfterCopy);
	MM_EvacuatorCopyspace *effectiveCopyspace = reserveOutsideCopyspace(&evacuationRegion, slotObjectSizeAfterCopy, isSplitable);

	/* failure to evacuate must return original address in evacuate space */
	Debug_MM_true(!forwardedHeader->isForwardedPointer());
	omrobjectptr_t forwardingAddress = forwardedHeader->getObject();
	Debug_MM_true(isInEvacuate(forwardingAddress));

	/* copy slot object to effective outside copyspace */
	if (NULL != effectiveCopyspace) {
		Debug_MM_true(slotObjectSizeAfterCopy <= effectiveCopyspace->getWhiteSize());
		omrobjectptr_t copyHead = (omrobjectptr_t)effectiveCopyspace->getCopyHead();
		forwardingAddress = copyForward(forwardedHeader, referringSlotAddress, effectiveCopyspace, slotObjectSizeBeforeCopy, slotObjectSizeAfterCopy);
		if (copyHead == forwardingAddress) {

			/* object copied into effective copyspace -- check for sharable work to distribute */
			if (effectiveCopyspace == &_largeCopyspace) {

				/* add large object to the worklist to free large copyspace for reuse */
				if (isSplitable) {

					/* record 1-based array offsets to mark split pointer array work packets */
					splitPointerArrayWork(copyHead);
					/* prepare the large object copyspace for next use */
					_largeCopyspace.reset();

				} else {

					/* set up a work packet containing a single large scalar or primitive/non-splitable pointer array object */
					MM_EvacuatorWorkPacket *work = _freeList.next();
					Debug_MM_true((slotObjectSizeAfterCopy == _largeCopyspace.getCopySize()) && (0 == _largeCopyspace.getWhiteSize()));
					work->base = (omrobjectptr_t)_largeCopyspace.rebase(&work->length);
					addWork(work);
				}

			} else if ((effectiveCopyspace != _scanStackFrame) && (effectiveCopyspace->getCopySize() >= _workReleaseThreshold)) {

				/* pull work from head of effective copyspace into a work packet, leaving only trailing whitespace in copyspace */
				MM_EvacuatorWorkPacket *work = _freeList.next();
				work->base = (omrobjectptr_t)effectiveCopyspace->rebase(&work->length);
				addWork(work);
			}

		} else if (effectiveCopyspace == &_largeCopyspace) {

			/* object copied by other thread so clip reserved whitespace from large object copyspace onto the whitelist */
			_whiteList[evacuationRegion].add(_largeCopyspace.trim());
			/* prepare the large object copyspace for next use */
			_largeCopyspace.reset();
		}
	}

	return forwardingAddress;
}

MM_EvacuatorCopyspace *
MM_Evacuator::reserveOutsideCopyspace(EvacuationRegion *evacuationRegion, const uintptr_t slotObjectSizeAfterCopy, const bool isSplittable)
{
	MM_EvacuatorCopyspace *copyspace = NULL;
	MM_EvacuatorWhitespace *whitespace = NULL;

	/* splittable object is a pointer array to be copied to large object copyspace and split into work packets */
	const EvacuationRegion preferredRegion = *evacuationRegion;
	if (!isSplittable) {

		copyspace = &_copyspace[preferredRegion];
		uintptr_t copyspaceRemainder = copyspace->getWhiteSize();
		if (slotObjectSizeAfterCopy > copyspaceRemainder) {
			/* use an outside copyspace unless required to large object space or object will not fit in copyspace whitespace remainder > max_copyspace_remainder */
			copyspace = NULL;

			/* take whitespace from the preferred region whitelist if it is presenting a large space on top */
			if ((slotObjectSizeAfterCopy <= _whiteList[preferredRegion].top()) && (MM_EvacuatorBase::max_copyspace_remainder <= _whiteList[preferredRegion].top())) {

				/* take top whitespace to load into preferred region copyspace */
				whitespace = _whiteList[preferredRegion].top(slotObjectSizeAfterCopy);

			} else {

				/* allow large whitespace remainder truncation of after failing to accommodate a series of larger objects */
				bool allowTruncation = (MM_EvacuatorBase::max_large_object_sequence <= _largeObjectCounter[preferredRegion]);
				/* try to allocate whitespace from the preferred region */
				whitespace = _controller->getOutsideFreespace(this, preferredRegion, (allowTruncation ? 0 : copyspaceRemainder), slotObjectSizeAfterCopy);
				if (NULL == whitespace) {

					/* try the other evacuation region (survivor/tenure) */
					*evacuationRegion = otherOutsideRegion(preferredRegion);
					copyspaceRemainder = _copyspace[*evacuationRegion].getWhiteSize();
					if (slotObjectSizeAfterCopy > copyspaceRemainder) {

						/* try to allocate from other region whitelist if it is presenting a large chunk on top */
						if ((slotObjectSizeAfterCopy <= _whiteList[*evacuationRegion].top()) && (_whiteList[*evacuationRegion].top() >= MM_EvacuatorBase::max_copyspace_remainder)) {

							whitespace = _whiteList[*evacuationRegion].top(slotObjectSizeAfterCopy);

						} else {

							/* allow large whitespace remainder truncation of after failing to accommodate a series of larger objects */
							allowTruncation = (MM_EvacuatorBase::max_large_object_sequence <= _largeObjectCounter[*evacuationRegion]);
							/* try to allocate from the other region */
							whitespace = _controller->getOutsideFreespace(this, *evacuationRegion, (allowTruncation ? 0 : copyspaceRemainder), slotObjectSizeAfterCopy);
							if (NULL == whitespace) {

								/* last chance -- outside regions and whitelists exhausted, try to steal the whitespace from the stack */
								if ((NULL != _whiteStackFrame[preferredRegion]) && (slotObjectSizeAfterCopy <= _whiteStackFrame[preferredRegion]->getWhiteSize())) {

									whitespace = _whiteStackFrame[preferredRegion]->trim();
									*evacuationRegion = preferredRegion;

								} else if ((NULL != _whiteStackFrame[*evacuationRegion]) && (slotObjectSizeAfterCopy <= _whiteStackFrame[*evacuationRegion]->getWhiteSize())) {

									whitespace = _whiteStackFrame[*evacuationRegion]->trim();

								} else {

									*evacuationRegion = unreachable;
								}
							}
						}

					} else {

						/* the other outside region has enough whitespace to receive the object */
						copyspace = &_copyspace[*evacuationRegion];
					}
				}
			}
		}

	} else {

		/* allocate exactly enough heap memory to contain the large object */
		const uintptr_t copyspaceRemainder = MM_EvacuatorBase::max_copyspace_remainder;
		whitespace = _controller->getOutsideFreespace(this, preferredRegion, copyspaceRemainder, slotObjectSizeAfterCopy);
		if (NULL == whitespace) {

			const EvacuationRegion otherRegion = otherOutsideRegion(preferredRegion);
			whitespace = _controller->getOutsideFreespace(this, otherRegion, copyspaceRemainder, slotObjectSizeAfterCopy);
			*evacuationRegion = (NULL != whitespace) ? otherRegion : unreachable;

		}
		Debug_MM_true((NULL == whitespace) || (slotObjectSizeAfterCopy == whitespace->length()));
	}

	if (NULL == copyspace) {

		if (NULL != whitespace) {
			Debug_MM_true(slotObjectSizeAfterCopy <= whitespace->length());
			Debug_MM_true(*evacuationRegion == getEvacuationRegion(whitespace));

			/* pull whitespace from evacuation region into outside or large copyspace */
			if (!isSplittable && (slotObjectSizeAfterCopy < whitespace->length())) {

				/* select receiving outside copyspace to receive object with whitespace to spare */
				_largeObjectCounter[*evacuationRegion] = 0;
				copyspace = &_copyspace[*evacuationRegion];

				/* distribute any existing work in copyspace */
				if (0 < copyspace->getCopySize()) {
					MM_EvacuatorWorkPacket *work = _freeList.next();
					work->base = (omrobjectptr_t)copyspace->rebase(&work->length);
					addWork(work);
				}

				/* trim whitespace from copyspace to whitelist */
				_whiteList[*evacuationRegion].add(copyspace->trim());

			} else {

				/* select large copyspace as receiver -- whitespace size fits object shape exactly */
				if (_controller->_minimumCopyspaceSize > slotObjectSizeAfterCopy) {
					_largeObjectCounter[*evacuationRegion] += 1;
				}
				copyspace = &_largeCopyspace;

				Debug_MM_true4(_env, (slotObjectSizeAfterCopy >= MM_EvacuatorBase::max_copyspace_remainder), "object should fit in outside %s copyspace tail: object=%llx; survivor-tail=%llx; tenure-tail=%llx\n",
						((survivor == *evacuationRegion) ? "survivor" : "tenure"), slotObjectSizeAfterCopy, _copyspace[survivor].getWhiteSize(), _copyspace[tenure].getWhiteSize());
				Debug_MM_true(slotObjectSizeAfterCopy == whitespace->length());
				Debug_MM_true(0 == copyspace->getCopySize());
				Debug_MM_true(0 == copyspace->getWhiteSize());
			}

			/* pull the reserved whitespace into the selected copyspace */
			copyspace->setCopyspace((uint8_t*)whitespace, (uint8_t*)whitespace, whitespace->length(), whitespace->isLOA());

		} else {

			/* abort the evacuation and broadcast this to other evacuators through controller */
			if (!setAbortedCycle()) {
				/* do this only if this evacuator is first to set the abort condition */
#if defined(EVACUATOR_DEBUG)
				if (_controller->_debugger.isDebugEnd()) {
					OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
					omrtty_printf("%5lu %2llu %2llu:     abort; flags:%llx", _controller->getEpoch()->gc, (uint64_t)_controller->getEpoch()->epoch, (uint64_t)_workerIndex, (uint64_t)_controller->sampleEvacuatorFlags());
					if (NULL != _scanStackFrame) {
						omrtty_printf("; scanning:0x%llx", (uint64_t)_scanStackFrame->getScanHead());
					}
					omrtty_printf("\n");
				}
#endif /* defined(EVACUATOR_DEBUG) */
			}

			/* record every allocation failure after abort condition raised */
			if (survivor == preferredRegion) {
				_stats->_failedFlipCount += 1;
				_stats->_failedFlipBytes += slotObjectSizeAfterCopy;
			} else {
				_stats->_failedTenureCount += 1;
				_stats->_failedTenureBytes += slotObjectSizeAfterCopy;
				_stats->_failedTenureLargest = OMR_MAX(slotObjectSizeAfterCopy, _stats->_failedTenureLargest);
			}
		}

	} else {

		if (0 < _largeObjectCounter[*evacuationRegion]) {
			_largeObjectCounter[*evacuationRegion] -= 1;
		}
	}

	return copyspace;
}

omrobjectptr_t
MM_Evacuator::copyForward(GC_SlotObject *slotObject, MM_EvacuatorCopyspace * const copyspace, const uintptr_t originalLength, uintptr_t const forwardedLength)
{
	omrobjectptr_t object = slotObject->readReferenceFromSlot();
	if (isInEvacuate(object)) {
		MM_ForwardedHeader forwardedHeader(object);
		object = copyForward(&forwardedHeader, slotObject->readAddressFromSlot(), copyspace, originalLength, forwardedLength);
		slotObject->writeReferenceToSlot(object);
	}
	return object;
}

omrobjectptr_t
MM_Evacuator::copyForward(MM_ForwardedHeader *forwardedHeader, fomrobject_t *referringSlotAddress, MM_EvacuatorCopyspace * const copyspace, const uintptr_t originalLength, const uintptr_t forwardedLength)
{
	/* if not already forwarded object will be copied to copy head in designated copyspace */
	omrobjectptr_t const copyHead = (omrobjectptr_t)copyspace->getCopyHead();
	Debug_MM_true(isInSurvivor(copyHead) || isInTenure(copyHead));

	/* try to set forwarding address to the copy head in copyspace; otherwise do not copy, just return address forwarded by another thread */
	omrobjectptr_t forwardedAddress = forwardedHeader->isForwardedPointer() ? forwardedHeader->getForwardedObject() : forwardedHeader->setForwardedObject(copyHead);
	if (forwardedAddress == copyHead) {
#if defined(OMR_VALGRIND_MEMCHECK)
		valgrindMempoolAlloc(_env->getExtensions(), (uintptr_t)forwardedAddress, forwardedLength);
#endif /* defined(OMR_VALGRIND_MEMCHECK) */
#if defined(EVACUATOR_DEBUG)
		_delegate.debugValidateObject(forwardedHeader);
#endif /* defined(EVACUATOR_DEBUG) */

		/* forwarding address set by this thread -- object will be evacuated to the copy head in copyspace */
		memcpy(forwardedAddress, forwardedHeader->getObject(), originalLength);

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
		/* update proximity and size metrics */
		_stats->countCopyDistance((uintptr_t)referringSlotAddress, (uintptr_t)forwardedAddress);
		_stats->countObjectSize(forwardedLength, _maxInsideCopySize);
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

		/* update scavenger stats */
		uintptr_t objectAge = _objectModel->getPreservedAge(forwardedHeader);
		if (isInTenure(forwardedAddress)) {
			_stats->_tenureAggregateCount += 1;
			_stats->_tenureAggregateBytes += originalLength;
			_stats->getFlipHistory(0)->_tenureBytes[objectAge + 1] += forwardedLength;
#if defined(OMR_GC_LARGE_OBJECT_AREA)
			if (copyspace->isLOA()) {
				_stats->_tenureLOACount += 1;
				_stats->_tenureLOABytes += originalLength;
			}
#endif /* OMR_GC_LARGE_OBJECT_AREA */
			_copiedBytesDelta[tenure] += forwardedLength;
		} else {
			Debug_MM_true(isInSurvivor(forwardedAddress));
			_stats->_flipCount += 1;
			_stats->_flipBytes += originalLength;
			_stats->getFlipHistory(0)->_flipBytes[objectAge + 1] += forwardedLength;
			_copiedBytesDelta[survivor] += forwardedLength;
		}

		/* update object age and finalize copied object header */
		if (tenure == getEvacuationRegion(forwardedAddress)) {
			objectAge = STATE_NOT_REMEMBERED;
		} else if (objectAge < OBJECT_HEADER_AGE_MAX) {
			objectAge += 1;
		}

		/* copy the preserved fields from the forwarded header into the destination object */
		forwardedHeader->fixupForwardedObject(forwardedAddress);
		/* object model fixes the flags in the destination object */
		_objectModel->fixupForwardedObject(forwardedHeader, forwardedAddress, objectAge);

#if defined(EVACUATOR_DEBUG)
		if (_controller->_debugger.isDebugCopy()) {
			OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
			char className[32];
			omrobjectptr_t parent = (NULL != _scanStackFrame) ? _scanStackFrame->getActiveObjectScanner()->getParentObject() : NULL;
			omrtty_printf("%5lu %2llu %2llu:%c copy %3s; base:%llx; copy:%llx; end:%llx; free:%llx; %llx %s %llx -> %llx %llx\n", _controller->getEpoch()->gc, (uint64_t)_controller->getEpoch()->epoch, (uint64_t)_workerIndex,
					(NULL != _scanStackFrame) && ((uint8_t *)forwardedAddress >= _scanStackFrame->getBase()) && ((uint8_t *)forwardedAddress < _scanStackFrame->getEnd()) ? 'I' : 'O',
					isInSurvivor(forwardedAddress) ? "new" : "old",	(uint64_t)copyspace->getBase(), (uint64_t)copyspace->getCopyHead(), (uint64_t)copyspace->getEnd(), (uint64_t)copyspace->getWhiteSize(),
					(uint64_t)parent, _delegate.debugGetClassname(forwardedAddress, className, 32), (uint64_t)forwardedHeader->getObject(), (uint64_t)forwardedAddress, (uint64_t)forwardedLength);
		}
#endif /* defined(EVACUATOR_DEBUG) */

		/* advance the copy head in the receiving copyspace and track scan/copy progress */
		copyspace->advanceCopyHead(forwardedLength);
		if ((_scannedBytesDelta >= _copiedBytesReportingDelta) || ((_copiedBytesDelta[survivor] + _copiedBytesDelta[tenure]) >= _copiedBytesReportingDelta)) {
			_controller->reportProgress(this, _copiedBytesDelta, &_scannedBytesDelta);
		}

#if defined(EVACUATOR_DEBUG)
		_delegate.debugValidateObject(forwardedAddress);
#endif /* defined(EVACUATOR_DEBUG) */
	}

	Debug_MM_true(isInSurvivor(forwardedAddress) || isInTenure(forwardedAddress) || (isInEvacuate(forwardedAddress) && isAbortedCycle()));
	return forwardedAddress;
}

bool
MM_Evacuator::isSplitablePointerArray(MM_ForwardedHeader *forwardedHeader, uintptr_t objectSizeInBytes)
{
	/* large pointer arrays are split by default but scanners for these objects may inhibit splitting eg when pruning remembered set */
	return ((MM_EvacuatorBase::min_split_indexable_size < objectSizeInBytes) && _delegate.isIndexablePointerArray(forwardedHeader));
}

void
MM_Evacuator::splitPointerArrayWork(omrobjectptr_t pointerArray)
{
	uintptr_t elements = 0;
	_delegate.getIndexableDataBounds(pointerArray, &elements);

	/* distribute elements to segments as evenly as possible and take largest segment first */
	uintptr_t segments = elements / MM_EvacuatorBase::max_split_segment_elements;
	if (0 != (elements % MM_EvacuatorBase::max_split_segment_elements)) {
		segments += 1;
	}
	uintptr_t elementsPerSegment = elements / segments;
	uintptr_t elementsThisSegment = elementsPerSegment + (elements % segments);

	omrthread_monitor_enter(_mutex);

	/* record 1-based array offsets to mark split array work packets */
	uintptr_t offset = 1;
	while (0 < segments) {
		/* add each array segment as a work packet */
		MM_EvacuatorWorkPacket* work = _freeList.next();
		work->base = pointerArray;
		work->offset = offset;
		work->length = elementsThisSegment;
		_workList.add(work);
		offset += elementsThisSegment;
		elementsThisSegment = elementsPerSegment;
		segments -= 1;
	}

	omrthread_monitor_exit(_mutex);

	/* tell the world about it */
	_controller->notifyOfWork(this);
}

bool
MM_Evacuator::shouldRememberObject(omrobjectptr_t objectPtr)
{
	Debug_MM_true((NULL != objectPtr) && isInTenure(objectPtr));
	Debug_MM_true(_objectModel->getRememberedBits(objectPtr) < (uintptr_t)0xc0);

	/* scan object for referents in the nursery */
	GC_ObjectScannerState objectScannerState;
	GC_ObjectScanner *objectScanner = _delegate.getObjectScanner(objectPtr, &objectScannerState, GC_ObjectScanner::scanRoots);
	if (NULL != objectScanner) {

		GC_SlotObject *slotPtr;
		while (NULL != (slotPtr = objectScanner->getNextSlot())) {

			omrobjectptr_t slotObjectPtr = slotPtr->readReferenceFromSlot();
			if (NULL != slotObjectPtr) {

				if (isInSurvivor(slotObjectPtr)) {

					return true;
				}

				Debug_MM_true(isInTenure(slotObjectPtr) || (isInEvacuate(slotObjectPtr) && isAbortedCycle()));
			}
		}
	}

	/* the remembered state of a class object also depends on the class statics */
	if (_objectModel->hasIndirectObjectReferents((CLI_THREAD_TYPE*)_env->getLanguageVMThread(), objectPtr)) {

		return _delegate.objectHasIndirectObjectsInNursery(objectPtr);
	}

	return false;
}

bool
MM_Evacuator::rememberObject(omrobjectptr_t object)
{
	Debug_MM_true(isInTenure(object));
	Debug_MM_true(_objectModel->getRememberedBits(object) < (uintptr_t)0xc0);

	/* try to set the REMEMBERED bit in the flags field (if it hasn't already been set) */
	bool rememberedByThisEvacuator = _objectModel->atomicSetRememberedState(object, STATE_REMEMBERED);
	if (rememberedByThisEvacuator) {
		Debug_MM_true(_objectModel->isRemembered(object));

		/* the object has been successfully marked as REMEMBERED - allocate an entry in the remembered set */
		if ((_env->_scavengerRememberedSet.fragmentCurrent < _env->_scavengerRememberedSet.fragmentTop) ||
				(0 == allocateMemoryForSublistFragment(_env->getOmrVMThread(), (J9VMGC_SublistFragment*)&_env->_scavengerRememberedSet))
		) {

			/* there is at least 1 free entry in the fragment - use it */
			_env->_scavengerRememberedSet.count++;
			uintptr_t *rememberedSetEntry = _env->_scavengerRememberedSet.fragmentCurrent++;
			*rememberedSetEntry = (uintptr_t)object;

		} else {

			/* failed to allocate a fragment - set the remembered set overflow state */
			if (!_env->getExtensions()->isRememberedSetInOverflowState()) {
				_stats->_causedRememberedSetOverflow = 1;
			}
			_env->getExtensions()->setRememberedSetOverflowState();
		}
	}

	Debug_MM_true(_objectModel->getRememberedBits(object) < (uintptr_t)0xc0);
	return rememberedByThisEvacuator;
}

void
MM_Evacuator::reserveWhitespace(MM_EvacuatorWhitespace *whitespace)
{
	EvacuationRegion whiteRegion = getEvacuationRegion(whitespace);
	Debug_MM_true((NULL == whitespace) || (whiteRegion < evacuate));
	_whiteList[whiteRegion].add(whitespace);
}

#if defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
uint64_t
MM_Evacuator::startWaitTimer()
{
	OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
#if defined(EVACUATOR_DEBUG)
	if (_controller->_debugger.isDebugWork()) {
		omrtty_printf("%5lu %2llu %2llu:     stall; ", _controller->getEpoch()->gc, (uint64_t)_controller->getEpoch()->epoch, (uint64_t)_workerIndex);
		_controller->printEvacuatorBitmap(_env, "stalled", _controller->sampleStalledMap());
		_controller->printEvacuatorBitmap(_env, "; resuming", _controller->sampleResumingMap());
		omrtty_printf("; flags:%llx; vow:%llx\n", (uint64_t)_controller->sampleEvacuatorFlags(), getVolumeOfWork());
	}
#endif /* defined(EVACUATOR_DEBUG) */
	return omrtime_hires_clock();
}

void
MM_Evacuator::endWaitTimer(uint64_t waitStartTime, MM_EvacuatorWorkPacket *work)
{
	OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
	uint64_t waitEndTime = omrtime_hires_clock();

#if defined(EVACUATOR_DEBUG)
	if (_controller->_debugger.isDebugWork()) {
		uint64_t waitMicros = omrtime_hires_delta(waitStartTime, waitEndTime, OMRPORT_TIME_DELTA_IN_MICROSECONDS);
		omrtty_printf("%5lu %2llu %2llu:    resume; ", _controller->getEpoch()->gc, (uint64_t)_controller->getEpoch()->epoch, (uint64_t)_workerIndex);
		_controller->printEvacuatorBitmap(_env, "stalled", _controller->sampleStalledMap());
		_controller->printEvacuatorBitmap(_env, "; resuming", _controller->sampleResumingMap());
		omrtty_printf("; flags:%llx; vow:%llx; work:0x%llx; length:%llx; micros:%llu\n", _controller->sampleEvacuatorFlags(),
				getVolumeOfWork(), (uint64_t)work, (uint64_t)((NULL != work) ? work->length : 0), waitMicros);
	}
#endif /* defined(EVACUATOR_DEBUG) */

#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
	if (NULL == work) {
		_stats->addToCompleteStallTime(waitStartTime, waitEndTime);
	} else {
		_stats->addToWorkStallTime(waitStartTime, waitEndTime);
	}
#endif /* defined(J9MODRON_TGC_PARALLEL_STATISTICS) */
}
#endif /* defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

#if defined(EVACUATOR_DEBUG)
void
MM_Evacuator::debugStack(const char *stackOp, bool treatAsWork)
{
	if (_controller->_debugger.isDebugStack() || (treatAsWork && (_controller->_debugger.isDebugWork() || _controller->_debugger.isDebugBackout()))) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
		MM_EvacuatorScanspace *scanspace = (NULL != _scanStackFrame) ? _scanStackFrame : _stackBottom;
		EvacuationRegion region = getEvacuationRegion(scanspace->getBase());
		char isWhiteFrame = (evacuate > region) && (_whiteStackFrame[region] == scanspace) ? '*' : ' ';
		const char *whiteRegion = (survivor == region) ? "survivor" : "tenure";
		omrtty_printf("%5lu %2llu %2llu:%6s[%2d];%cbase:%llx; copy:%llx; end:%llx; %s:%llx; scan:%llx; unscanned:%llx; sow:%llx; tow:%llx\n",
				_controller->getEpoch()->gc, (uint64_t)_controller->getEpoch()->epoch, (uint64_t)_workerIndex, (uint64_t)stackOp, (uint64_t)(scanspace - _stackBottom),
				isWhiteFrame, (uint64_t)scanspace->getBase(), (uint64_t)scanspace->getCopyHead(), (uint64_t)scanspace->getEnd(),
				whiteRegion, (uint64_t)scanspace->getWhiteSize(), (uint64_t)scanspace->getScanHead(), (uint64_t)scanspace->getWorkSize(),
				(uint64_t)_copyspace[survivor].getCopySize(), (uint64_t)(uint64_t)_copyspace[tenure].getCopySize());
	}
}

void
MM_Evacuator::checkSurvivor()
{
	OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
	omrobjectptr_t object = (omrobjectptr_t)(_heapBounds[survivor][0]);
	omrobjectptr_t end = (omrobjectptr_t)(_heapBounds[survivor][1]);
	while (isInSurvivor(object)) {
		while (isInSurvivor(object) && _objectModel->isDeadObject(object)) {
			object = (omrobjectptr_t)((uintptr_t)object + _objectModel->getSizeInBytesDeadObject(object));
		}
		if (isInSurvivor(object)) {
			_delegate.debugValidateObject(object);
			object = (omrobjectptr_t)((uintptr_t)object + _objectModel->getConsumedSizeInBytesWithHeader(object));
		}
	}
	omrtty_printf("%5lu %2llu %2llu:  survivor; end:%llx\n", _controller->getEpoch()->gc, (uint64_t)_controller->getEpoch()->epoch, (uint64_t)_workerIndex, (uint64_t)object);
	Debug_MM_true(object == end);
}

void
MM_Evacuator::checkTenure()
{
	OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
	MM_GCExtensionsBase *extensions = _env->getExtensions();
	omrobjectptr_t object = (omrobjectptr_t)(_heapBounds[tenure][0]);
	omrobjectptr_t end = (omrobjectptr_t)(_heapBounds[tenure][1]);
	while (extensions->isOld(object)) {
		while (extensions->isOld(object) && _objectModel->isDeadObject(object)) {
			object = (omrobjectptr_t)((uintptr_t)object + _objectModel->getSizeInBytesDeadObject(object));
		}
		if (extensions->isOld(object)) {
			_delegate.debugValidateObject(object);
			Debug_MM_true(_objectModel->getRememberedBits(object) < (uintptr_t)0xc0);
			if (_objectModel->isRemembered(object)) {
				if (!shouldRememberObject(object)) {
					omrtty_printf("%5lu %2llu %2llu:downgraded; object:%llx; flags:%llx\n", _controller->getEpoch()->gc, (uint64_t)_controller->getEpoch()->epoch, (uint64_t)_workerIndex, (uint64_t)object, (uint64_t)_objectModel->getObjectFlags(object));
				}
			} else if (shouldRememberObject(object)) {
				omrtty_printf("%5lu %2llu %2llu: !remember; object:%llx; flags:%llx\n", _controller->getEpoch()->gc, (uint64_t)_controller->getEpoch()->epoch, (uint64_t)_workerIndex, (uint64_t)object, (uint64_t)_objectModel->getObjectFlags(object));
				Debug_MM_true(isAbortedCycle());
			}
			object = (omrobjectptr_t)((uintptr_t)object + _objectModel->getConsumedSizeInBytesWithHeader(object));
		}
	}
	omrtty_printf("%5lu %2llu %2llu:    tenure; end:%llx\n", _controller->getEpoch()->gc, (uint64_t)_controller->getEpoch()->epoch, (uint64_t)_workerIndex, (uint64_t)object);
	Debug_MM_true(object == end);
}
#else
void MM_Evacuator::debugStack(const char *stackOp, bool treatAsWork) { }
#endif /* defined(EVACUATOR_DEBUG) */
