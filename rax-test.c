/* Rax -- A radix tree implementation.
 *
 * Copyright (c) 2017, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "rax.h"

/* ---------------------------------------------------------------------------
 * Simple hash table implementation, no rehashing, just chaining. This is
 * used in order to test the radix tree implementation against something that
 * will always "tell the truth" :-) */

#define HT_TABLE_SIZE 100000 /* This is huge but we want it fast enough without
                              * reahshing needed. */
typedef struct htNode {
    uint64_t keylen;
    unsigned char *key;
    void *data;
    struct htNode *next;
} htNode;

typedef struct ht {
    uint64_t numele;
    htNode *table[HT_TABLE_SIZE];
} hashtable;

/* Create a new hash table. */
hashtable *htNew(void) {
    hashtable *ht = malloc(sizeof(*ht));
    ht->numele = 0;
    return ht;
}

/* djb2 hash function. */
uint32_t htHash(unsigned char *s, size_t len) {
    uint32_t hash = 5381;
    for (size_t i = 0; i < len; i++)
	hash = hash * 33 + s[i];
    return hash % HT_TABLE_SIZE;
}

/* Low level hash table lookup function. */
htNode *htRawLookup(hashtable *t, unsigned char *s, size_t len, uint32_t *hash, htNode ***parentlink) {
    uint32_t h = htHash(s,len);
    if (hash) *hash = h;
    htNode *n = t->table[h];
    if (parentlink) *parentlink = &t->table[h];
    while(n) {
        if (n->keylen == len && memcmp(n->key,s,len) == 0) return n;
        if (parentlink) *parentlink = &n->next;
        n = n->next;
    }
    return NULL;
}

/* Add an elmenet to the hash table, return 1 if the element is new,
 * 0 if it existed and the value was updated to the new one. */
int htAdd(hashtable *t, unsigned char *s, size_t len, void *data) {
    uint32_t hash;
    htNode *n = htRawLookup(t,s,len,&hash,NULL);

    if (!n) {
        n = malloc(sizeof(*n));
        n->key = malloc(len);
        memcpy(n->key,s,len);
        n->keylen = len;
        n->data = data;
        n->next = t->table[hash];
        t->table[hash] = n;
        t->numele++;
        return 1;
    } else {
        n->data = data;
        return 0;
    }
}

/* Remove the specified element, returns 1 on success, 0 if the element
 * was not there already. */
int htRem(hashtable *t, unsigned char *s, size_t len) {
    htNode **parentlink;
    htNode *n = htRawLookup(t,s,len,NULL,&parentlink);

    if (!n) return 0;
    *parentlink = n->next;
    free(n->key);
    free(n);
    return 1;
}

void *htNotFound = (void*)"ht-not-found";

/* Find an element inside the hash table. Returns htNotFound if the
 * element is not there, otherwise returns the associated value. */
void *htFind(hashtable *t, unsigned char *s, size_t len) {
    htNode *n = htRawLookup(t,s,len,NULL,NULL);
    if (!n) return htNotFound;
    return n->data;
}

/* --------------------------------------------------------------------------
 * Utility functions to generate keys, check time usage and so forth.
 * -------------------------------------------------------------------------*/

/* This is a simple Feistel network in order to turn every possible
 * uint32_t input into another "randomly" looking uint32_t. It is a
 * one to one map so there are no repetitions. */
static uint32_t int2int(uint32_t input) {
    uint16_t l = input & 0xffff;
    uint16_t r = input >> 16;
    for (int i = 0; i < 8; i++) {
        uint16_t nl = r;
        uint16_t F = (((r * 31) + (r >> 5) + 7 * 371) ^ r) & 0xffff;
        r = l ^ F;
        l = nl;
    }
    return (r<<16)|l;
}

/* Turn an uint32_t integer into an alphanumerical key and return its
 * length. This function is used in order to generate keys that have
 * a large charset, so that the radix tree can be testsed with many
 * children per node. */
static size_t int2alphakey(char *s, size_t maxlen, uint32_t i) {
    const char *set = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                      "abcdefghijklmnopqrstuvwxyz"
                      "0123456789";
    const size_t setlen = 62;

    if (maxlen == 0) return 0;
    maxlen--; /* Space for null term char. */
    size_t len = 0;
    while(len < maxlen) {
        s[len++] = set[i%setlen];
        i /= setlen;
        if (i == 0) break;
    }
    s[len] = '\0';
    return len;
}

