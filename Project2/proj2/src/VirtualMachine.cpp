#include "VirtualMachine.h"
#include "Machine.h"
#include <iostream>

extern "C" {
	// Stuff for functions in headers
	#define VM_TIMEOUT_INFINITE ((TVMTick)0)
	#define VM_TIMEOUT_IMMEDIATE ((TVMTick)-1)
	typedef void (*TVMMainEntry) (int, char* []);
	typedef void (*TMachineAlarmCallback) (void* calldata);
	TVMMainEntry VMLoadModule(const char* module);
	void VMUnloadModule(void);
	void timerCallback(void*);

	// Store the tickms arg that was passed when starting the program
	int tickTimeMSArg;
	// tickCount stores the number of ticks since start
	volatile TVMTick totalTickCount = 0;

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
		if (tickmsref == NULL) {return VM_STATUS_ERROR_INVALID_PARAMETER;}
		*tickmsref = tickTimeMSArg;
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMTickCount(TVMTickRef tickref) {
		if (tickref == NULL) {return VM_STATUS_ERROR_INVALID_PARAMETER;}
		*tickref = totalTickCount;
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize,
							 TVMThreadPriority prio, TVMThreadIDRef tid) {
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadDelete(TVMThreadID thread) {
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadActivate(TVMThreadID thread) {
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadTerminate(TVMThreadID thread) {
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadID(TVMThreadIDRef thread) {
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadState(TVMThreadID thread, TVMThreadStateRef stateref) {
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMThreadSleep(TVMTick tick) {
		if (tick == VM_TIMEOUT_INFINITE) {return VM_STATUS_ERROR_INVALID_PARAMETER;}

		TVMTick stopUntil = totalTickCount + tick;
		while(totalTickCount < stopUntil) {
		}

		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMFileOpen(const char* filename, int flags, int mode, int *filedescriptor) {
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMFileClose(int filedescriptor) {
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMFileRead(int filedescriptor, void* data, int* length) {
		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMFileWrite(int filedescriptor, void* data, int* length) {
		if (data == NULL || length == NULL) {
			return VM_STATUS_ERROR_INVALID_PARAMETER;
		}

		if (write(filedescriptor, data, *length) != *length) {
			return VM_STATUS_FAILURE;
		}

		return VM_STATUS_SUCCESS;
	}

	TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int* newoffset) {
		return VM_STATUS_SUCCESS;
	}


}
