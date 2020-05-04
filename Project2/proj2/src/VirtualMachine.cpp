#include "VirtualMachine.h"
#include "Machine.h"
#include <iostream>
#include <vector>
#include <queue>

extern "C" {
	// Stuff for functions in headers
	#define VM_TIMEOUT_INFINITE						((TVMTick)0)
	#define VM_TIMEOUT_IMMEDIATE					((TVMTick)-1)

	#define VM_THREAD_STATE_DEAD                    ((TVMThreadState)0x00)
	#define VM_THREAD_STATE_RUNNING                 ((TVMThreadState)0x01)
	#define VM_THREAD_STATE_READY                   ((TVMThreadState)0x02)
	#define VM_THREAD_STATE_WAITING                 ((TVMThreadState)0x03)

	#define VM_THREAD_PRIORITY_LOW                  ((TVMThreadPriority)0x01)
	#define VM_THREAD_PRIORITY_NORMAL               ((TVMThreadPriority)0x02)
	#define VM_THREAD_PRIORITY_HIGH                 ((TVMThreadPriority)0x03)

	typedef void (*TVMMainEntry) (int, char* []);
	typedef void (*TMachineAlarmCallback) (void* calldata);
	typedef void (*TVMThreadEntry)(void*);
	typedef void (*TMachineFileCallback)(void *calldata, int result);

	TVMMainEntry VMLoadModule(const char* module);
	void VMUnloadModule(void);
	void timerCallback(void*);

	// Store the tickms arg that was passed when starting the program
	int tickTimeMSArg;
	// tickCount stores the number of ticks since start
	volatile TVMTick totalTickCount = 0;


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
	};

	volatile TVMThreadID currThread = 1;

	std::vector<Thread> threadHolder;
	// 0 = LOW, 1 = NORMAL, 2 = HIGH
	std::vector<std::queue<unsigned int>> readyThreads;
	std::vector<unsigned int> sleepingThreads;

	std::vector<unsigned int> oldStates;

	void dispatch(TVMThreadID next) {
		TVMThreadID prev = currThread;
		currThread = next;

		// std::cout << "Going from " << prev << " to " << next << std::endl;
		threadHolder[currThread].state = VM_THREAD_STATE_RUNNING;

		MachineContextSwitch((SMachineContextRef)&threadHolder[prev].cntx, (SMachineContextRef)&threadHolder[currThread].cntx);

	}

	void schedule(int scheduleEqualPrio) {
		if (oldStates.size() < threadHolder.size()) {
			int oldSize = oldStates.size();
			// make vector longer to equal size of num threads
			oldStates.resize(threadHolder.size());
			for (unsigned int i = oldSize; i < threadHolder.size(); i++) {
				oldStates[i] = threadHolder[i].state;
				if (threadHolder[i].state == VM_THREAD_STATE_READY) {
					 readyThreads[threadHolder[i].prio-1].push(threadHolder[i].id);
				}
			}
		}
		for (unsigned int i = 0; i < threadHolder.size(); i++) {
			if ((oldStates[i] != VM_THREAD_STATE_READY) && (threadHolder[i].state == VM_THREAD_STATE_READY)) {
				readyThreads[threadHolder[i].prio-1].push(threadHolder[i].id);
			}
			oldStates[i] = threadHolder[i].state;
		}


		// for (unsigned int i = 0; i < readyThreads.size(); i++) {
		// 	switch(i) {
		// 		case 0:	std::cout << "LOW THREADS: " << std::endl;
		// 		break;
		// 		case 1: std::cout << "NORMAL THREADS: " << std::endl;
		// 		break;
		// 		case 2: std::cout << "HIGH THREADS: " << std::endl;
		// 		break;
		// 		default: break;
		// 	}
		// 	for (unsigned int j = 0; j < readyThreads[i].size(); j++) {
		// 		std::cout << "thread ID: " << readyThreads[i].front() << std::endl;
		// 		readyThreads[i].push(readyThreads[i].front());
		// 		readyThreads[i].pop();
		// 	}
		// }
		TVMThreadID nextThread;

		if (scheduleEqualPrio == 1) {
			if (readyThreads[threadHolder[currThread].prio-1].size() != 0) {
				nextThread = readyThreads[threadHolder[currThread].prio-1].front();
				readyThreads[threadHolder[currThread].prio-1].pop();
				dispatch(nextThread);
				return;
			}
			else {
				return;
			}
		}

		if (readyThreads[2].size() == 0 && readyThreads[1].size() == 0) {
			nextThread = readyThreads[0].front();
			readyThreads[0].pop();
		} else if (readyThreads[2].size() == 0 && readyThreads[1].size() != 0) {
			nextThread = readyThreads[1].front();
			readyThreads[1].pop();
		} else {
			nextThread = readyThreads[2].front();
			readyThreads[2].pop();
		}


		dispatch(nextThread);
	}

	void skeleton(void* param) {
		// std::cout << "in skeleton with id: " << currThread << std::endl;
		threadHolder[currThread].entry(threadHolder[currThread].args);
		VMThreadTerminate(currThread);
	}

	void idle(void* param) {
		// std::cout << "In IDLE" << std::endl;
		while(true) {
		}
	}

	TVMStatus VMStart(int tickms, int argc, char* argv[]) {
		readyThreads.resize(3);
		TVMMainEntry VMMain = VMLoadModule(argv[0]);
		if (VMMain == NULL) {return VM_STATUS_FAILURE;}
		tickTimeMSArg = tickms;
		MachineInitialize();
		MachineEnableSignals();
		// create the idle and main thread;
		TVMThreadID idleID, mainID;
		VMThreadCreate(idle, NULL, 0x100000, VM_THREAD_PRIORITY_LOW, &idleID);
		VMThreadCreate((TVMThreadEntry)VMMain, argv, 0x100000, VM_THREAD_PRIORITY_NORMAL, &mainID);
		threadHolder[idleID].state = VM_THREAD_STATE_READY;
		readyThreads[threadHolder[idleID].prio-1].push(threadHolder[idleID].id);
		threadHolder[mainID].state = VM_THREAD_STATE_RUNNING;
		MachineContextCreate((SMachineContextRef)&threadHolder[idleID].cntx, &skeleton, threadHolder[idleID].args, threadHolder[idleID].stackaddr, threadHolder[idleID].memsize);

		// Store the old states. Will be used to determine if a thread needs to be push to readyQueue
		oldStates.resize(2);
		oldStates[idleID] = threadHolder[idleID].state;
		oldStates[mainID] = threadHolder[mainID].state;

		// create alarm for tick incrementing
		useconds_t tickus = tickms * 1000;
		MachineRequestAlarm(tickus, timerCallback, NULL);

		VMMain(argc, argv);
		MachineTerminate();
		VMUnloadModule();
		return VM_STATUS_SUCCESS;
	}

	void timerCallback(void* calldata) {
		totalTickCount++;
		for (unsigned int i = 0; i < sleepingThreads.size(); i++) {
			if (threadHolder[sleepingThreads[i]].sleepCountdown == 0) {
				threadHolder[sleepingThreads[i]].state = VM_THREAD_STATE_READY;
				TVMThreadID prv = currThread;
				currThread = sleepingThreads[i];
				threadHolder[prv].state = VM_THREAD_STATE_READY;
				threadHolder[currThread].state = VM_THREAD_STATE_RUNNING;
				sleepingThreads.erase(sleepingThreads.begin()+i);
				MachineContextSwitch((SMachineContextRef)&threadHolder[prv].cntx, (SMachineContextRef)&threadHolder[currThread].cntx);
			} else {
				threadHolder[sleepingThreads[i]].sleepCountdown -= 1;
			}
		}
	}

	TVMStatus VMTickMS(int *tickmsref) {
		/* Retrieves milliseconds between ticks of VM
			Params:
				tickmsref = location to put tick time interval in ms
			Returns:
				VM_STATUS_SUCCESS on successful retireval
				VM_STATUS_ERROR_INVALID_PARAMETER when tickmsref = NULL
		*/
		if (tickmsref == NULL) {return VM_STATUS_ERROR_INVALID_PARAMETER;}
		*tickmsref = tickTimeMSArg;
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMTickCount(TVMTickRef tickref) {
		/* Retrieves number of ticks that have occurred since start of VM
			Params:
				tickref = location to put ticks
			Returns:
				VM_STATUS_SUCCESS on success
				VM_STATUS_ERROR_INVALID_PARAMETER if tickref = NULL
		*/
		if (tickref == NULL) {return VM_STATUS_ERROR_INVALID_PARAMETER;}
		*tickref = totalTickCount;
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority prio, TVMThreadIDRef tid) {
		/* Create a thread in VM
			Params:
				Thread is created in dead state
				entry = function of the thread
				param = parameter of function
				memsize = memory size
				prio = priority (1 = LOW, 2 = NORMAL, 3 = HIGH) (1 = IDLE)
				tid = thread identifier (ID)
		  	Returns:
		    	VM_STATUS_SUCCESS on successful creation
		    	VM_STATUS_ERROR_INVALID_PARAMETER on entry == NULL or tid == NULL
		*/
		if (entry == NULL || tid == NULL) {return VM_STATUS_ERROR_INVALID_PARAMETER;}

		Thread *thread = new Thread();
		thread->state = VM_THREAD_STATE_DEAD;
		thread->entry = entry;
		thread->args = param;
		thread->memsize = memsize;
		thread->prio = prio;
		thread->stackaddr = malloc(thread->memsize * sizeof(TVMMemorySize));
		thread->id = threadHolder.size();
		*tid = thread->id;
		thread->sleepCountdown = 0;
		threadHolder.push_back(*thread);

		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadDelete(TVMThreadID thread) {
		/* Deletes a DEAD thread
			Params:
				thread = thread ID
			Returns:
				VM_STATUS_SUCCESS on successful deletion
				VM_STATUS_ERROR_INVALID_ID on NULL thread param
				VM_STATUS_ERROR_INVALID_STATE on non-dead thread
		*/
		if (thread > threadHolder.size()-1 || thread < 0) {return VM_STATUS_ERROR_INVALID_ID;}
		if (threadHolder[thread].state != VM_THREAD_STATE_DEAD) {return VM_STATUS_ERROR_INVALID_STATE;}

		delete &threadHolder[thread];
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadActivate(TVMThreadID thread) {
		/* Activate DEAD thread
			Params:
				thread = dead thread ID
			Returns:
				VM_STATUS_SUCCESS on successful activation
				VM_STATUS_ERROR_INVALID_ID on NULL thread
				VM_STATUS_ERROR_INVALID_STATE on valid thread but not dead
		*/
		if (thread > threadHolder.size()-1 || thread < 0) {return VM_STATUS_ERROR_INVALID_ID;}

		threadHolder[thread].state = VM_THREAD_STATE_READY;

		MachineContextCreate((SMachineContextRef)&threadHolder[thread].cntx, &skeleton, threadHolder[thread].args, threadHolder[thread].stackaddr, threadHolder[thread].memsize);

		if (threadHolder[thread].prio > threadHolder[currThread].prio) {
			schedule(0);
		}

		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadTerminate(TVMThreadID thread) {
		/* Terminate thread
			Params:
				thread = thread ID
			Returns:
				VM_STATUS_SUCCESS on successful termination
				VM_STATUS_ERROR_INVALID_ID on NULL thread
				VM_STATUS_ERROR_INVALID_STATE on valid dead thread
		*/
		if (thread > threadHolder.size()-1 || thread < 0) {return VM_STATUS_ERROR_INVALID_ID;}
		if (threadHolder[thread].state == VM_THREAD_STATE_DEAD) {return VM_STATUS_ERROR_INVALID_STATE;}
		threadHolder[thread].state = VM_THREAD_STATE_DEAD;

		schedule(0);

		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadID(TVMThreadIDRef threadRef) {
		/* Retrieve thread identifier of current operating thread
			Params:
				threadref = location to put thread ID
			Returns:
				VM_STATUS_SUCCESS on successful retrieval
				VM_STATUS_ERROR_INVALID_PARAMETER if threadref = NULL
		*/
		if (threadRef == NULL) {return VM_STATUS_ERROR_INVALID_PARAMETER;}
		*threadRef = currThread;
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadState(TVMThreadID thread, TVMThreadStateRef stateref) {
		/* Retrieves state of a thread in VM
			Params:
				thread = thread ID
				stateref = place to put thread state
			Returns:
				VM_STATUS_SUCCESS on successsful retrieval of state
				VM_STATUS_ERROR_INVALID_ID if thread does not exist
				VM_STATUS_ERROR_INVALID_PARAMETER on stateref = NULL
		*/
		if (stateref == NULL) {return VM_STATUS_ERROR_INVALID_PARAMETER;}

		if (thread > threadHolder.size()-1 || thread < 0) {return VM_STATUS_ERROR_INVALID_ID;}

		*stateref = threadHolder[thread].state;
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadSleep(TVMTick tick) {
		/* Put current thread in VM to sleep
			Params:
				tick = num of ticks to sleep for
					If tick = VM_TIMEOUT_IMMEDIATE
						current process yields remainder of proccessing quantum
						to next ready process of equal prio
			Returns:
				VM_STATUS_SUCCESS on successful sleep
				VM_STATUS_ERROR_INVALID_PARAMETER if tick = VM_TIMEOUT_INFINITE
		*/

		if (tick == VM_TIMEOUT_INFINITE) {return VM_STATUS_ERROR_INVALID_PARAMETER;}

		if (tick == VM_TIMEOUT_IMMEDIATE) {
			schedule(1);
		} else {
			threadHolder[currThread].state = VM_THREAD_STATE_WAITING;
			threadHolder[currThread].sleepCountdown = tick;
			sleepingThreads.push_back(threadHolder[currThread].id);
			schedule(0);
		}

		return VM_STATUS_SUCCESS;
	}

	void fileOpenCallBack(void *calldata, int result) {
		std::vector<int*> = (std::vector<int*>)calldata;
		std::cout << "First Arg: " << *(args[0]) << std::endl;
		std::cout << "Second Arg: " << *(args[1]) << std::endl;
		std::cout << "Third Arg: " << result << std::endl;
		schedule(0);
	}
	TVMStatus VMFileOpen(const char* filename, int flags, int mode, int *fd) {
		/* Open and possibly creates file in file system.
		   Thread is in VM_THREAD_STATE_WAITING until success or failure of FileOpen
			Params:
				filename = file to open
				flags = flags to use
				mode = mode to use
				fd = file descriptor to use
			Returns:
				VM_STATUS_SUCCESS on successful open
				VM_STATUS_FAILURE on failure
				VM_STATUS_ERROR_INVALID_PARAMETER if fd or filename are NULL
		*/
		if (fd == NULL || filename == NULL) {return VM_STATUS_ERROR_INVALID_PARAMETER;}
		threadHolder[currThread].state = VM_THREAD_STATE_WAITING;
		*fd =12;
		std::vector<int*> callData;
		callData.push_back((int*)&currThread);
		callData.push_back(fd);
		MachineFileOpen(filename, flags, mode, &fileOpenCallBack, callData);
		std::cout << "after" << std::endl;
		schedule(0);
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMFileClose(int fd) {
		/* Closes a file. VM_THREAD_STATE_WAITING until successful/unsuccessful close
			Params:
				fd = file descriptor
			Returns:
				VM_STATUS_SUCCESS on successful close
				VM_STATUS_FAILURE on failure
		*/
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMFileRead(int fd, void* data, int* length) {
		/* Read file. Thread is VM_THREAD_STATE_WAITING until success or failure
			Params:
				fd = file descriptor
				data = location where file data is stored
				length = number of bytes
			Returns:
				VM_STATUS_SUCCESS on success
				VM_STATUS_FAILURE on failure
				VM_STATUS_ERROR_INVALID_PARAMETER if data or length are NULL
		*/
		if (data == NULL || length == NULL) {return VM_STATUS_ERROR_INVALID_PARAMETER;}
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMFileWrite(int fd, void* data, int* length) {
		/* Write to file. Thread is VM_THREAD_STATE_WAITING until success or failure
			Params:
				fd = file descriptor
				data = location where file data is stored
				length = number of bytes
			Returns:
				VM_STATUS_SUCCESS on success
				VM_STATUS_FAILURE on failure
				VM_STATUS_ERROR_INVALID_PARAMETER if data or length are NULL
		*/
		if (data == NULL || length == NULL) {return VM_STATUS_ERROR_INVALID_PARAMETER;}

		if (write(fd, data, *length) != *length) {
			return VM_STATUS_FAILURE;
		}

		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int* newoffset) {
		/* Seeks within a file. VM_THREAD_STATE_WAITING until success or failure
			Params:
				fd = file descriptor, obtained by prev call to VMFileOpen()
				offset = numbers of bytes to seek
				whence = location to begin seeking
				newoffset = the location to place the new offset if not NULL
			Returns:
				VM_STATUS_SUCCESS on success
				VM_STATUS_FAILURE on failure
		*/
		return VM_STATUS_SUCCESS;
	}


}
