// To compile on DICE use: /usr/lib64/openmpi/bin/mpicc -o partA aquadPartA.c stack.h stack.c
// and then run with: /usr/lib64/openmpi/bin/mpirun -c 5 ./partA

/*
Implementation strategy
======================
My strategy was to make the interface between the workers and the farmer as simple as possible, to simplify the problem conceptually.
The messages between the farmer and the workers always contain an array of two doubles, which mean, specifically:
  - the boundaries of the integral to evaluate (when giving a task to a worker)
  - the boundaries that must be divided into two new tasks (when a worker returns new tasks)
  - the estimated larea and rarea that must be accumulated to the total area (when a worker returns the area)
  - they are ignored for the worker termination message.
The simple interface does cost some extra computation (such as recomputing the mid, and the function values), but it is insignificant
to the overall running time.

I used MPI tags when sending messages to make clear the messages' intent.  I chose this instead of checking the shape of the received
data, or just assuming what the data is from the context of the function call.  Using tags also helped with debugging as I could print
them and see what was going on in more detail.

To keep track of which workers are available, I use an array with an entry for each worker.  Whenever a worker is given a task, or
completes one, its entry is updated in this array.  When sending out new tasks, a simple linear search is done to find the first idle
worker.  If there were many thousands of workers it would be worth implementing a more efficient sort like merge sort, but I didn't
think it was necessary.  The need to keep track of which workers are busy could be avoided entirely if the farmer could send a
message that goes only to the first worker who calls receive.  I couldn't find an MPI primitive for that so I did it manually.

I used the standard MPI send and receive modes.  These are generally asynchronous, which is good because there is never any need for
the sender to wait for the receive to happen before moving on.  If I used synchronous sends that would waste time.  I considered using
MPI_Isend, so that execution could continue immediately, without waiting for the send buffer to clear, but I didn't see much point
because I generally reuse the buffer immediately afterwards, and rather than complicate the code with tests for send completion,
or else constantly proliferate the number of buffers, I stuck with simplicity.

One other thing worth noting is that the farmer completely depletes the supply of available workers or available tasks between every
call of MPI_Recieve.  This is done because I assume that sending tasks takes a shorter time than waiting for work to be done, and
it minimises the chance of workers sitting unnecessarily idle.
*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <mpi.h>
#include "stack.h"

#define EPSILON 1e-3
#define F(arg)  cosh(arg)*cosh(arg)*cosh(arg)*cosh(arg)
#define A 0.0
#define B 5.0

#define SLEEPTIME 1
#define NO_MORE_WORK 1000
#define NEW_TASKS 2000
#define RESULT 3000

int *tasks_per_process;

double farmer(int);

void worker(int);

int main(int argc, char **argv ) {
  int i, myid, numprocs;
  double area, a, b;

  MPI_Init(&argc, &argv);
  MPI_Comm_size(MPI_COMM_WORLD,&numprocs);
  MPI_Comm_rank(MPI_COMM_WORLD,&myid);

  if(numprocs < 2) {
    fprintf(stderr, "ERROR: Must have at least 2 processes to run\n");
    MPI_Finalize();
    exit(1);
  }

  if (myid == 0) { // Farmer
    // init counters
    tasks_per_process = (int *) malloc(sizeof(int)*(numprocs));
    for (i=0; i<numprocs; i++) {
      tasks_per_process[i]=0;
    }
  }
  if (myid == 0) { // Farmer
    area = farmer(numprocs);
  } else { //Workers
    worker(myid);
  }

  if(myid == 0) { // Farmer prints output table when done
    fprintf(stdout, "Area=%lf\n", area);
    fprintf(stdout, "\nTasks Per Process\n");
    for (i=0; i<numprocs; i++) {
      fprintf(stdout, "%d\t", i);
    }
    fprintf(stdout, "\n");
    for (i=0; i<numprocs; i++) {
      fprintf(stdout, "%d\t", tasks_per_process[i]);
    }
    fprintf(stdout, "\n");
    free(tasks_per_process);
  }
  MPI_Finalize();
  return 0;
}

int firstIdleWorker(int *busyWorkers, int numprocs) {
  int i;
  for (i=0; i<numprocs; i++) {
    if (busyWorkers[i] == 0) return i; // Return the index of the first idle worker in the array
  }
  return -1;
}

double farmer(int numprocs) {
  // Initialise variables
  double accumulatedArea = 0, mid, a, b;
  int numIdleWorkers = numprocs-1, idleWorker, i;
  int *busyWorkers = (int *) malloc(sizeof(int)*(numprocs));
  for (i=0; i<numprocs; i++) {busyWorkers[i]=0;}
  busyWorkers[0] = 1; // The farmer is always busy
  MPI_Status status;

  // Initialise stack with first task
  stack *taskStack = new_stack();
  double taskAB[2] = {A, B};
  push(taskAB, taskStack);

  while (!is_empty(taskStack) || numIdleWorkers < numprocs-1) { // While there is still work to do
    while(!is_empty(taskStack) && numIdleWorkers > 0) { // Give out tasks to workers until they are all busy or there are no more tasks
      idleWorker = firstIdleWorker(busyWorkers, numprocs);
      MPI_Send(pop(taskStack), 2, MPI_DOUBLE, idleWorker, 0, MPI_COMM_WORLD);
      tasks_per_process[idleWorker]++; // Keep track of how many tasks each worker has done.
      busyWorkers[idleWorker] = 1; // Keep track of which workers are busy
      numIdleWorkers--;
    }

    // Wait for a worker to report back
    MPI_Recv(&taskAB, 2, MPI_DOUBLE, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
    busyWorkers[status.MPI_SOURCE] = 0;
    numIdleWorkers++;

    // Accumulate result or add new tasks to stack
    if (status.MPI_TAG == NEW_TASKS) { // If the result was inconclusive, divide and carry on conquering
      a = taskAB[0];
      b = taskAB[1];
      mid = (a+b) / 2;
      taskAB[0] = a;
      taskAB[1] = mid;
      push(taskAB, taskStack);
      taskAB[0] = mid;
      taskAB[1] = b;
      push(taskAB, taskStack);
    } else if (status.MPI_TAG == RESULT) { // If the result was within error tolerance, add it to the running total area.
      accumulatedArea += taskAB[0] + taskAB[1];
    }

  }

  // Terminate workers
  for (i=1; i<numprocs; i++) {
    MPI_Send(&taskAB, 2, MPI_DOUBLE, i, NO_MORE_WORK, MPI_COMM_WORLD);
  }

  return accumulatedArea;
}

void worker(int mypid) {
  double taskAB[2], mid, fmid, fleft, fright, larea, rarea, lrarea;
  MPI_Status status;
  while (1) {
    MPI_Recv(&taskAB, 2, MPI_DOUBLE, 0, MPI_ANY_TAG, MPI_COMM_WORLD, &status); // Recieve a task from the farmer
    if (status.MPI_TAG == NO_MORE_WORK) break; // Break if all tasks are done

    // Otherwise do the work
    usleep(SLEEPTIME); // Prevent one worker from hijacking farmer on a sequential machine (for testing only).
    mid = (taskAB[0] + taskAB[1]) / 2;
    fmid = F(mid);
    fleft = F(taskAB[0]);
    fright = F(taskAB[1]);
    larea = (fleft + fmid) * (mid - taskAB[0]) / 2;
    rarea = (fmid + fright) * (taskAB[1] - mid) / 2;
    lrarea = (fleft + fright) * (taskAB[1] - taskAB[0]) / 2;

    // If the answer is within the tolerated error return the result, else return new tasks to the farmer
    if (fabs((larea + rarea) - lrarea) > EPSILON) {
      MPI_Send(&taskAB, 2, MPI_DOUBLE, 0, NEW_TASKS, MPI_COMM_WORLD);  // Farmer will create two new tasks between A and B
    } else {
      taskAB[0] = larea;
      taskAB[1] = rarea;
      MPI_Send(&taskAB, 2, MPI_DOUBLE, 0, RESULT, MPI_COMM_WORLD);  // Farmer will add larea and rarea to final result.
    }
  }
}
