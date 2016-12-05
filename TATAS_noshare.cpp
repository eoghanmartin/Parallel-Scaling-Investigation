//
// sharing.cpp
//
// Copyright (C) 2013 - 2015 jones@scss.tcd.ie
//
// This program is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software Foundation;
// either version 2 of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
// without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//
// 19/11/12 first version
// 19/11/12 works with Win32 and x64
// 21/11/12 works with Character Set: Not Set, Unicode Character Set or Multi-Byte Character
// 21/11/12 output results so they can be easily pasted into a spreadsheet from console
// 24/12/12 increment using (0) non atomic increment (1) InterlockedIncrement64 (2) InterlockedCompareExchange
// 12/07/13 increment using (3) RTM (restricted transactional memory)
// 18/07/13 added performance counters
// 27/08/13 choice of 32 or 64 bit counters (32 bit can oveflow if run time longer than a couple of seconds)
// 28/08/13 extended struct Result
// 16/09/13 linux support (needs g++ 4.8 or later)
// 21/09/13 added getWallClockMS()
// 12/10/13 Visual Studio 2013 RC
// 12/10/13 added FALSESHARING
// 14/10/14 added USEPMS
//

//
// NB: hints for pasting from console window
// NB: Edit -> Select All followed by Edit -> Copy
// NB: paste into Excel using paste "Use Text Import Wizard" option and select "/" as the delimiter
//

//#include "stdafx.h"                             // pre-compiled headers
#include <iostream>
#include <iomanip>                              // setprecision
#include "helper1.h"
#include <math.h>
#include <fstream>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

#include <termios.h>        //
#include <unistd.h>         //
#include <limits.h>         // HOST_NAME_MAX
#include <sys/utsname.h>    //
#include <fcntl.h>

using namespace std;

#define K           1024
#define GB          (K*K*K)
#define NOPS        1000
#define NSECONDS    1                           // run each test for NSECONDS

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

clock_t start, stop;
double elapsed;

UINT64 tstart;                                  // start of test in ms
int sharing;
int lineSz;                                     // cache line size
int maxThread;                                  // max # of threads

THREADH *threadH;                               // thread handles
UINT64 *ops;                                    // for ops per thread

//ALIGN(64) volatile long lock = 0;

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

//
// getPageSz
//
UINT getPageSz()
{
    return sysconf(_SC_PAGESIZE);
}

//
// getWallClockMS
//
UINT64 getWallClockMS()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
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
void worker()
{
    int thread = 0;

    UINT64 n = 0;

    //runThreadOnCPU(thread % ncpu);

    UINT *chooseRandom  = new UINT;
    UINT randomValue;
    UINT randomBit;

    while (1) {
        for(int y=0; y<NOPS; y++) {
            randomBit = 0;
            *chooseRandom = rand(*chooseRandom);
            randomBit = *chooseRandom % 2;
            runOp(*chooseRandom % 16, randomBit);
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
        //if ((gettimeofday() - tstart) > NSECONDS*1000)
        //    break;
    }
    ops[thread] = n;
    BinarySearchTree->destroy(BinarySearchTree->root); //Recursively destroy BST
    BinarySearchTree->root = NULL;
}
/*
//
// worker
//
WORKER worker(void *vthread)
{
    int thread = (int)((size_t) vthread);

    UINT64 n = 0;

    runThreadOnCPU(thread % ncpu);

    UINT *chooseRandom  = new UINT;
    UINT randomValue;
    UINT randomBit;

    while (1) {
        for(int y=0; y<NOPS; y++) {
            randomBit = 0;
            *chooseRandom = rand(*chooseRandom);
            randomBit = *chooseRandom % 2;
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
        }
        n += NOPS;
        //
        // check if runtime exceeded
        //
        if ((getWallClockMS() - tstart) > NSECONDS*1000)
            break;
    }
    ops[thread] = n;
    BinarySearchTree->destroy(BinarySearchTree->root); //Recursively destroy BST
    BinarySearchTree->root = NULL;
    return 0;
}
*/
//
// main
//
int main()
{
    //ncpu = getNumberOfCPUs();   // number of logical CPUs
    //maxThread = 2 * ncpu;       // max number of threads
    maxThread = 0;
    //
    // get date
    //
    //char dateAndTime[256];
    //getDateAndTime(dateAndTime, sizeof(dateAndTime));
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
   // memset(r, 0, 5*maxThread*sizeof(Result));                                        // zero

    indx = 0;
    //
    // use thousands comma separator
    //
    setCommaLocale();
    //
    // header
    //
    cout << setw(13) << "BST";
    cout << setw(10) << "nt";
    cout << setw(10) << "rt";
    cout << setw(20) << "ops";
    cout << setw(10) << "rel";
    cout << endl;

    cout << setw(13) << "---";       // random count
    cout << setw(10) << "--";        // nt
    cout << setw(10) << "--";        // rt
    cout << setw(20) << "---";       // ops
    cout << setw(10) << "---";       // rel
    cout << endl;

    //
    // run tests
    //
    UINT64 ops1 = 1;

    int nt = 1;
    int sharing = 0;
    int thread = 0;

/*
    for (sharing = 0; sharing < 5; sharing++) {
        for (int nt = 1; nt <= maxThread; nt+=1, indx++) {
            //
            //  zero shared memory
            //
            for (int thread = 0; thread < nt; thread++)
                *(GINDX(thread)) = 0;   // thread local
            *(GINDX(maxThread)) = 0;    // shared
            //
            // get start time
            //
            */
            //tstart = gettimeofday();
            start = clock();
            /*
            //
            // create worker threads
            //
            for (int thread = 0; thread < nt; thread++)
                createThread(&threadH[thread], worker, (void*)(size_t)thread);
            //
            // wait for ALL worker threads to finish
            //
            waitForThreadsToFinish(nt, threadH);
            */
            worker();
            double rt = (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;;
            //UINT64 rt = gettimeofday() - tstart;

            //
            // save results and output summary to console
            //
            for (int thread = 0; thread < nt; thread++) {
                r[indx].ops += ops[thread];
                r[indx].incs += *(GINDX(thread));
            }
            r[indx].incs += *(GINDX(maxThread));
            if ((sharing == 0) && (nt == 1))
                ops1 = r[indx].ops;
            r[indx].sharing = sharing;
            r[indx].nt = nt;
            r[indx].rt = rt;

            cout << setw(13) << pow(16,sharing+1);
            cout << setw(10) << nt;
            cout << setw(10) << fixed << setprecision(2) << (double) rt / 1000;
            cout << setw(20) << r[indx].ops;
            cout << setw(10) << fixed << setprecision(2) << (double) r[indx].ops / ops1;
            cout << endl;

            /*
            ofstream metrics;
            metrics.open("metricsTATAS.txt", ios_base::app);

            metrics << pow(16,sharing+1) << ", ";
            metrics << nt << ", ";
            metrics << fixed << setprecision(2) << (double)rt / 1000 << ", ";
            metrics << r[indx].ops << ", ";
            metrics << fixed << setprecision(2) << (double)r[indx].ops / ops1;
            metrics << endl;

            metrics.close();

            //
            // delete thread handles
            //
            for (int thread = 0; thread < nt; thread++) {
                closeThread(threadH[thread]);
            }
        }
    }
*/

    cout << endl;
    //quit();

    return 0;

}

// eof