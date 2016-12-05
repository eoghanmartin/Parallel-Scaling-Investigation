#!/bin/csh

#$ -pe mpi-12 12	 # Specify parallel environment and legal core size
#$ -q long		 # Specify queue
#$ -N mpi_test	         # Specify job name

module load mpich	         # Required modules

mpicc -o mpi mpi.c

mpiexec -n $NSLOTS ./mpi # Application to execute