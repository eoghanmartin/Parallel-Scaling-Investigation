#!/bin/csh

#$ -M netid@nd.edu	 # Email address for job notification
#$ -m abe		 # Send mail when job begins, ends and aborts
#$ -pe mpi-12 12	 # Specify parallel environment and legal core size
#$ -q long		 # Specify queue
#$ -N job_name	         # Specify job name

module load xyz	         # Required modules

mpiexec -n $NSLOTS ./app # Application to execute