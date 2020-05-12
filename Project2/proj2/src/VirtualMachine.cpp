#include "VirtualMachine.h"
#include "Machine.h"
#include <iostream>
#include <vector>
#include <queue>

extern "C" {
        // Stuff for functions in headers
        #define VM_TIMEOUT_INFINITE						((TVMTick)0)
        #define VM_TIMEOUT_IMMEDIATE                    ((TVMTick)-1)

        #define VM_THREAD_STATE_DEAD                    ((TVMThreadState)0x00)
        #define VM_THREAD_STATE_RUNNING                 ((TVMThreadState)0x01)
        #define VM_THREAD_STATE_READY                   ((TVMThreadState)0x02)
        #define VM_THREAD_STATE_WAITING                 ((TVMThreadState)0x03)

		#define VM_THREAD_PRIORITY_NONE					((TVMThreadPriority)0x00)
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
        // Signal State
        TMachineSignalState signalState;

        // Struct to hold callback information when using VMFile* functions
        struct callBackDataStorage {
            TVMThreadID id;
            int* resultPtr;
        };

        // Class holding thread info (TCB)
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

        std::vector<Thread> threadList;
        std::vector<std::queue<unsigned int>> readyThreads;
        std::vector<unsigned int> sleepingThreads;

       void dispatch(TVMThreadID next) {

            TVMThreadID prev = currThread;
            currThread = next;

            // std::cout << "Going from " << prev << " to " << next << std::endl;

            if (threadList[prev].state == VM_THREAD_STATE_READY) {
                readyThreads[threadList[prev].prio].push(threadList[prev].id);
            }
 
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
                    return;
                }
                else {
                    return;
                }
            }

            if (readyThreads[1].empty() && readyThreads[2].empty() && readyThreads[3].empty()) {
                nextThread = readyThreads[0].front();
                readyThreads[0].pop();
            } else if (!readyThreads[3].empty()) {
                nextThread = readyThreads[3].front();
                readyThreads[3].pop();
            } else if (!readyThreads[2].empty()) {
                nextThread = readyThreads[2].front();
                readyThreads[2].pop();
            } else {
                nextThread = readyThreads[1].front();
                readyThreads[1].pop();
            }
            dispatch(nextThread);
        }

        void fileCallBack(void *calldata, int result) {
        	MachineSuspendSignals(&signalState);
            callBackDataStorage *args = (callBackDataStorage*) calldata;
            *(args->resultPtr) = result;
            if (threadList[args->id].state == VM_THREAD_STATE_DEAD) {
                return;
            } else {
                threadList[args->id].state = VM_THREAD_STATE_READY;
                readyThreads[threadList[args->id].prio].push(args->id);
                if (threadList[args->id].prio > threadList[currThread].prio) {
	                threadList[currThread].state = VM_THREAD_STATE_READY;
		            // std::cout << "Scheduling through fileCallBack\n";
    	            schedule(0);
                }
            }
            MachineResumeSignals(&signalState);
        }

        void tickCallBack(void* calldata) {
            MachineSuspendSignals(&signalState);
            totalTickCount++;
            for (unsigned int i = 0; i < sleepingThreads.size(); i++) {
                if (threadList[sleepingThreads[i]].sleepCountdown == 0) {
                    threadList[sleepingThreads[i]].state = VM_THREAD_STATE_READY;
                    readyThreads[threadList[sleepingThreads[i]].prio].push(threadList[sleepingThreads[i]].id);
                    sleepingThreads.erase(sleepingThreads.begin()+i);
                    i--;
                } else {
                    threadList[sleepingThreads[i]].sleepCountdown -= 1;
                }
            }
            if (threadList[currThread].state != VM_THREAD_STATE_DEAD) {
                threadList[currThread].state = VM_THREAD_STATE_READY;
            }
            // std::cout << "Scheduling through tickCallBack\n";
            schedule(0);
            MachineResumeSignals(&signalState);
        }

        void skeleton(void* param) {
            MachineEnableSignals();
            threadList[currThread].entry(threadList[currThread].args);
            VMThreadTerminate(currThread);
        }

        void idleFunction(void* param) {
            MachineResumeSignals(&signalState);
            while(true) {}
        }

        void VMCreateIdleThread() {
        	// Creates the idle thread and push to thread list
            Thread *idleThread = new Thread();
            idleThread->state = VM_THREAD_STATE_READY;
            idleThread->entry = &idleFunction;
            idleThread->args = NULL;
            idleThread->prio = VM_THREAD_PRIORITY_NONE;
            idleThread->id = threadList.size();
            idleThread->sleepCountdown = 0;
            idleThread->memsize = 0x100000;
            idleThread->stackaddr = malloc(idleThread->memsize * sizeof(TVMMemorySize));
            threadList.push_back(*idleThread);
            MachineContextCreate(&threadList[0].cntx, &skeleton, threadList[0].args,
            	threadList[0].stackaddr, threadList[0].memsize);
            return;
        }

        void VMCreateMainThread(TVMMainEntry VMMain, char* argv[]) {
        	// Create the main thread and push to thread list
            Thread *mainThread = new Thread();
            mainThread->state = VM_THREAD_STATE_RUNNING;
            mainThread->entry = (TVMThreadEntry) VMMain;
            mainThread->args = argv;
            mainThread->prio = VM_THREAD_PRIORITY_NORMAL;
            mainThread->id = threadList.size();
            mainThread->sleepCountdown = 0;
            threadList.push_back(*mainThread);
        }

        TVMStatus VMStart(int tickms, int argc, char* argv[]) {
        	// Create the ready queues
  			// Index 0: NONE priority queue (ONLY IDLE THREAD)
        	// Index 1: LOW priority queue
        	// Index 2: NORMAL priority queue
        	// Index 3: HIGH priority queue
            readyThreads.resize(4);
            
            // Load the Main Function
            TVMMainEntry VMMain = VMLoadModule(argv[0]);
            // If the function doesn't exist then return a failure
            if (VMMain == NULL) {return VM_STATUS_FAILURE;}
            
            // Set the global variable tickTime
            tickTime = tickms+1;

            // Init the Machine
            MachineInitialize();
            // Enable Interrupts
            MachineEnableSignals();

            VMCreateIdleThread();
            VMCreateMainThread(VMMain, argv);

            // Set up timer
            useconds_t tickus = tickTime * 1000;
            MachineRequestAlarm(tickus, tickCallBack, NULL);
            
            VMMain(argc, argv);
            
            MachineTerminate();
            VMUnloadModule();
            
            return VM_STATUS_SUCCESS;
        }

        TVMStatus VMTickMS(int *tickmsref) {
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
            MachineSuspendSignals(&signalState);
            if (tickref == NULL) {
                MachineResumeSignals(&signalState);
                return VM_STATUS_ERROR_INVALID_PARAMETER;
            }
            *tickref = totalTickCount;
            MachineResumeSignals(&signalState);
            return VM_STATUS_SUCCESS;
        }

        TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize,
        	TVMThreadPriority prio, TVMThreadIDRef tid) {

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
            threadList.push_back(*thread);
            MachineResumeSignals(&signalState);
            return VM_STATUS_SUCCESS;
        }

        TVMStatus VMThreadDelete(TVMThreadID thread) {
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
                schedule(0);
            }
            MachineResumeSignals(&signalState);

            return VM_STATUS_SUCCESS;
        }

        TVMStatus VMThreadTerminate(TVMThreadID thread) {
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
            if (thread == currThread) {
                schedule(0);
            }

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
            MachineSuspendSignals(&signalState);
            if (tick == VM_TIMEOUT_INFINITE) {
                MachineResumeSignals(&signalState);
                return VM_STATUS_ERROR_INVALID_PARAMETER;
            }

            if (tick == VM_TIMEOUT_IMMEDIATE) {
                threadList[currThread].state = VM_THREAD_STATE_READY;
                readyThreads[threadList[currThread].prio].push(threadList[currThread].id);
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
            MachineSuspendSignals(&signalState);
            if (data == NULL || length == NULL) {
                MachineResumeSignals(&signalState);
                return VM_STATUS_ERROR_INVALID_PARAMETER;
            }

            threadList[currThread].state = VM_THREAD_STATE_WAITING;

			callBackDataStorage *cb = new callBackDataStorage();
			cb->id = currThread;
			cb->resultPtr = length;
			// std::cout << "Writing for thread: " << currThread << " now\n";
        	// std::cout << "---------------\n";
         //    for (unsigned int i = 1; i < readyThreads.size()-2; i++) {
         //    	switch(i) {
         //    		case 1:
         //    			std::cout << "LOW: ";	
         //    			break;
         //    		default:
         //    			break;
         //    	}

         //    	for (unsigned int j = 0; j < readyThreads[i].size(); j++) {
         //    		TVMThreadID tid = readyThreads[i].front();
         //    		readyThreads[i].pop();
         //    		std::cout << tid << " ";
         //    		readyThreads[i].push(tid);
         //    	}
         //    	std::cout << std::endl;
         //    }
			MachineFileWrite(fd, data, *length, &fileCallBack, cb);
            // std::cout << "Scheduling through VMFileWrite\n";
            schedule(0);
			// std::cout << "Writing for thread: " << currThread << " finished\n";
            if (*length < 0) {
                MachineResumeSignals(&signalState);
                return VM_STATUS_FAILURE;
            }
            MachineResumeSignals(&signalState);

            return VM_STATUS_SUCCESS;
        }

        TVMStatus VMFileSeek(int fd, int offset, int whence, int* newoffset) {
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


}
