
//#include "stdafx.h"                             // pre-compiled headers
#include <mpi.h>
#include <iostream>
#include <iomanip>                              // setprecision
#include "helper1.h"
#include <math.h>
#include <fstream>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#include <termios.h>        //
#include <unistd.h>         //
#include <limits.h>         // HOST_NAME_MAX
#include <sys/utsname.h>    //
#include <fcntl.h>

using namespace std;

#define K           1024
#define GB          (K*K*K)
#define NOPS        1000
#define NSECONDS    2                           // run each test for NSECONDS

#define COUNTER64                               // comment for 32 bit counter

#ifdef COUNTER64
#define VINT    UINT64                          //  64 bit counter
#else
#define VINT    UINT                            //  32 bit counter
#endif

#ifdef FALSESHARING
#define GINDX(n)    (g+n)
#else
#define GINDX(n)    (g+n*lineSz/sizeof(VINT))
#endif

#define ALIGNED_MALLOC(sz, align) _aligned_malloc(sz, align)

#define  MASTER     1
#define ACCUMULATOR 0

clock_t start, stop;
double elapsed;

int lineSz;                                     // cache line size
int maxThread;                                  // max # of threads

THREADH *threadH;                               // thread handles
UINT64 *ops;                                    // for ops per thread

void _mm_pause ()
{
    __asm__ __volatile__ ("rep; nop" : : );
}

struct _cd {
    UINT eax;
    UINT ebx;
    UINT ecx;
    UINT edx;
} cd;

//
// look for L1 cache line size (see Intel Application note on CPUID instruction)
//
int lookForL1DataCacheInfo(int v)
{
    if (v & 0x80000000)
        return 0;

    for (int i = 0; i < 4; i++) {
        switch (v & 0xff) {
        case 0x0a:
        case 0x0c:
        case 0x10:
            return 32;
        case 0x0e:
        case 0x2c:
        case 0x60:
        case 0x66:
        case 0x67:
        case 0x68:
            return 64;
        }
        v >>= 8;
    }
    return 0;
}

//
// getL1DataCacheInfo
//
int getL1DataCacheInfo()
{
    CPUID(cd, 2);

    if ((cd.eax & 0xff) != 1) {
        cout << "unrecognised cache type: default L 64" << endl;
        return 64;
    }

    int sz;

    if ((sz = lookForL1DataCacheInfo(cd.eax & ~0xff)))
        return sz;
    if ((sz = lookForL1DataCacheInfo(cd.ebx)))
        return sz;
    if ((sz = lookForL1DataCacheInfo(cd.ecx)))
        return sz;
    if ((sz = lookForL1DataCacheInfo(cd.edx)))
        return sz;

    cout << "unrecognised cache type: default L 64" << endl;
    return 64;
}

//
// getCacheInfo
//
int getCacheInfo(int level, int data, int &l, int &k, int&n)
{
    CPUID(cd, 0x00);
    if (cd.eax < 4)
        return 0;
    int i = 0;
    while (1) {
        CPUIDEX(cd, 0x04, i);
        int type = cd.eax & 0x1f;
        if (type == 0)
            return 0;
        int lev = ((cd.eax >> 5) & 0x07);
        if ((lev == level) && (((data == 0) && (type = 2)) || ((data == 1) && (type == 1))))
            break;
        i++;
    }
    k = ((cd.ebx >> 22) & 0x03ff) + 1;
    int partitions = ((cd.ebx) >> 12 & 0x03ff) + 1;
    n = cd.ecx + 1;
    l = (cd.ebx & 0x0fff) + 1;
    return partitions == 1;
}

//
// getDeterministicCacheInfo
//
int getDeterministicCacheInfo()
{
    int type, ways, partitions, lineSz = 0, sets;
    int i = 0;
    while (1) {
        CPUIDEX(cd, 0x04, i);
        type = cd.eax & 0x1f;
        if (type == 0)
            break;
        cout << "L" << ((cd.eax >> 5) & 0x07);
        cout << ((type == 1) ? " D" : (type == 2) ? " I" : " U");
        ways = ((cd.ebx >> 22) & 0x03ff) + 1;
        partitions = ((cd.ebx) >> 12 & 0x03ff) + 1;
        sets = cd.ecx + 1;
        lineSz = (cd.ebx & 0x0fff) + 1;
        cout << " " << setw(5) << ways*partitions*lineSz*sets / 1024 << "K" << " L" << setw(3) << lineSz << " K" << setw(3) << ways << " N" << setw(5) << sets;
        cout << endl;
        i++;
    }
    return lineSz;
}

//
// getCacheLineSz
//
int getCacheLineSz()
{
    CPUID(cd, 0x00);
    if (cd.eax >= 4)
        return getDeterministicCacheInfo();
    return getL1DataCacheInfo();
}

