#!/bin/csh

#$ -M emarti17@nd.edu	 # Email address for job notification
#$ -m abe		 # Send mail when job begins, ends and aborts
#$ -pe smp 8 # Specify parallel environment and legal core size
#$ -q debug		 # Specify queue
#$ -N mpi_test	         # Specify job name

mpirun  mpi