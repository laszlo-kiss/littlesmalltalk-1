/*
    Little Smalltalk memory management
    Written by Tim Budd, budd@cs.orst.edu

    Uses baker two-space garbage collection algorithm

    Relicensed under BSD 3-clause license per permission from Dr. Budd by
    Kyle Hayes.

    See LICENSE file.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>
#include "memory.h"
#include "globs.h"

extern int debugging;   /* true if we are debugging */


int64_t gc_count = 0;
int64_t gc_total_time = 0;
int64_t gc_max_time = 0;
int64_t gc_total_mem_copied = 0;
int64_t gc_mem_max_copied = 0;

/*
    static memory space -- never recovered
*/
static struct object *staticBase, *staticTop, *staticPointer;

/*
    dynamic (managed) memory space
    recovered using garbage collection
*/

static struct object *spaceOne, *spaceTwo;
static int spaceSize;

struct object *memoryBase, *memoryPointer, *memoryTop;

static int inSpaceOne;
static struct object *oldBase, *oldTop;

/*
    roots for memory access
    used as bases for garbage collection algorithm
*/
struct object *rootStack[ROOTSTACKLIMIT];
int rootTop = 0;
#define STATICROOTLIMIT (200)
static struct object **staticRoots[STATICROOTLIMIT];
static int staticRootTop = 0;



/* local routines */
//static int64_t time_usec();


/*
    test routine to see if a pointer is in dynamic memory
    area or not
*/

int isDynamicMemory(struct object *x)
{
    return ((x >= spaceOne) && (x <= (spaceOne + spaceSize))) ||
           ((x >= spaceTwo) && (x <= (spaceTwo + spaceSize)));
}

/*
    gcinit -- initialize the memory management system
*/
void gcinit(int staticsz, int dynamicsz)
{
    /* allocate the memory areas */
    staticBase = (struct object *)
                 malloc(staticsz * sizeof(struct object));
    spaceOne = (struct object *)
               malloc(dynamicsz * sizeof(struct object));
    spaceTwo = (struct object *)
               malloc(dynamicsz * sizeof(struct object));
    if ((staticBase == 0) || (spaceOne == 0) || (spaceTwo == 0)) {
        sysError("not enough memory for space allocations\n");
    }

    staticTop = staticBase + staticsz;
    staticPointer = staticTop;

    spaceSize = dynamicsz;
    memoryBase = spaceOne;
    memoryPointer = memoryBase + spaceSize;
    if (debugging) {
        printf("space one 0x%lx, top 0x%lx,"
               " space two 0x%lx , top 0x%lx\n",
               (intptr_t)spaceOne, (intptr_t)(spaceOne + spaceSize),
               (intptr_t)spaceTwo, (intptr_t)(spaceTwo + spaceSize));
    }
    inSpaceOne = 1;
}

/*
    gc_move is the heart of the garbage collection algorithm.
    It takes as argument a pointer to a value in the old space,
    and moves it, and everything it points to, into the new space
    The returned value is the address in the new space.
*/
struct mobject {
    uint size;
    struct mobject *data[0];
};

static struct object *gc_move(struct mobject *ptr)
{
    struct mobject *old_address = ptr, *previous_object = 0,*new_address = 0, *replacement  = 0;
    int sz;

