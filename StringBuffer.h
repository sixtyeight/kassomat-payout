/*
 * Taken from: https://sites.google.com/site/rickcreamer/Home/cc/c-implementation-of-stringbuffer-functionality
 */

#ifndef _STRINGBUFFER_H_
#define _STRINGBUFFER_H_

struct StringBuffer;            /* Forward declaration of StringBuffer symbol for typedef line    */
typedef struct StringBuffer SB; /* Forward typedef declaration so can use SB in struct definition */

struct StringBuffer {
    size_t count;                                 /* Number of strings appended                 */
    size_t capacity;                              /* Length of ptrList                          */
    char **ptrList;                               /* Array of char * pointers added w/ append() */
    void (*append) ( SB *sb, char *s );           /* The append() function pointer              */
    char * (*toString) ( SB *sb );                /* The toString() function pointer            */
    void (*dispose) ( SB **sb ); /* The dispose() function pointer             */
};

/* Only quasi-public function - remainder wrapped in StringBuffer struct members */
SB *getStringBuffer();

#endif
