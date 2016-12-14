
//#include "stdafx.h"                             // pre-compiled headers
//#include <mpi.h>
#include <omp.h>

#include <iostream>
#include <iomanip>                              // setprecision
#include "helper1.h"
#include <math.h>
#include <fstream>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#include <ctime>

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

#define  MASTER     0
#define ACCUMULATOR 1

time_t t;

int lineSz;                                     // cache line size

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
        ALIGN(64) volatile long lock_node;
        Node() {key = 0; right = left = NULL, lock_node = 0;} // default constructor
        void acquireTATAS_node();
        void releaseTATAS_node();
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
    //Node lockedNode = *n;
    while (p) {
        //lockedNode = **pp;
        //lockedNode.acquireTATAS_node();
        if (n->key < p->key) {
            pp = &p->left;
        } else if (n->key > p->key) {
            pp = &p->right;
        } else {
            releaseTATAS();
            //lockedNode.releaseTATAS_node();
            return;
        }
        p = *pp;
        //lockedNode.releaseTATAS_node();
    }
    releaseTATAS();
    *pp = n;
}

void BST::remove(INT64 key)
{
    Node* volatile* volatile pp = &root;
    Node* volatile p = root;
    Node lockedNode = Node();
    
    while (p) {
        lockedNode = **pp;
        lockedNode.acquireTATAS_node();
        if (key < p->key) {
            cout << p->key << endl;
            pp = &p->left;
        } else if (key > p->key) {
            pp = &p->right;
        } else {
            lockedNode.releaseTATAS_node();
            break;
        }
        p = *pp;
        lockedNode.releaseTATAS_node();
    }
    if (p == NULL){
        return;
    }
    lockedNode = *p;
    lockedNode.acquireTATAS_node();
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
    lockedNode.releaseTATAS_node();
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
        cout << "acquiring" << endl;
        do {
            cout << "acquiring" << endl;
            _mm_pause();
        } while (lock == 1);
    }
}

void BST::releaseTATAS() {
    lock = 0;
}

void Node::acquireTATAS_node() {
    while (InterlockedExchange(&lock_node, 1) == 1){
        cout << "acquiring" << endl;
        do {
            _mm_pause();
        } while (lock_node == 1);
    }
}

void Node::releaseTATAS_node() {
    lock_node = 0;
}

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
        //BinarySearchTree->remove(randomValue);
    }
}
//
// main
//
int main()
{
    setCommaLocale();
    t = time(0);

    lineSz = getCacheLineSz();
    ops = (UINT64*) malloc(lineSz);

    UINT64 n = 0;

    int iam = 0, np = 1, thread_count =0;
    INT64 total_count[64] = {};
    int numThreads = 0;

    UINT64 *chooseRandom  = new UINT64;
    UINT randomBit;

    omp_set_dynamic(0);     // Explicitly disable dynamic teams
    omp_set_num_threads(16);
    #pragma omp parallel private(iam, np, thread_count, randomBit)
    {
        np = numThreads = omp_get_num_threads();
        iam = omp_get_thread_num();
        thread_count = 0;
        ops[iam] = 0;

        while(1){
            if ((double)(time(0) - t) > NSECONDS-1) {
                break;
            }
            *chooseRandom = rand(*chooseRandom);
            randomBit = *chooseRandom % 2;
            runOp(*chooseRandom % 16, randomBit);
            thread_count += 1;
        }
        total_count[iam] = thread_count;
    }

    BinarySearchTree->destroy(BinarySearchTree->root); //Recursively destroy BST
    BinarySearchTree->root = NULL;

    for (int thread = 0; thread < numThreads; thread++) {
        n += total_count[thread];
    }

    cout << setw(10) << "rt";
    cout << setw(20) << "ops";
    cout << endl;

    cout << setw(10) << "--";        // rt
    cout << setw(20) << "---";       // ops
    cout << endl;

    double rt = (double)(time(0) - t);

    cout << setw(10) << fixed << setprecision(2) << (double) rt;
    cout << setw(20) << n;
    cout << endl;

    cout << endl;

}

// eof