    while (1) {

        /*
         * part 1.  Walking down the tree
         * keep stacking objects to be moved until we find
         * one that we can handle
         */
        for (;;) {
            /*
             * SmallInt's are not proper memory pointers,
             * so catch them first.  Their "object pointer"
             * value can be used as-is in the new space.
             */
            if (IS_SMALLINT(old_address)) {
                replacement = old_address;
                old_address = previous_object;
                break;

                /*
                 * If we find a pointer in the current space
                 * to the new space (other than indirections) then
                 * something is very wrong
                 */
            } else if ((old_address >=
                        (struct mobject *) memoryBase)
                       && (old_address <= (struct mobject *) memoryTop)) {
                sysErrorInt("GC invariant failure -- address in new space",
                            (intptr_t)old_address);

                /* else see if not  in old space */
            } else if ((old_address < (struct mobject *) oldBase) ||
                       (old_address > (struct mobject *) oldTop)) {
                replacement = old_address;
                old_address = previous_object;
                break;

                /* else see if already forwarded */
            } else if (old_address->size & FLAG_GCDONE)  {
                if (old_address->size & FLAG_BIN) {
                    replacement = old_address->data[0];
                } else {
                    sz = SIZE(old_address);
                    replacement = old_address->data[sz];
                }
                old_address = previous_object;
                break;

                /* else see if binary object */
            } else if (old_address->size & FLAG_BIN) {
                int isz;

                isz = SIZE(old_address);
                sz = (isz + BytesPerWord - 1)/BytesPerWord;
                memoryPointer = WORDSDOWN(memoryPointer,
                                          sz + 2);
                new_address = (struct mobject *)memoryPointer;
                SETSIZE(new_address, isz);
                new_address->size |= FLAG_BIN;
                while (sz) {
                    new_address->data[sz] =
                        old_address->data[sz];
                    sz--;
                }
                old_address->size |= FLAG_GCDONE;
                new_address->data[0] = previous_object;
                previous_object = old_address;
                old_address = old_address->data[0];
                previous_object->data[0] = new_address;
                /* now go chase down class pointer */

                /* must be non-binary object */
            } else  {
                sz = SIZE(old_address);
                memoryPointer = WORDSDOWN(memoryPointer,
                                          sz + 2);
                new_address = (struct mobject *)memoryPointer;
                SETSIZE(new_address, sz);
                old_address->size |= FLAG_GCDONE;
                new_address->data[sz] = previous_object;
                previous_object = old_address;
                old_address = old_address->data[sz];
                previous_object->data[sz] = new_address;
            }
        }

        /*
         * part 2.  Fix up pointers,
         * move back up tree as long as possible
         * old_address points to an object in the old space,
         * which in turns points to an object in the new space,
         * which holds a pointer that is now to be replaced.
         * the value in replacement is the new value
         */
        for (;;) {
            /* backed out entirely */
            if (old_address == 0) {
                return (struct object *) replacement;
            }

            /* case 1, binary or last value */
            if ((old_address->size & FLAG_BIN) ||
                    (SIZE(old_address) == 0)) {

                /* fix up class pointer */
                new_address = old_address->data[0];
                previous_object = new_address->data[0];
                new_address->data[0] = replacement;
                old_address->data[0] = new_address;
                replacement = new_address;
                old_address = previous_object;
            } else {
                sz = SIZE(old_address);
                new_address = old_address->data[sz];
                previous_object = new_address->data[sz];
                new_address->data[sz] = replacement;
                sz--;

                /*
                 * quick cheat for recovering zero fields
                 */
                while (sz && (old_address->data[sz] == 0)) {
                    new_address->data[sz--] = 0;
                }

                SETSIZE(old_address, sz);
                old_address->size |= FLAG_GCDONE;
                new_address->data[sz] = previous_object;
                previous_object = old_address;
                old_address = old_address->data[sz];
                previous_object->data[sz] = new_address;
                break; /* go track down this value */
            }
        }
    }

    /* make the compiler happy */
    return (struct object *)0;
}

/*
    gcollect -- garbage collection entry point
*/


struct object *gcollect(int sz)
{
    int i;
    int64_t start = time_usec();
    int64_t end = 0;

    /* first change spaces */
    if (inSpaceOne) {
        memoryBase = spaceTwo;
        inSpaceOne = 0;
        oldBase = spaceOne;
    } else {
        memoryBase = spaceOne;
        inSpaceOne = 1;
        oldBase = spaceTwo;
    }