UINT64 rand(UINT64 &r)
{
    r ^= r >> 12;   // a
    r ^= r << 25;   // b
    r ^= r >> 27;   // c
    return r * 2685821657736338717LL;
}

locale *commaLocale = NULL;

//
// setCommaLocale
//
void setCommaLocale()
{
    if (commaLocale == NULL)
        commaLocale = new locale(locale(), new CommaLocale());
    cout.imbue(*commaLocale);
}

//
// setLocale
//
void setLocale()
{
    cout.imbue(locale());
}

class Node {
    public:
        INT64 volatile key;
        Node* volatile left;
        Node* volatile right;
        Node() {key = 0; right = left = NULL;} // default constructor
};

class BST {
    public:
        Node* volatile root; // root of BST, initially NULL
        ALIGN(64) volatile long lock;
        BST() {root = NULL, lock = 0;} // default constructor
        void add(Node *nn); // add node to tree
        void destroy(volatile Node *nextNode);
        void remove(INT64 key); // remove key from tree
        void releaseTATAS();  //HLE functionality added to BST class
        void acquireTATAS();
};

BST *BinarySearchTree = new BST;

void BST::add (Node *n)
{
    acquireTATAS();
    Node* volatile* volatile pp = &root;
    Node* volatile p = root;
    while (p) {
        if (n->key < p->key) {
        pp = &p->left;
        } else if (n->key > p->key) {
            pp = &p->right;
            } else {
                releaseTATAS();
                return;
            }
        p = *pp;
    }
    *pp = n;
    releaseTATAS();
}

void BST::remove(INT64 key)
{
    acquireTATAS();
    Node* volatile* volatile pp = &root;
    Node* volatile p = root;
    while (p) {
        if (key < p->key) {
            pp = &p->left;
        } else if (key > p->key) {
            pp = &p->right;
            } else {
                break;
            }
        p = *pp;
    }
    if (p == NULL)
        releaseTATAS();
        return;
    if (p->left == NULL && p->right == NULL) {
        *pp = NULL; // NO children
    } else if (p->left == NULL) {
        *pp = p->right; // ONE child
    } else if (p->right == NULL) {
        *pp = p->left; // ONE child
    } else {
        Node *r = p->right; // TWO children
        Node* volatile* volatile ppr = &p->right; // find min key in right sub tree
        while (r->left) {
            ppr = &r->left;
            r = r->left;
        }
        p->key = r->key; // could move...
        p = r; // node instead
        *ppr = r->right;
    }
    releaseTATAS();
}

void BST::destroy(volatile Node *nextNode)
{
    if (nextNode != NULL)
    {
        destroy(nextNode->left);
        destroy(nextNode->right);
    }
}

void BST::acquireTATAS() {
    while (InterlockedExchange(&lock, 1) == 1){
        do {
            _mm_pause();
        } while (lock == 1);
    }
}

void BST::releaseTATAS() {
    lock = 0;
}

typedef struct {
    int sharing;                                // sharing
    int nt;                                     // # threads
    double rt;                                  // run time (ms)
    UINT64 ops;                                 // ops
    UINT64 incs;                                // should be equal ops
} Result;

Result *r;                                      // results
UINT indx;                                      // results index

volatile VINT *g;                               // NB: position of volatile

