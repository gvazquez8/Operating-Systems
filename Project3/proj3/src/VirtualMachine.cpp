#include "VirtualMachine.h"
#include "Machine.h"
#include <iostream>
#include <vector>
#include <queue>

extern "C" {
	// Stuff for functions in headers
	#define VM_TIMEOUT_INFINITE                     ((TVMTick)0)
	#define VM_TIMEOUT_IMMEDIATE                    ((TVMTick)-1)

	#define VM_THREAD_STATE_DEAD                    ((TVMThreadState)0x00)
	#define VM_THREAD_STATE_RUNNING                 ((TVMThreadState)0x01)
	#define VM_THREAD_STATE_READY                   ((TVMThreadState)0x02)
	#define VM_THREAD_STATE_WAITING                 ((TVMThreadState)0x03)

	#define VM_THREAD_PRIORITY_NONE                 ((TVMThreadPriority)0x00)
	#define VM_THREAD_PRIORITY_LOW                  ((TVMThreadPriority)0x01)
	#define VM_THREAD_PRIORITY_NORMAL               ((TVMThreadPriority)0x02)
	#define VM_THREAD_PRIORITY_HIGH                 ((TVMThreadPriority)0x03)

	typedef void (*TVMMainEntry) (int, char* []);
	typedef void (*TMachineAlarmCallback) (void* calldata);
	typedef void (*TVMThreadEntry)(void*);
	typedef void (*TMachineFileCallback)(void *calldata, int result);
	typedef sigset_t TMachineSignalState, *TMachineSignalStateRef;

	TVMMainEntry VMLoadModule(const char* module);
	void VMUnloadModule(void);

	// Store the tickms arg that was passed when starting the program
	volatile int tickTime;
	// tickCount stores the number of ticks since start
	volatile TVMTick totalTickCount = 0;

	// Struct for FileOpen
	struct callBackDataStorage {
			TVMThreadID id;
			int* resultPtr;
	};
	// TCB
	class Thread {
	public:
			TVMThreadID id;
			TVMThreadState state;
			TVMThreadPriority prio;
			TVMThreadEntry entry;
			void* args;
			SMachineContext cntx;
			TVMMemorySize memsize;
			void* stackaddr;
			int sleepCountdown;
			int mtxWaitTime;
	};

	class Mutex {
		public:
			TVMMutexID mtxId;
			TVMThreadID owner;
			bool isLocked;
			std::vector<std::queue<unsigned int>> waitingQ;
	};

	volatile TVMThreadID currThread = 1;

	std::vector<Thread> threadList;
	std::vector<Mutex> mutexList;
	// 1 = LOW, 2 = NORMAL, 3 = HIGH
	std::vector<std::queue<unsigned int>> readyThreads;
	std::vector<unsigned int> sleepingThreads;

	void dispatch(TVMThreadID next) {

		if (threadList[currThread].state == VM_THREAD_STATE_READY) {
			readyThreads[threadList[currThread].prio].push(threadList[currThread].id);
		}

		TVMThreadID prev = currThread;
		currThread = next;
		//std::cout << "Going from " << prev << " to " << next << std::endl;

		threadList[currThread].state = VM_THREAD_STATE_RUNNING;
		MachineContextSwitch(&threadList[prev].cntx, &threadList[currThread].cntx);
	}

	void schedule(int scheduleEqualPrio) {

		TVMThreadID nextThread;

		if (scheduleEqualPrio == 1) {
			if (readyThreads[threadList[currThread].prio].size() != 0) {
				nextThread = readyThreads[threadList[currThread].prio].front();
				readyThreads[threadList[currThread].prio].pop();
				dispatch(nextThread);
			}
			return;
		}

		if (!readyThreads[3].empty()) {
			nextThread = readyThreads[3].front();
			readyThreads[3].pop();
		} else if (!readyThreads[2].empty()) {
			nextThread = readyThreads[2].front();
			readyThreads[2].pop();
		} else if (!readyThreads[1].empty()) {
			nextThread = readyThreads[1].front();
			readyThreads[1].pop();
		} else {
			nextThread = readyThreads[0].front();
			readyThreads[0].pop();
		}

		dispatch(nextThread);
	}

	void timerCallback(void* calldata) {
		TMachineSignalState signalState;
		MachineSuspendSignals(&signalState);
		totalTickCount++;

		// Check on Sleeping Threads
		for (unsigned int i = 0; i < sleepingThreads.size(); i++) {
			threadList[sleepingThreads[i]].sleepCountdown -=1;
			if (threadList[sleepingThreads[i]].sleepCountdown <= 0) {
				threadList[sleepingThreads[i]].state = VM_THREAD_STATE_READY;
				readyThreads[threadList[sleepingThreads[i]].prio].push(sleepingThreads[i]);
				sleepingThreads.erase(sleepingThreads.begin()+i);
				i--;
			}
		}

		// Check on mutex queues ?

		if (threadList[currThread].state != VM_THREAD_STATE_DEAD) {
			threadList[currThread].state = VM_THREAD_STATE_READY;
		}
		schedule(0);
		MachineResumeSignals(&signalState);
	}

	void fileCallBack(void *calldata, int result) {
		TMachineSignalState signalState;
		MachineSuspendSignals(&signalState);
		callBackDataStorage *args = (callBackDataStorage*) calldata;
		*(args->resultPtr) = result;
		threadList[args->id].state = VM_THREAD_STATE_READY;
		readyThreads[threadList[args->id].prio].push(args->id);
		if (threadList[args->id].prio > threadList[currThread].prio) {
			threadList[currThread].state = VM_THREAD_STATE_READY;
			schedule(0);
		}
		MachineResumeSignals(&signalState);
	}

	void skeleton(void* param) {
		MachineEnableSignals();
		threadList[currThread].entry(threadList[currThread].args);
		VMThreadTerminate(currThread);
	}

	void idleFunction(void* param) {
		while(true) {}
	}

	void VMCreateIdleThread() {
		Thread *idleThread = new Thread();
		idleThread->state = VM_THREAD_STATE_READY;
		idleThread->entry = &idleFunction;
		idleThread->args = NULL;
		idleThread->prio = VM_THREAD_PRIORITY_NONE;
		idleThread->id = threadList.size();
		idleThread->sleepCountdown = 0;
		idleThread->mtxWaitTime = 0;
		idleThread->memsize = 0x100000;
		idleThread->stackaddr = malloc(idleThread->memsize * sizeof(TVMMemorySize));
		threadList.push_back(*idleThread);
		MachineContextCreate(&threadList[0].cntx, &skeleton, threadList[0].args,
								threadList[0].stackaddr, threadList[0].memsize);
		return;
	}

	void VMCreateMainThread(TVMMainEntry VMMain, char* argv[]) {

		Thread *mainThread = new Thread();
		mainThread->state = VM_THREAD_STATE_RUNNING;
		mainThread->entry = (TVMThreadEntry) VMMain;
		mainThread->args = argv;
		mainThread->prio = VM_THREAD_PRIORITY_NORMAL;
		mainThread->id = threadList.size();
		mainThread->sleepCountdown = 0;
		mainThread->mtxWaitTime = 0;

		threadList.push_back(*mainThread);
	}

	TVMStatus VMStart(int tickms, TVMMemorySize , int argc, char* argv[]) {
		readyThreads.resize(4);

		TVMMainEntry VMMain = VMLoadModule(argv[0]);

		if (VMMain == NULL) {return VM_STATUS_FAILURE;}

		tickTime = tickms;
		MachineInitialize();
		MachineEnableSignals();

		// create the idle and main thread;
		VMCreateIdleThread();
		VMCreateMainThread(VMMain, argv);

		// create alarm for tick incrementing
		useconds_t tickus = tickms * 1000;
		MachineRequestAlarm(tickus, timerCallback, NULL);
		VMMain(argc, argv);
		MachineTerminate();
		VMUnloadModule();

		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMTickMS(int *tickmsref) {
		TMachineSignalState signalState;
		MachineSuspendSignals(&signalState);
		if (tickmsref == NULL) {
			MachineResumeSignals(&signalState);
			return VM_STATUS_ERROR_INVALID_PARAMETER;
		}
		*tickmsref = tickTime;
		MachineResumeSignals(&signalState);
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMTickCount(TVMTickRef tickref) {
		TMachineSignalState signalState;
		MachineSuspendSignals(&signalState);
		if (tickref == NULL) {
			MachineResumeSignals(&signalState);
			return VM_STATUS_ERROR_INVALID_PARAMETER;
		}
		*tickref = totalTickCount;
		MachineResumeSignals(&signalState);
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority prio, TVMThreadIDRef tid) {
		TMachineSignalState signalState;
		MachineSuspendSignals(&signalState);
		if (entry == NULL || tid == NULL) {
			MachineResumeSignals(&signalState);
			return VM_STATUS_ERROR_INVALID_PARAMETER;
		}
		Thread *thread = new Thread();
		thread->state = VM_THREAD_STATE_DEAD;
		thread->entry = entry;
		thread->args = param;
		thread->memsize = memsize;
		thread->prio = prio;
		thread->stackaddr = malloc(thread->memsize * sizeof(TVMMemorySize));
		thread->id = threadList.size();
		*tid = thread->id;
		thread->sleepCountdown = 0;
		thread->mtxWaitTime = 0;
		threadList.push_back(*thread);
		MachineResumeSignals(&signalState);
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadDelete(TVMThreadID thread) {
		TMachineSignalState signalState;
		MachineSuspendSignals(&signalState);
		if (thread > threadList.size()-1 || thread < 0) {
			MachineResumeSignals(&signalState);
			return VM_STATUS_ERROR_INVALID_ID;
		}

		if (threadList[thread].state != VM_THREAD_STATE_DEAD) {
			MachineResumeSignals(&signalState);
			return VM_STATUS_ERROR_INVALID_STATE;
		}

		delete &threadList[thread];
		MachineResumeSignals(&signalState);

		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadActivate(TVMThreadID thread) {
		TMachineSignalState signalState;
		MachineSuspendSignals(&signalState);
		if (thread > threadList.size()-1 || thread < 0) {
			MachineResumeSignals(&signalState);
			return VM_STATUS_ERROR_INVALID_ID;
		}

		MachineContextCreate(&threadList[thread].cntx, &skeleton, threadList[thread].args,
			threadList[thread].stackaddr, threadList[thread].memsize);

		threadList[thread].state = VM_THREAD_STATE_READY;
		readyThreads[threadList[thread].prio].push(threadList[thread].id);
		if (threadList[thread].prio > threadList[currThread].prio) {
			threadList[currThread].state = VM_THREAD_STATE_READY;
			schedule(0);
		}
		MachineResumeSignals(&signalState);

		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadTerminate(TVMThreadID thread) {
		TMachineSignalState signalState;
		MachineSuspendSignals(&signalState);

		if (thread > threadList.size()-1 || thread < 0) {
			MachineResumeSignals(&signalState);
			return VM_STATUS_ERROR_INVALID_ID;
		}

		if (threadList[thread].state == VM_THREAD_STATE_DEAD) {
			MachineResumeSignals(&signalState);
			return VM_STATUS_ERROR_INVALID_STATE;
		}

		threadList[thread].state = VM_THREAD_STATE_DEAD;
		if (thread == currThread) { schedule(0); }

		MachineResumeSignals(&signalState);

		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadID(TVMThreadIDRef threadRef) {
		if (threadRef == NULL) {
			return VM_STATUS_ERROR_INVALID_PARAMETER;
		}

		*threadRef = currThread;
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadState(TVMThreadID thread, TVMThreadStateRef stateref) {
		if (stateref == NULL) {
			return VM_STATUS_ERROR_INVALID_PARAMETER;
		}

		if (thread > threadList.size()-1 || thread < 0) {
			return VM_STATUS_ERROR_INVALID_ID;
		}

		*stateref = threadList[thread].state;
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadSleep(TVMTick tick) {
		TMachineSignalState signalState;
		MachineSuspendSignals(&signalState);
		if (tick == VM_TIMEOUT_INFINITE) {
			MachineResumeSignals(&signalState);
			return VM_STATUS_ERROR_INVALID_PARAMETER;
		}

		if (tick == VM_TIMEOUT_IMMEDIATE) {
			threadList[currThread].state = VM_THREAD_STATE_READY;
			schedule(1);
		} else {
			threadList[currThread].state = VM_THREAD_STATE_WAITING;
			threadList[currThread].sleepCountdown = tick;
			sleepingThreads.push_back(threadList[currThread].id);
			schedule(0);
		}
		MachineResumeSignals(&signalState);

		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMFileOpen(const char* filename, int flags, int mode, int *fd) {
		TMachineSignalState signalState;
		MachineSuspendSignals(&signalState);
		if (fd == NULL || filename == NULL) {
			MachineResumeSignals(&signalState);
			return VM_STATUS_ERROR_INVALID_PARAMETER;}

		threadList[currThread].state = VM_THREAD_STATE_WAITING;

		callBackDataStorage *cb = new callBackDataStorage();
		cb->id = currThread;
		cb->resultPtr = fd;

		MachineFileOpen(filename, flags, mode, &fileCallBack, cb);
		schedule(0);

		if (*fd < 0) {
			MachineResumeSignals(&signalState);
			return VM_STATUS_FAILURE;
		}
		MachineResumeSignals(&signalState);

		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMFileClose(int fd) {
		TMachineSignalState signalState;
		MachineSuspendSignals(&signalState);
		threadList[currThread].state = VM_THREAD_STATE_WAITING;
		int result;
		callBackDataStorage *cb = new callBackDataStorage();
		cb->id = currThread;
		cb->resultPtr = (int*)&result;
		MachineFileClose(fd, &fileCallBack, cb);
		schedule(0);

		if (result < 0) {
			MachineResumeSignals(&signalState);
			return VM_STATUS_FAILURE;
		}
		MachineResumeSignals(&signalState);

		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMFileRead(int fd, void* data, int* length) {
		TMachineSignalState signalState;
		MachineSuspendSignals(&signalState);
		if (data == NULL || length == NULL) {
			MachineResumeSignals(&signalState);
			return VM_STATUS_ERROR_INVALID_PARAMETER;
		}

		threadList[currThread].state = VM_THREAD_STATE_WAITING;

		callBackDataStorage *cb = new callBackDataStorage();;
		cb->id = currThread;
		cb->resultPtr = length;

		MachineFileRead(fd, data, *length, &fileCallBack, cb);
		schedule(0);
		if (*length < 0) {
			MachineResumeSignals(&signalState);
			return VM_STATUS_FAILURE;
		}
		MachineResumeSignals(&signalState);

		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMFileWrite(int fd, void* data, int* length) {
		TMachineSignalState signalState;
		MachineSuspendSignals(&signalState);
		if (data == NULL || length == NULL) {
			MachineResumeSignals(&signalState);
			return VM_STATUS_ERROR_INVALID_PARAMETER;
		}

		threadList[currThread].state = VM_THREAD_STATE_WAITING;

		callBackDataStorage *cb = new callBackDataStorage();
		cb->id = currThread;
		cb->resultPtr = length;
		MachineFileWrite(fd, data, *length, &fileCallBack, cb);
		schedule(0);
		if (*length < 0) {
			MachineResumeSignals(&signalState);
			return VM_STATUS_FAILURE;
		}
		MachineResumeSignals(&signalState);

		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMFileSeek(int fd, int offset, int whence, int* newoffset) {
		TMachineSignalState signalState;
		MachineSuspendSignals(&signalState);
		int placeHolder = 0;
		int* tempPointer = &placeHolder;

		threadList[currThread].state = VM_THREAD_STATE_WAITING;

		callBackDataStorage *cb = new callBackDataStorage();
		cb->id = currThread;
		cb->resultPtr = tempPointer;

		MachineFileSeek(fd, offset, whence, &fileCallBack, cb);
		schedule(0);

		if (newoffset != NULL) {
			*newoffset = *tempPointer;
			MachineResumeSignals(&signalState);
			if (*newoffset < 0) {return VM_STATUS_FAILURE;}
		}
		else {
			MachineResumeSignals(&signalState);
			if (*tempPointer < 0) {return VM_STATUS_FAILURE;}
		}

		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMMutexCreate(TVMMutexIDRef mutexref) {
		TMachineSignalState signalState;
		MachineSuspendSignals(&signalState);
		if (mutexref == NULL) {
			MachineResumeSignals(&signalState);
			return VM_STATUS_ERROR_INVALID_PARAMETER;
		}

		Mutex* mtx = new Mutex();

		mtx->isLocked = false;
		mtx->waitingQ.resize(3);
		mtx->mtxId = mutexList.size();
		mtx->owner = VM_THRAED_ID_INVALID;
		mutexList.push_back(*mtx);

		*mutexref = mtx->mtxId;

		MachineResumeSignals(&signalState);
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMMutexDelete(TVMMutexID mutex) {
		TMachineSignalState signalState;
		MachineSuspendSignals(&signalState);
		if (mutex < 0 || mutex > mutexList.size()) {
			MachineResumeSignals(&signalState);
			return  VM_STATUS_ERROR_INVALID_ID;
		}

		if (mutexList[mutex].isLocked) {
			MachineResumeSignals(&signalState);
			return VM_STATUS_ERROR_INVALID_STATE;
		}

		delete &mutexList[mutex];
		MachineResumeSignals(&signalState);
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMMutexQuery(TVMMutexID mutex, TVMThreadIDRef ownerref) {
		TMachineSignalState signalState;
		MachineSuspendSignals(&signalState);

		if (mutex < 0 || mutex >= mutexList.size()) {
			MachineResumeSignals(&signalState);
			return VM_STATUS_ERROR_INVALID_ID;
		}

		if (ownerref == NULL) {
			MachineResumeSignals(&signalState);
			return VM_STATUS_ERROR_INVALID_PARAMETER;
		}

		if (mutexList[mutex].isLocked) {
			*ownerref = mutexList[mutex].owner;
		} else {
			*ownerref = VM_THREAD_ID_INVALID;
		}
		MachineResumeSignals(&signalState);
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMMutexAcquire(TVMMutexID mutex, TVMTick timeout) {
		TMachineSignalState signalState;
		MachineSuspendSignals(&signalState);

		if (mutex < 0 || mutex >= mutexList.size()) {
			MachineResumeSignals(&signalState);
			return VM_STATUS_ERROR_INVALID_ID;
		}

		// timeout immediate logic
		if (timeout == VM_TIMEOUT_IMMEDIATE) {
			if (mutexList[mutex].isLocked) {
				MachineResumeSignals(&signalState);
				return VM_STATUS_FAILURE;
			} else {
				mutexList[mutex].isLocked = true;
				mutexList[mutex].owner = currThread;
				MachineResumeSignals(&signalState);
				return VM_STATUS_SUCCESS;
			}
		}

		if (mutexList[mutex].isLocked) {
			if (timeout == VM_TIMEOUT_INFINITE) { threadList[currThread].mtxWaitTime = -1; }
			else { threadList[currThread].mtxWaitTime = timeout; }
			mutexList[mutex].waitingQ[threadList[currThread].prio-1].push((TVMThreadID)currThread);
			threadList[currThread].state = VM_THREAD_STATE_WAITING;
			schedule(0);
		} else {
			mutexList[mutex].isLocked = true;
			mutexList[mutex].owner = currThread;
		}
		MachineResumeSignals(&signalState);
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMMutexRelease(TVMMutexID mutex) {
		return VM_STATUS_SUCCESS;
	}


}