    memoryPointer = memoryTop = memoryBase + spaceSize;
    oldTop = oldBase + spaceSize;

    /* then do the collection */
    for (i = 0; i < rootTop; i++) {
        rootStack[i] = gc_move((struct mobject *) rootStack[i]);
    }
    for (i = 0; i < staticRootTop; i++) {
        (* staticRoots[i]) =  gc_move((struct mobject *)
                                      *staticRoots[i]);
    }

    flushCache();

    gc_total_mem_copied += ((char *)memoryTop - (char *)memoryPointer);
    if(((char *)memoryTop - (char *)memoryPointer) > gc_mem_max_copied) {
        gc_mem_max_copied = ((char *)memoryTop - (char *)memoryPointer);
    }

    /* then see if there is room for allocation */
    memoryPointer = WORDSDOWN(memoryPointer, sz + 2);
    if (memoryPointer < memoryBase) {
        sysErrorInt("insufficient memory after garbage collection", sz);
    }
    SETSIZE(memoryPointer, sz);

    end = time_usec();

    /* calculate stats about the GC runs. */
    gc_count++;
    gc_total_time += (end - start);

    if(gc_max_time < (end - start)) {
        gc_max_time = (end - start);
    }

    return(memoryPointer);
}

/*
    static allocation -- tries to allocate values in an area
    that will not be subject to garbage collection
*/

struct object *staticAllocate(int sz)
{
    staticPointer = WORDSDOWN(staticPointer, sz + 2);
    if (staticPointer < staticBase) {
        sysError("insufficient static memory");
    }
    SETSIZE(staticPointer, sz);
    return(staticPointer);
}

struct object *staticIAllocate(int sz)
{
    int trueSize;
    struct object *result;

    trueSize = (sz + BytesPerWord - 1) / BytesPerWord;
    result = staticAllocate(trueSize);
    SETSIZE(result, sz);
    result->size |= FLAG_BIN;
    return result;
}

/*
    if definition is not in-lined, here  is what it should be
*/
#ifndef gcalloc
struct object *gcalloc(int sz)
{
    struct object *result;

    memoryPointer = WORDSDOWN(memoryPointer, sz + 2);
    if (memoryPointer < memoryBase) {
        return gcollect(sz);
    }
    SETSIZE(memoryPointer, sz);
    return(memoryPointer);
}
# endif

struct object *gcialloc(int sz)
{
    int trueSize;
    struct object *result;

    trueSize = (sz + BytesPerWord - 1) / BytesPerWord;
    result = gcalloc(trueSize);
    SETSIZE(result, sz);
    result->size |= FLAG_BIN;
    return result;
}

/*
    File in and file out of Smalltalk images
*/

static int indirtop = 0;
static struct object **indirArray;




/* return the size in bytes necessary to accurately handle the integer
value passed.  Note that negatives will always get BytesPerWord size.
This will return zero if the passed value is less than LST_SMALL_TAG_LIMIT.
In this case, the value can be packed into the tag it self when read or
written. */

static int getIntSize(int val)
{
    int i;
    /* negatives need sign extension.  this is a to do. */
    if(val<0) {
        return BytesPerWord;
    }

    if(val<LST_SMALL_TAG_LIMIT) {
        return 0;
    }

    /* how many bytes? */

    for(i=1; i<BytesPerWord; i++)
        if(val < (1<<(8*i))) {
            return i;
        }

    return BytesPerWord;
}



/* image file reading routines */


