// To compile on DICE use: /usr/lib64/openmpi/bin/mpicc -o partB aquadPartB.c
// and then run with: /usr/lib64/openmpi/bin/mpirun -c 5 ./partB

/*
Implementation strategy
======================
My strategy here was to use the MPI collective operations scatter and gather.  These made the processes of disseminating tasks to
workers, and of collecting the results and the task counts, simpler and more concise.  The alternative would have been a series
of sends and receives which would have worked but been less clear.  One of the side effects of doing this is that the farmer cannot
start adding together the areas from each worker until all have finished processing.  However the number of processes would have to
be enormous before this had any significant effect on running time.

I chose to keep all of the processes in the same communicator.  This had the side effect of causing the farmer process to needlessly
receive a task, and meant it had to provide a dummy area and count in the gather step.  The alternative would be to put all the workers
in their own communicator, and only scatter to and gather from that.  This would, however, just substitute the complication of
ignoring the farmer's part in the collectives with the complication of managing another communicator, so I decided against it.

The workers keep track of how many tasks they have done by passing a reference to a counter into the quad function.  Every time the
function recurses the counter gets incremented and at the end all the workers send their counts back to the farmer in a gather step.

I used separate gather steps for the areas and the counts.  This makes it simpler to understand, and also to implement because the
area and counts have different data types.  I could have cast the count to a double and sent both pieces of information at once,
and this would probably have been more efficient if it takes longer to send two small messages than one slightly bigger one.  However,
because the gathers are only executed once, at the end of the program, the performance cost is negligible so I went with the simple
approach.

The resulting task distribution is very unbalanced between the workers because, although each worker recieves the same sized piece of
the function to integrate, the shape of the function is different in each piece.  Some pieces require many more recursive steps to
accurately estimate the integral of the curve, and so some workers end up doing far more than others.
*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <mpi.h>

#define EPSILON 1e-3
#define F(arg)  cosh(arg)*cosh(arg)*cosh(arg)*cosh(arg)
#define A 0.0
#define B 5.0

#define SLEEPTIME 1

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

  if(myid == 0) {
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

double farmer(int numprocs) {
  int dummyCount=0, i;
  double area=0, dummyTask[2];
  double *areas = (double *) malloc(sizeof(double)*(numprocs));
  double *tasks = (double *) malloc(sizeof(double)*(numprocs*2));

  // Divide the task into equally sized chunks for each worker
  for (i = 0; i<numprocs; i++) {
    tasks[2*i] = A + (i-1)*(B-A)/(numprocs-1);
    tasks[2*i+1] = A + (i)*(B-A)/(numprocs-1);
  }
  
  MPI_Scatter(tasks, 2, MPI_DOUBLE, dummyTask, 2, MPI_DOUBLE, 0, MPI_COMM_WORLD); // Scatter tasks among the workers
  MPI_Gather(&area, 1, MPI_DOUBLE, areas, 1, MPI_DOUBLE, 0, MPI_COMM_WORLD); // Gather computed areas from the workers
  MPI_Gather(&dummyCount, 1, MPI_INT, tasks_per_process, 1, MPI_INT, 0, MPI_COMM_WORLD); // Gather task counts from the workers

  // Sum the areas to get the total
  for (i = 0; i<numprocs; i++) {
    area += areas[i];
  }

  free(areas);
  free(tasks);
  return area;
}

double quad(double left, double right, double fleft, double fright, double lrarea, int *count) {
  (*count)++;
  double mid, fmid, larea, rarea;
  
  mid = (left + right) / 2;
  fmid = F(mid);
  larea = (fleft + fmid) * (mid - left) / 2;
  rarea = (fmid + fright) * (right - mid) / 2;
  if( fabs((larea + rarea) - lrarea) > EPSILON ) {
    larea = quad(left, mid, fleft, fmid, larea, count);
    rarea = quad(mid, right, fmid, fright, rarea, count);
  }
  return (larea + rarea);
}

void worker(int mypid) {
  double taskAB[2];
  int tasksDone = 0; // Calls to quad will increment this
  MPI_Scatter(NULL, 2, MPI_DOUBLE, taskAB, 2, MPI_DOUBLE, 0, MPI_COMM_WORLD); // Recieve task from the farmer
  double area = quad(taskAB[0], taskAB[1], F(taskAB[0]), F(taskAB[1]), (F(taskAB[0])+F(taskAB[1])) * (taskAB[1]-taskAB[0])/2, &tasksDone); // Do the work and count calls to the quad function
  MPI_Gather(&area, 1, MPI_DOUBLE, NULL, 1, MPI_DOUBLE, 0, MPI_COMM_WORLD); // Send area to farmer
  MPI_Gather(&tasksDone, 1, MPI_INT, NULL, 1, MPI_INT, 0, MPI_COMM_WORLD); // Send task count to farmer
}