/* Return the UNIX time in microseconds */
long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec)*1000000;
    ust += tv.tv_usec;
    return ust;
}

/* Turn the integer 'i' into a key according to 'mode'.
 * KEY_INT: Just represents the integer as a string.
 * KEY_UNIQUE_ALPHA: Turn it into a random-looking alphanumerical string
 *                   according to the int2alphakey() function, so that
 *                   at every integer is mapped a different string.
 * KEY_RANDOM: Totally random string up to maxlen bytes. */
#define KEY_INT 0
#define KEY_UNIQUE_ALPHA 1
#define KEY_RANDOM 2
static size_t int2key(char *s, size_t maxlen, uint32_t i, int mode) {
    if (mode == KEY_INT) {
        return snprintf(s,maxlen,"%lu",(unsigned long)i);
    } else if (mode == KEY_UNIQUE_ALPHA) {
        i = int2int(i);
        return int2alphakey(s,maxlen,i);
    } else if (mode == KEY_RANDOM) {
        int r = rand() % maxlen;
        for (int i = 0; i < r; i++) s[i] = rand()&0xff;
        return r;
    } else {
        return 0;
    }
}

/* -------------------------------------------------------------------------- */

/* Perform a fuzz test, returns 0 on success, 1 on error. */
int fuzzTest(int keymode) {
    hashtable *ht = htNew();
    rax *rax = raxNew();

    printf("Fuzz test in mode %d: ", keymode);
    fflush(stdout);

    /* Perform random operations on both the dictionaries. */
    for (int i = 0; i < 1000000; i++) {
        unsigned char key[64];
        uint32_t keylen = int2key((char*)key,sizeof(key),i,keymode);
        void *val = (void*)(unsigned long)htHash(key,keylen);

        int retval1 = htAdd(ht,key,keylen,val);
        int retval2 = raxInsert(rax,key,keylen,val);
        if (retval1 != retval2) {
            printf("Fuzz: key insertion reported mismatching value in HT/RAX\n");
            return 1;
        }
    }

    /* Check that count matches. */
    if (ht->numele != rax->numele) {
        printf("Fuzz: HT / RAX keys count mismatch: %lu vs %lu\n",
            (unsigned long) ht->numele,
            (unsigned long) rax->numele);
        return 1;
    }
    printf("%lu elements inserted\n", (unsigned long)ht->numele);

    /* Check that elements match. */
    raxIterator iter;
    raxStart(&iter,rax);
    raxSeek(&iter,NULL,0,"^");

    size_t numkeys = 0;
    while(raxNext(&iter,NULL,0,NULL)) {
        void *expected = (void*)(unsigned long)htHash(iter.key,iter.key_len);
        void *val1 = htFind(ht,iter.key,iter.key_len);
        void *val2 = raxFind(rax,iter.key,iter.key_len);
        if (val1 != val2 || val1 != expected) {
            printf("Fuzz: HT, RAX, and expected value do not match: %p %p %p\n",
                val1, val2, expected);
            return 1;
        }
        numkeys++;
    }

    /* Check that the iterator reported all the elements. */
    if (ht->numele != numkeys) {
        printf("Fuzz: the iterator reported %lu keys instead of %lu\n",
            (unsigned long) numkeys,
            (unsigned long) ht->numele);
        return 1;
    }

    return 0;
}

int main(int argc, char **argv) {
    /* Tests to run by default are set here. */
    int do_benchmark = 0;
    int do_units = 1;
    int do_fuzz = 1;

    /* If the user passed arguments, override the tests to run. */
    if (argc > 1) {
        do_benchmark = 0;
        do_units = 0;
        do_fuzz = 0;

        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i],"--bench")) {
                do_benchmark = 1;
            } else if (!strcmp(argv[i],"--fuzz")) {
                do_fuzz = 1;
            } else if (!strcmp(argv[i],"--units")) {
                do_units = 1;
            } else {
                fprintf(stderr, "Usage: %s [--bench] [--fuzz] [--units]\n",
                    argv[0]);
                exit(1);
            }
        }
    }

    int errors = 0;
    if (do_fuzz) {
        if (fuzzTest(KEY_INT)) errors++;
        if (fuzzTest(KEY_UNIQUE_ALPHA)) errors++;
        if (fuzzTest(KEY_RANDOM)) errors++;
    }
    return errors;
}