static void readTag(FILE *fp, int *type, int *val)
{
    int i;
    int tempSize;
    int inByte;

    inByte = fgetc(fp);

    if (inByte == EOF) {
        sysError("Unexpected EOF reading image file: reading tag byte.");
    }

    tempSize = (int)(inByte & LST_TAG_SIZE_MASK);
    *type = (int)(inByte & LST_TAG_TYPE_MASK);

    if(tempSize & LST_LARGE_TAG_FLAG) {
        /* large size, actual value is in succeeding
        bytes (tempSize bytes). The value is not sign
        extended. */
        *val = 0;

        /* get the number of bytes in the size field */
        tempSize = tempSize & LST_SMALL_TAG_LIMIT;

        if(tempSize>BytesPerWord) {
            sysError("Error reading image file: tag value field exceeds machine word size.  Image created on another machine?");
        }

        for(i=0; i<tempSize; i++) {
            inByte = fgetc(fp);

            if(inByte == EOF) {
                sysError("Unexpected EOF reading image file: reading extended value.");
            }

            *val = *val  | (((unsigned int)inByte & 0xFF) << (8*i));
        }
    } else {
        *val = tempSize;
    }
}


/**
* objectRead
*
* Read in an object from the input image file.  Several kinds of object are
* handled as special cases.  The routine readTag above does most of the work
* of figuring out what type of object it is an how big it is.
*/

struct object *objectRead(FILE *fp)
{
    int type;
    int size;
    int val;
    int i;
    struct object *newObj=(struct object *)0;
    struct byteObject *bnewObj;

    /* get the tag header for the object, this has a type and value */
    readTag(fp,&type,&val);

    switch(type) {
    case LST_ERROR_TYPE:    /* nil obj */
        sysErrorInt("Read in a null object", (intptr_t)newObj);

        break;

    case LST_OBJ_TYPE:  /* ordinary object */
        size = val;
        newObj = staticAllocate(size);
        indirArray[indirtop++] = newObj;
        newObj->class = objectRead(fp);
        for (i = 0; i < size; i++) {
            newObj->data[i] = objectRead(fp);
        }
        break;


    case LST_PINT_TYPE: /* positive integer */
        newObj = newInteger(val);
        break;

    case LST_NINT_TYPE: /* negative integer */
        newObj = newInteger(-val);
        break;

    case LST_BARRAY_TYPE:   /* byte arrays */
        size = val;
        newObj = staticIAllocate(size);
        indirArray[indirtop++] = newObj;
        bnewObj = (struct byteObject *) newObj;
        for (i = 0; i < size; i++) {
            /* TODO check for EOF! */
            bnewObj->bytes[i] = getc(fp);
        }

        bnewObj->class = objectRead(fp);
        break;

    case LST_POBJ_TYPE: /* previous object */
        if(val>indirtop) {
            sysErrorInt("Illegal previous object index",val);
        }

        newObj = indirArray[val];
        break;

    case LST_NIL_TYPE:  /* object 0 (nil object) */
        newObj = indirArray[0];
        break;

    default:
        sysErrorInt("Illegal tag type: ",type);
        break;
    }

    return newObj;
}








int fileIn(FILE *fp)
{
    int i;

    /* use the currently unused space for the indir pointers */
    if (inSpaceOne) {
        indirArray = (struct object * *) spaceTwo;
    } else {
        indirArray = (struct object * *) spaceOne;
    }
    indirtop = 0;

    /* read in the method from the image file */
    nilObject = objectRead(fp);
    trueObject = objectRead(fp);
    falseObject = objectRead(fp);
    globalsObject = objectRead(fp);
    SmallIntClass = objectRead(fp);
    IntegerClass = objectRead(fp);
    ArrayClass = objectRead(fp);
    BlockClass = objectRead(fp);
    ContextClass = objectRead(fp);
    initialMethod = objectRead(fp);
    for (i = 0; i < 3; i++) {
        binaryMessages[i] = objectRead(fp);
    }
    badMethodSym = objectRead(fp);

    /* clean up after ourselves.  KRH -- replace bzero(), it is deprecated. */
    memset((void *) indirArray,(int)0,(size_t)(spaceSize * sizeof(struct object)));

    return indirtop;
}


/**
* writeTag
*
* This write a special tag to the output file.  This tag has three bits
* for a type field and five bits for either a value or a size.
*/

