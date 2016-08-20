/*
 * Taken from: https://sites.google.com/site/rickcreamer/Home/cc/c-implementation-of-stringbuffer-functionality
 */

#include <stdlib.h>
#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include "StringBuffer.h"

/* Constant determining initial pointer list length - is small so we can test growPtrList() */

#define STR_BUF_INIT_SIZE 4

/* Function declarations */

void append(SB *sb, char *s);
char *toString(SB *sb);
void dispose(SB **sb);
void error(char *msg);
void growPtrList(SB *sb);
void *getMem(int nBytes);
void init(SB *sb);

/* Quasi-public functions exposed as function pointers in StringBuffer structure */

/* Factory-like StringBuffer instantiator */
SB * getStringBuffer() {
	SB *sb = (SB *) getMem(sizeof(SB));
	init(sb);
	return sb;
}

/* Append a string to the StringBuffer */
void append(SB *sb, char *s) {
	if (!s)
		error("Null pointer passed for argument 's' in SB.append()!"); /* Abort */
	if (sb->count == sb->capacity)
		growPtrList(sb);

	char *copy_of_s=NULL;
	asprintf(&copy_of_s, "%s", s);

	sb->ptrList[sb->count++] = copy_of_s;
}

/* Catenate all strings and return result */
char *toString(SB *sb) {
	if (!sb || !sb->count)
		return ""; /* TODO: Decide if error message or other action is preferable */
	int len = 0;
	for (size_t i = 0; i < sb->count; ++i)
		len += strlen(sb->ptrList[i]);
	size_t nBytes = (len + 1) * sizeof(char); /* 1 is for '\0' null terminator */
	char *ret = (char *) getMem(nBytes);
	for (size_t i = 0; i < sb->count; ++i)
		strcat(ret, sb->ptrList[i]);
	return ret;
}

/* Delete this StringBuffer object and free all memory */
/* Argument 'freeStrings' controls whether the individual append()ed strings will be freed */
/* Note: The argument 'sb' is the ADDRESS of the POINTER to a StringBuffer structure */
void dispose(SB **sb) {
	if (!sb || !*sb || !(*sb)->ptrList)
		return; /* TODO: Decide if should abort here or take other action */

	// free all the strings allocated in append
	for (size_t i = 0; i < (*sb)->count; ++i)
		free((*sb)->ptrList[i]);

	free((*sb)->ptrList);
	free(*sb);
	*sb = 0; /* Set value of pointer to zero */
}

/* Begin of quasi-private functions */

/* Print simple error message to stderr and call exit() */
void error(char *msg) {
	fprintf( stderr, "%s\n", (msg) ? msg : "");
	exit(1);
}

/* Double length of the array of pointers when append() needs to go past current limit */
void growPtrList(SB *sb) {
	size_t nBytes = 2 * sb->capacity * sizeof(char *);
	char **pTemp = (char **) getMem(nBytes);
	memcpy((void *) pTemp, (void *) sb->ptrList, sb->capacity * sizeof(char *));
	sb->capacity *= 2;
	free(sb->ptrList);
	sb->ptrList = pTemp;
}

/* Wrapper around malloc() - allocate memory and initialize to all zeros */
void *getMem(int nBytes) {
	void *ret = malloc(nBytes);
	if (!ret)
		error("Memory allocation failed!");
	memset(ret, 0, nBytes);
	return ret;
}

/* Initialize a new StringBuffer structure */
void init(SB *sb) {
	sb->count = 0;
	sb->capacity = STR_BUF_INIT_SIZE;
	sb->ptrList = (char **) getMem( STR_BUF_INIT_SIZE * sizeof(char *));
	sb->append = append;
	sb->toString = toString;
	sb->dispose = dispose;
}
