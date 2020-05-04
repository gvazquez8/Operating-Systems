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

	typedef void (*TVMMainEntry) (int, char* []);
	typedef void (*TMachineAlarmCallback) (void* calldata);
	typedef void (*TVMThreadEntry)(void*);

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
	};

	Thread *currThread = NULL;

	std::vector<std::queue<TVMThreadIDRef>> threadHolder;
	threadHolder.resize(3);

	void skeleton(void* param) {
		Thread* thread = (Thread*) param;
		thread->entry(thread->args);
		VMThreadTerminate(thread->id);
	}

	TVMStatus VMStart(int tickms, int argc, char* argv[]) {
		TVMMainEntry VMMain = VMLoadModule(argv[0]);
		if (VMMain == NULL) {return VM_STATUS_FAILURE;}
		tickTimeMSArg = tickms;
		MachineInitialize();
		MachineEnableSignals();

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

	TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize,
							 TVMThreadPriority prio, TVMThreadIDRef tid) {
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

		Thread thread = new Thread();
		thread.state = VM_THREAD_STATE_DEAD;
		thread.entry = entry;
		thread.args = param;
		thread.memsize = memsize;
		thread.prio = prio;
		thread.stackaddr = malloc(thread.memsize * sizeof(TVMMemorySize));

		threadHolder[thread.prio - 1].push(&thread);



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
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadID(TVMThreadIDRef thread) {
		/* Retrieve thread identifier of current operating thread
			Params:
				threadref = location to put thread ID
			Returns:
				VM_STATUS_SUCCESS on successful retrieval
				VM_STATUS_ERROR_INVALID_PARAMETER if threadref = NULL
		*/
		if (thread == NULL) {return VM_STATUS_ERROR_INVALID_PARAMETER;}
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

		TVMTick currentTickCount = 0;
		TVMTick stopUntil = 0;
		TVMTickRef totalTickCountRef = &currentTickCount;
		TVMTickRef stopUntilRef = &stopUntil;

		*stopUntilRef = *totalTickCountRef + tick;
		while(*totalTickCountRef < *stopUntilRef) {
			VMTickCount(totalTickCountRef);
		}

		return VM_STATUS_SUCCESS;
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
				VM_STATUS_FAILURE on filaure
				VM_STATUS_ERROR_INVALID_PARAMETER if fd or filename are NULL
		*/
		if (fd == NULL || filename == NULL) {return VM_STATUS_ERROR_INVALID_PARAMETER;}
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