static void writeTag(FILE *fp, int type, int val)
{
    int tempSize;
    int i;

    /* get the number of bytes required to store the value */
    tempSize = getIntSize(val);

    if(tempSize) {
        /*write the tag byte*/
        fputc((type|tempSize|LST_LARGE_TAG_FLAG),fp);

        for(i=0; i<tempSize; i++) {
            fputc((val>>(8*i)),fp);
        }
    } else {
        fputc((type|val),fp);
    }
}


/**
* objectWrite
*
* This routine writes an object to the output image file.
*/

static void objectWrite(FILE *fp, struct object *obj)
{
    int i;
    int size;
    int intVal;

    /* check for illegal object */
    if (obj == 0) {
        sysErrorInt("writing out a null object", (intptr_t)obj);
    }

    /* small integer?, if so, treat this specially as this is not a pointer */

    if (IS_SMALLINT(obj)) { /* SmallInt */
        intVal = integerValue(obj);

        /* if it is negative, we use the positive value and use a special tag. */
        if(intVal<0) {
            writeTag(fp,LST_NINT_TYPE,-intVal);
        } else {
            writeTag(fp,LST_PINT_TYPE,intVal);
        }
        return;
    }

    /* see if already written */
    for (i = 0; i < indirtop; i++)
        if (obj == indirArray[i]) {
            if (i == 0) {
                writeTag(fp,LST_NIL_TYPE,0);
            } else {
                writeTag(fp,LST_POBJ_TYPE,i);
            }
            return;
        }

    /* not written, do it now */
    indirArray[indirtop++] = obj;

    /* byte objects */
    if (obj->size & FLAG_BIN) {
        struct byteObject *bobj = (struct byteObject *) obj;
        size = SIZE(obj);

        /* write the header tag */
        writeTag(fp,LST_BARRAY_TYPE,size);

        /*write out bytes*/
        for(i=0; i<size; i++) {
            fputc(bobj->bytes[i],fp);
        }

        objectWrite(fp, obj->class);

        return;
    }

    /* ordinary objects */
    size = SIZE(obj);

    writeTag(fp,LST_OBJ_TYPE,size);

    /* write the class first */
    objectWrite(fp, obj->class);

    /* write the instance variables of the object */
    for (i = 0; i < size; i++) {
        objectWrite(fp, obj->data[i]);
    }
}





int fileOut(FILE *fp)
{
    int i;

    /* use the currently unused space for the indir pointers */
    if (inSpaceOne) {
        indirArray = (struct object * *) spaceTwo;
    } else {
        indirArray = (struct object * *) spaceOne;
    }
    indirtop = 0;

    /* write out the roots of the image file */
    objectWrite(fp, nilObject);
    objectWrite(fp, trueObject);
    objectWrite(fp, falseObject);
    objectWrite(fp, globalsObject);
    objectWrite(fp, SmallIntClass);
    objectWrite(fp, IntegerClass);
    objectWrite(fp, ArrayClass);
    objectWrite(fp, BlockClass);
    objectWrite(fp, ContextClass);
    objectWrite(fp, initialMethod);
    for (i = 0; i < 3; i++) {
        objectWrite(fp, binaryMessages[i]);
    }
    objectWrite(fp, badMethodSym);
    printf("%d objects written in image\n", indirtop);

    /* clean up after ourselves */
    memset((void *) indirArray, (int)0, (size_t)(spaceSize * sizeof(struct object)));
    return indirtop;
}

/*
 * addStaticRoot()
 *  Add another object root off a static object
 *
 * Static objects, in general, do not get garbage collected.  When
 * a static object is discovered adding a reference to a non-static
 * object, we link on the reference to our staticRoot table so we can
 * give it proper treatment during garbage collection.
 */