void runOp(UINT randomValue, UINT randomBit) {
    if (randomBit) {
        Node *addNode = new Node;
        addNode->key = randomValue;
        addNode->left = NULL;
        addNode->right = NULL;
        BinarySearchTree->add(addNode);
    }
    else {
        BinarySearchTree->remove(randomValue);
    }
}
//
// worker
//
void worker(int rank)
{
    int thread = rank;

    UINT64 n = 0;

    UINT64 *chooseRandom  = new UINT64;
    UINT randomValue;
    UINT randomBit;

    while (1) {
        for(int y=0; y<NOPS; y++) {
            randomBit = 0;
            *chooseRandom = rand(*chooseRandom);
            randomBit = *chooseRandom % 2;
            runOp(*chooseRandom % 16, randomBit); //up to 32 branch tree (-16 - 16)
            /*
            switch (sharing) {
                case 0:
                    runOp(*chooseRandom % 16, randomBit);
                    break;
                case 1:
                    randomValue = *chooseRandom % 256;
                    runOp(randomValue, randomBit);
                    break;
                case 2:
                    randomValue = *chooseRandom % 4096;
                    runOp(randomValue, randomBit);
                    break;
                case 3:
                    randomValue = *chooseRandom % 65536;
                    runOp(randomValue, randomBit);
                    break;
                case 4:
                    randomValue = *chooseRandom % 1048576;
                    runOp(randomValue, randomBit);
                    break;
            }
            */
        }
        n += NOPS;
        //
        // check if runtime exceeded
        //
        if (((double)(clock() - start) * 1000.0) / CLOCKS_PER_SEC > NSECONDS*1000)
            break;
    }
    ops[thread] = n;
    BinarySearchTree->destroy(BinarySearchTree->root); //Recursively destroy BST
    BinarySearchTree->root = NULL;
}
//
// main
//
int main()
{
    MPI_Init(NULL, NULL);

    maxThread = 4;
    //
    // get cache info
    //
    lineSz = getCacheLineSz();
    //
    // allocate global variable
    //
    // NB: each element in g is stored in a different cache line to stop false sharing
    //
    ops = (UINT64*) malloc(lineSz);                   // for ops per thread

    g = (VINT*) malloc(lineSz);                         // local and shared global variables

    r = (Result*) malloc(lineSz);                   // for results
   // memset(r, 0, 5*maxThread*sizeof(Result));     // zero

    indx = 0;

    UINT64 ops1 = 1;

    start = clock();
    // Initialize the MPI environment

    MPI_Status status;
    MPI_Status status_master;

    // Get the number of processes
    int world_size;
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    // Get the rank of the process
    int world_rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);

    // Get the name of the processor
    char processor_name[MPI_MAX_PROCESSOR_NAME];
    int name_len;
    MPI_Get_processor_name(processor_name, &name_len);

    int message;

    if (world_rank == ACCUMULATOR) {
        /*
        double cont = (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;
        while (cont < 3000) {
            cont = (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;
        }
        setCommaLocale();
        //
        // save results and output summary to console
        //
        int thread = 0;
        double rt = (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;
        for (thread = 2; thread < maxThread; thread++) {
            r[indx].ops += ops[thread];
            r[indx].incs += *(GINDX(thread));
        }
        r[indx].incs += *(GINDX(maxThread));
        ops1 = ops[thread]; //r[indx].ops;
        r[indx].nt = maxThread;
        r[indx].rt = rt;

        cout << setw(13) << pow(16,1); // sharing+1);
        cout << setw(10) << maxThread; //nt;
        cout << setw(10) << fixed << setprecision(2) << (double) rt / 1000;
        cout << setw(20) << r[indx].ops;
        cout << setw(10) << fixed << setprecision(2) << (double) r[indx].ops / ops1;
        cout << endl;

        cout << endl;
        */
    }
    else if (world_rank == MASTER) {
        int message_recv[2];
        UINT randomValue_recv;
        UINT randomBit_recv;

        UINT64 n = 0;

        while(1){
            MPI_Recv(&message_recv, 2, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status_master);
            cout << "message recieved is: " << message_recv[0] << " and " << message_recv[1] << endl;
            randomValue_recv = message_recv[0];
            randomBit_recv = message_recv[1];
            runOp(randomValue_recv, randomBit_recv);
            ops[status_master.MPI_SOURCE] = ops[status_master.MPI_SOURCE] + 1;
            MPI_Send(&world_rank, 1, MPI_INT, status_master.MPI_SOURCE, 1, MPI_COMM_WORLD);
            n += 1;
            if (((double)(clock() - start) * 1000.0) / CLOCKS_PER_SEC > NSECONDS*1000)
                break;
        }
        BinarySearchTree->destroy(BinarySearchTree->root); //Recursively destroy BST
        BinarySearchTree->root = NULL;

        setCommaLocale();

        cout << setw(10) << "nt";
        cout << setw(10) << "rt";
        cout << setw(20) << "ops";
        cout << endl;

        cout << setw(10) << "--";        // nt
        cout << setw(10) << "--";        // rt
        cout << setw(20) << "---";       // ops
        cout << endl;

        double rt = (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;
        r[0].nt = maxThread;
        r[0].ops = n;

        cout << setw(10) << r[0].nt ; //nt;
        cout << setw(10) << fixed << setprecision(2) << (double) rt / 1000;
        cout << setw(20) << r[0].ops;
        cout << endl;

        cout << endl;
    }
    else {
        int message_send[2];

        UINT64 n = 0;

        UINT64 *chooseRandom  = new UINT64;
        UINT randomValue;
        UINT randomBit;

        printf("Processor %s, rank %d"
           " out of %d processors\n",
           processor_name, world_rank, world_size);

        while (1) {
            randomBit = 0;
            *chooseRandom = rand(*chooseRandom);
            randomBit = *chooseRandom % 2;
            message_send[0] = *chooseRandom % 16;
            message_send[1] = randomBit;
            MPI_Send(&message_send, 2, MPI_INT, MASTER, 1, MPI_COMM_WORLD);
            MPI_Recv(&message, 1, MPI_INT, MASTER, 1, MPI_COMM_WORLD, &status);
            cout << "This should be MASTER: " << message << endl;
            n += 1;
            //
            // check if runtime exceeded
            //
            if (((double)(clock() - start) * 1000.0) / CLOCKS_PER_SEC > NSECONDS*1000)
                break;
        }
        cout << "Number of ops for process " << world_rank << ": " << n << endl;
    }

    // Finalize the MPI environment.
    MPI_Finalize();

}

// eof