void addStaticRoot(struct object **objp)
{
    int i;

    for (i = 0; i < staticRootTop; ++i) {
        if (objp == staticRoots[i]) {
            return;
        }
    }
    if (staticRootTop >= STATICROOTLIMIT) {
        sysErrorInt("addStaticRoot: too many static references",
                    (intptr_t)objp);
    }
    staticRoots[staticRootTop++] = objp;
}

/*
 * map()
 *  Fix an OOP if needed, based on values to be exchanged
 */
static void map(struct object **oop, struct object *a1, struct object *a2, int size)
{
    int x;
    struct object *oo = *oop;

    for (x = 0; x < size; ++x) {
        if (a1->data[x] == oo) {
            *oop = a2->data[x];
            return;
        }
        if (a2->data[x] == oo) {
            *oop = a1->data[x];
            return;
        }
    }
}

/*
 * walk()
 *  Traverse an object space
 */
static void walk(struct object *base, struct object *top,
                 struct object *array1, struct object *array2, uint size)
{
    struct object *op, *opnext;
    uint x, sz;

    for (op = base; op < top; op = opnext) {
        /*
         * Re-map the class pointer, in case that's the
         * object which has been remapped.
         */
        map(&op->class, array1, array2, size);

        /*
         * Skip our argument arrays, since otherwise things
         * get rather circular.
         */
        sz = SIZE(op);
        if ((op == array1) || (op == array2)) {
            opnext = WORDSUP(op, sz + 2);
            continue;
        }

        /*
         * Don't have to worry about instance variables
         * if it's a binary format.
         */
        if (op->size & FLAG_BIN) {
            uint trueSize;

            /*
             * Skip size/class, and enough words to
             * contain the binary bytes.
             */
            trueSize = (sz + BytesPerWord - 1) / BytesPerWord;
            opnext = WORDSUP(op, trueSize + 2);
            continue;
        }

        /*
         * For each instance variable slot, fix up the pointer
         * if needed.
         */
        for (x = 0; x < sz; ++x) {
            map(&op->data[x], array1, array2, size);
        }

        /*
         * Walk past this object
         */
        opnext = WORDSUP(op, sz + 2);
    }
}

/*
 * exchangeObjects()
 *  Bulk exchange of object identities
 *
 * For each index to array1/array2, all references in current object
 * memory are modified so that references to the object in array1[]
 * become references to the corresponding object in array2[].  References
 * to the object in array2[] similarly become references to the
 * object in array1[].
 */
void exchangeObjects(struct object *array1, struct object *array2, uint size)
{
    uint x;

    /*
     * Convert our memory spaces
     */
    walk(memoryPointer, memoryTop, array1, array2, size);
    walk(staticPointer, staticTop, array1, array2, size);

    /*
     * Fix up the root pointers, too
     */
    for (x = 0; x < rootTop; x++) {
        map(&rootStack[x], array1, array2, size);
    }
    for (x = 0; x < staticRootTop; x++) {
        map(staticRoots[x], array1, array2, size);
    }
}





/* symbol table handling routines.  These use internal definitions that
should only be visible here. */

int symstrcomp(struct object *left, const char *right)
{
    int leftsize = SIZE(left);
    int rightsize = strlen(right);
    int minsize = leftsize < rightsize ? leftsize : rightsize;
    register int i;

    if (rightsize < minsize) {
        minsize = rightsize;
    }
    for (i = 0; i < minsize; i++) {
        if ((bytePtr(left)[i]) != (unsigned char)(right[i])) {
            return bytePtr(left)[i]-(unsigned char)(right[i]);
        }
    }
    return leftsize - rightsize;
}


int strsymcomp(const char *left, struct object *right)
{
    /* switch the sign of the result since the comparison
    is the other way */
    return -1 * symstrcomp(right,left);
}





/* Misc helpers */

int64_t time_usec()
{
    struct timeval tv;

    gettimeofday(&tv,NULL);

    return ((int64_t)1000000 * tv.tv_sec) + tv.tv_usec;
}
