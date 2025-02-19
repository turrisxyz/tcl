/*
 * tclListObj.c --
 *
 *	This file contains functions that implement the Tcl list object type.
 *
 * Copyright © 1995-1997 Sun Microsystems, Inc.
 * Copyright © 1998 Scriptics Corporation.
 * Copyright © 2001 Kevin B. Kenny.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tclInt.h"
#include <assert.h>

/*
 * Prototypes for functions defined later in this file:
 */

static List *		AttemptNewList(Tcl_Interp *interp, int objc,
			    Tcl_Obj *const objv[]);
static List *		NewListIntRep(int objc, Tcl_Obj *const objv[], int p);
static void		DupListInternalRep(Tcl_Obj *srcPtr, Tcl_Obj *copyPtr);
static void		FreeListInternalRep(Tcl_Obj *listPtr);
static int		SetListFromAny(Tcl_Interp *interp, Tcl_Obj *objPtr);
static void		UpdateStringOfList(Tcl_Obj *listPtr);

/*
 * The structure below defines the list Tcl object type by means of functions
 * that can be invoked by generic object code.
 *
 * The internal representation of a list object is a two-pointer
 * representation. The first pointer designates a List structure that contains
 * an array of pointers to the element objects, together with integers that
 * represent the current element count and the allocated size of the array.
 * The second pointer is normally NULL; during execution of functions in this
 * file that operate on nested sublists, it is occasionally used as working
 * storage to avoid an auxiliary stack.
 */

const Tcl_ObjType tclListType = {
    "list",			/* name */
    FreeListInternalRep,	/* freeIntRepProc */
    DupListInternalRep,		/* dupIntRepProc */
    UpdateStringOfList,		/* updateStringProc */
    SetListFromAny		/* setFromAnyProc */
};

/* Macros to manipulate the List internal rep */

#define ListSetIntRep(objPtr, listRepPtr)				\
    do {								\
	Tcl_ObjInternalRep ir;						\
	ir.twoPtrValue.ptr1 = (listRepPtr);				\
	ir.twoPtrValue.ptr2 = NULL;					\
	(listRepPtr)->refCount++;					\
	Tcl_StoreInternalRep((objPtr), &tclListType, &ir);			\
    } while (0)

#define ListGetIntRep(objPtr, listRepPtr)				\
    do {								\
	const Tcl_ObjInternalRep *irPtr;					\
	irPtr = TclFetchInternalRep((objPtr), &tclListType);		\
	(listRepPtr) = irPtr ? (List *)irPtr->twoPtrValue.ptr1 : NULL;		\
    } while (0)

#define ListResetIntRep(objPtr, listRepPtr) \
    TclFetchInternalRep((objPtr), &tclListType)->twoPtrValue.ptr1 = (listRepPtr)

#ifndef TCL_MIN_ELEMENT_GROWTH
#define TCL_MIN_ELEMENT_GROWTH TCL_MIN_GROWTH/sizeof(Tcl_Obj *)
#endif

/*
 *----------------------------------------------------------------------
 *
 * NewListIntRep --
 *
 *	Creates a 'List' structure with space for 'objc' elements.  'objc' must
 *	be > 0.  If 'objv' is not NULL, The list is initialized with first
 *	'objc' values in that array.  Otherwise the list is initialized to have
 *	0 elements, with space to add 'objc' more.  Flag value 'p' indicates
 *	how to behave on failure.
 *
 * Value
 *
 *	A new 'List' structure with refCount 0. If some failure
 *	prevents this NULL is returned if 'p' is 0 , and 'Tcl_Panic'
 *	is called if it is not.
 *
 * Effect
 *
 *	The refCount of each value in 'objv' is incremented as it is added
 *	to the list.
 *
 *----------------------------------------------------------------------
 */

static List *
NewListIntRep(
    int objc,
    Tcl_Obj *const objv[],
    int p)
{
    List *listRepPtr;

    if (objc <= 0) {
	Tcl_Panic("NewListIntRep: expects postive element count");
    }

    /*
     * First check to see if we'd overflow and try to allocate an object
     * larger than our memory allocator allows. Note that this is actually a
     * fairly small value when you're on a serious 64-bit machine, but that
     * requires API changes to fix. See [Bug 219196] for a discussion.
     */

    if ((size_t)objc > LIST_MAX) {
	if (p) {
	    Tcl_Panic("max length of a Tcl list (%d elements) exceeded",
		    LIST_MAX);
	}
	return NULL;
    }

    listRepPtr = (List *)Tcl_AttemptAlloc(LIST_SIZE(objc));
    if (listRepPtr == NULL) {
	if (p) {
	    Tcl_Panic("list creation failed: unable to alloc %" TCL_Z_MODIFIER "u bytes",
		    LIST_SIZE(objc));
	}
	return NULL;
    }

    listRepPtr->canonicalFlag = 0;
    listRepPtr->refCount = 0;
    listRepPtr->maxElemCount = objc;

    if (objv) {
	Tcl_Obj **elemPtrs;
	int i;

	listRepPtr->elemCount = objc;
	elemPtrs = &listRepPtr->elements;
	for (i = 0;  i < objc;  i++) {
	    elemPtrs[i] = objv[i];
	    Tcl_IncrRefCount(elemPtrs[i]);
	}
    } else {
	listRepPtr->elemCount = 0;
    }
    return listRepPtr;
}

/*
 *----------------------------------------------------------------------
 *
 *  AttemptNewList --
 *
 *	Like NewListIntRep, but additionally sets an error message on failure.
 *
 *----------------------------------------------------------------------
 */

static List *
AttemptNewList(
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    List *listRepPtr = NewListIntRep(objc, objv, 0);

    if (interp != NULL && listRepPtr == NULL) {
	if (objc > LIST_MAX) {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "max length of a Tcl list (%d elements) exceeded",
		    LIST_MAX));
	} else {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "list creation failed: unable to alloc %" TCL_Z_MODIFIER "u bytes",
		    LIST_SIZE(objc)));
	}
	Tcl_SetErrorCode(interp, "TCL", "MEMORY", NULL);
    }
    return listRepPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_NewListObj --
 *
 *	Creates a new list object and adds values to it. When TCL_MEM_DEBUG is
 *	defined, 'Tcl_DbNewListObj' is called instead.
 *
 * Value
 *
 *	A new list 'Tcl_Obj' to which is appended values from 'objv', or if
 *	'objc' is less than or equal to zero, a list 'Tcl_Obj' having no
 *	elements.  The string representation of the new 'Tcl_Obj' is set to
 *	NULL.  The refCount of the list is 0.
 *
 * Effect
 *
 *	The refCount of each elements in 'objv' is incremented as it is added
 *	to the list.
 *
 *----------------------------------------------------------------------
 */

#ifdef TCL_MEM_DEBUG
#undef Tcl_NewListObj

Tcl_Obj *
Tcl_NewListObj(
    int objc,			/* Count of objects referenced by objv. */
    Tcl_Obj *const objv[])	/* An array of pointers to Tcl objects. */
{
    return Tcl_DbNewListObj(objc, objv, "unknown", 0);
}

#else /* if not TCL_MEM_DEBUG */

Tcl_Obj *
Tcl_NewListObj(
    int objc,			/* Count of objects referenced by objv. */
    Tcl_Obj *const objv[])	/* An array of pointers to Tcl objects. */
{
    List *listRepPtr;
    Tcl_Obj *listPtr;

    TclNewObj(listPtr);

    if (objc <= 0) {
	return listPtr;
    }

    /*
     * Create the internal rep.
     */

    listRepPtr = NewListIntRep(objc, objv, 1);

    /*
     * Now create the object.
     */

    TclInvalidateStringRep(listPtr);
    ListSetIntRep(listPtr, listRepPtr);
    return listPtr;
}
#endif /* if TCL_MEM_DEBUG */

/*
 *----------------------------------------------------------------------
 *
 *  Tcl_DbNewListObj --
 *
 *	Like 'Tcl_NewListObj', but it calls Tcl_DbCkalloc directly with the
 *	file name and line number from its caller.  This simplifies debugging
 *	since the [memory active] command will report the correct file
 *	name and line number when reporting objects that haven't been freed.
 *
 *	When TCL_MEM_DEBUG is not defined, 'Tcl_NewListObj' is called instead.
 *
 *----------------------------------------------------------------------
 */

#ifdef TCL_MEM_DEBUG

Tcl_Obj *
Tcl_DbNewListObj(
    int objc,			/* Count of objects referenced by objv. */
    Tcl_Obj *const objv[],	/* An array of pointers to Tcl objects. */
    const char *file,		/* The name of the source file calling this
				 * function; used for debugging. */
    int line)			/* Line number in the source file; used for
				 * debugging. */
{
    Tcl_Obj *listPtr;
    List *listRepPtr;

    TclDbNewObj(listPtr, file, line);

    if (objc <= 0) {
	return listPtr;
    }

    /*
     * Create the internal rep.
     */

    listRepPtr = NewListIntRep(objc, objv, 1);

    /*
     * Now create the object.
     */

    TclInvalidateStringRep(listPtr);
    ListSetIntRep(listPtr, listRepPtr);

    return listPtr;
}

#else /* if not TCL_MEM_DEBUG */

Tcl_Obj *
Tcl_DbNewListObj(
    int objc,			/* Count of objects referenced by objv. */
    Tcl_Obj *const objv[],	/* An array of pointers to Tcl objects. */
    TCL_UNUSED(const char *) /*file*/,
    TCL_UNUSED(int) /*line*/)
{
    return Tcl_NewListObj(objc, objv);
}
#endif /* TCL_MEM_DEBUG */

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetListObj --
 *
 *	Like 'Tcl_NewListObj', but operates on an existing 'Tcl_Obj'instead of
 *	creating a new one.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_SetListObj(
    Tcl_Obj *objPtr,		/* Object whose internal rep to init. */
    int objc,			/* Count of objects referenced by objv. */
    Tcl_Obj *const objv[])	/* An array of pointers to Tcl objects. */
{
    List *listRepPtr;

    if (Tcl_IsShared(objPtr)) {
	Tcl_Panic("%s called with shared object", "Tcl_SetListObj");
    }

    /*
     * Free any old string rep and any internal rep for the old type.
     */

    TclFreeInternalRep(objPtr);
    TclInvalidateStringRep(objPtr);

    /*
     * Set the object's type to "list" and initialize the internal rep.
     * However, if there are no elements to put in the list, just give the
     * object an empty string rep and a NULL type.
     */

    if (objc > 0) {
	listRepPtr = NewListIntRep(objc, objv, 1);
	ListSetIntRep(objPtr, listRepPtr);
    } else {
	Tcl_InitStringRep(objPtr, NULL, 0);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TclListObjCopy --
 *
 *	Creates a new 'Tcl_Obj' which is a pure copy of a list value. This
 *	provides for the C level a counterpart of the [lrange $list 0 end]
 *	command, while using internals details to be as efficient as possible.
 *
 * Value
 *
 *	The address of the new 'Tcl_Obj' which shares its internal
 *	representation with 'listPtr', and whose refCount is 0.  If 'listPtr'
 *	is not actually a list, the value is NULL, and an error message is left
 *	in 'interp' if it is not NULL.
 *
 * Effect
 *
 *	'listPtr' is converted to a list if it isn't one already.
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj *
TclListObjCopy(
    Tcl_Interp *interp,		/* Used to report errors if not NULL. */
    Tcl_Obj *listPtr)		/* List object for which an element array is
				 * to be returned. */
{
    Tcl_Obj *copyPtr;
    List *listRepPtr;

    ListGetIntRep(listPtr, listRepPtr);
    if (NULL == listRepPtr) {
	if (SetListFromAny(interp, listPtr) != TCL_OK) {
	    return NULL;
	}
    }

    TclNewObj(copyPtr);
    TclInvalidateStringRep(copyPtr);
    DupListInternalRep(listPtr, copyPtr);
    return copyPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TclListObjRange --
 *
 *	Makes a slice of a list value.
 *      *listPtr must be known to be a valid list.
 *
 * Results:
 *	Returns a pointer to the sliced list.
 *      This may be a new object or the same object if not shared.
 *
 * Side effects:
 *	The possible conversion of the object referenced by listPtr
 *	to a list object.
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj *
TclListObjRange(
    Tcl_Obj *listPtr,		/* List object to take a range from. */
    size_t fromIdx,		/* Index of first element to include. */
    size_t toIdx)			/* Index of last element to include. */
{
    Tcl_Obj **elemPtrs;
    int listLen;
    size_t i, newLen;
    List *listRepPtr;

    TclListObjGetElementsM(NULL, listPtr, &listLen, &elemPtrs);

    if (fromIdx == TCL_INDEX_NONE) {
	fromIdx = 0;
    }
    if (toIdx + 1 >= (size_t)listLen + 1) {
	toIdx = listLen-1;
    }
    if (fromIdx + 1 > toIdx + 1) {
	Tcl_Obj *obj;
	TclNewObj(obj);
	return obj;
    }

    newLen = toIdx - fromIdx + 1;

    if (Tcl_IsShared(listPtr) ||
	    ((ListRepPtr(listPtr)->refCount > 1))) {
	return Tcl_NewListObj(newLen, &elemPtrs[fromIdx]);
    }

    /*
     * In-place is possible.
     */

    /*
     * Even if nothing below cause any changes, we still want the
     * string-canonizing effect of [lrange 0 end].
     */

    TclInvalidateStringRep(listPtr);

    /*
     * Delete elements that should not be included.
     */

    for (i = 0; i < fromIdx; i++) {
	TclDecrRefCount(elemPtrs[i]);
    }
    for (i = toIdx + 1; i < (size_t)listLen; i++) {
	TclDecrRefCount(elemPtrs[i]);
    }

    if (fromIdx > 0) {
	memmove(elemPtrs, &elemPtrs[fromIdx],
		(size_t) newLen * sizeof(Tcl_Obj*));
    }

    listRepPtr = ListRepPtr(listPtr);
    listRepPtr->elemCount = newLen;

    return listPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ListObjGetElements --
 *
 *	Retreive the elements in a list 'Tcl_Obj'.
 *
 * Value
 *
 *	TCL_OK
 *
 *	    A count of list elements is stored, 'objcPtr', And a pointer to the
 *	    array of elements in the list is stored in 'objvPtr'.
 *
 *	    The elements accessible via 'objvPtr' should be treated as readonly
 *	    and the refCount for each object is _not_ incremented; the caller
 *	    must do that if it holds on to a reference. Furthermore, the
 *	    pointer and length returned by this function may change as soon as
 *	    any function is called on the list object. Be careful about
 *	    retaining the pointer in a local data structure.
 *
 *	TCL_ERROR
 *
 *	    'listPtr' is not a valid list. An error message is left in the
 *	    interpreter's result if 'interp' is not NULL.
 *
 * Effect
 *
 *	'listPtr' is converted to a list object if it isn't one already.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_ListObjGetElements(
    Tcl_Interp *interp,		/* Used to report errors if not NULL. */
    Tcl_Obj *listPtr,	/* List object for which an element array is
				 * to be returned. */
    int *objcPtr,		/* Where to store the count of objects
				 * referenced by objv. */
    Tcl_Obj ***objvPtr)		/* Where to store the pointer to an array of
				 * pointers to the list's objects. */
{
    List *listRepPtr;

    ListGetIntRep(listPtr, listRepPtr);

    if (listRepPtr == NULL) {
	int result;
	size_t length;

	(void) Tcl_GetStringFromObj(listPtr, &length);
	if (length == 0) {
	    *objcPtr = 0;
	    *objvPtr = NULL;
	    return TCL_OK;
	}
	result = SetListFromAny(interp, listPtr);
	if (result != TCL_OK) {
	    return result;
	}
	ListGetIntRep(listPtr, listRepPtr);
    }
    *objcPtr = listRepPtr->elemCount;
    *objvPtr = &listRepPtr->elements;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ListObjAppendList --
 *
 *	Appends the elements of elemListPtr to those of listPtr.
 *
 * Value
 *
 *	TCL_OK
 *
 *	    Success.
 *
 *	TCL_ERROR
 *
 *	    'listPtr' or 'elemListPtr' are not valid lists.  An error
 *	    message is left in the interpreter's result if 'interp' is not NULL.
 *
 * Effect
 *
 *	The reference count of each element of 'elemListPtr' as it is added to
 *	'listPtr'. 'listPtr' and 'elemListPtr' are converted to 'tclListType'
 *	if they are not already. Appending the new elements may cause the
 *	array of element pointers in 'listObj' to grow.  If any objects are
 *	appended to 'listPtr'. Any preexisting string representation of
 *	'listPtr' is invalidated.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_ListObjAppendList(
    Tcl_Interp *interp,		/* Used to report errors if not NULL. */
    Tcl_Obj *listPtr,	/* List object to append elements to. */
    Tcl_Obj *elemListPtr)	/* List obj with elements to append. */
{
    int objc;
    Tcl_Obj **objv;

    if (Tcl_IsShared(listPtr)) {
	Tcl_Panic("%s called with shared object", "Tcl_ListObjAppendList");
    }

    /*
     * Pull the elements to append from elemListPtr.
     */

    if (TCL_OK != TclListObjGetElementsM(interp, elemListPtr, &objc, &objv)) {
	return TCL_ERROR;
    }

    /*
     * Insert the new elements starting after the lists's last element.
     * Delete zero existing elements.
     */

    return Tcl_ListObjReplace(interp, listPtr, LIST_MAX, 0, objc, objv);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ListObjAppendElement --
 *
 *	Like 'Tcl_ListObjAppendList', but Appends a single value to a list.
 *
 * Value
 *
 *	TCL_OK
 *
 *	    'objPtr' is appended to the elements of 'listPtr'.
 *
 *	TCL_ERROR
 *
 *	    listPtr does not refer to a list object and the object can not be
 *	    converted to one. An error message will be left in the
 *	    interpreter's result if interp is not NULL.
 *
 * Effect
 *
 *	If 'listPtr' is not already of type 'tclListType', it is converted.
 *	The 'refCount' of 'objPtr' is incremented as it is added to 'listPtr'.
 *	Appending the new element may cause the the array of element pointers
 *	in 'listObj' to grow.  Any preexisting string representation of
 *	'listPtr' is invalidated.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_ListObjAppendElement(
    Tcl_Interp *interp,		/* Used to report errors if not NULL. */
    Tcl_Obj *listPtr,		/* List object to append objPtr to. */
    Tcl_Obj *objPtr)		/* Object to append to listPtr's list. */
{
    List *listRepPtr, *newPtr = NULL;
    int numElems, numRequired, needGrow, isShared, attempt;

    if (Tcl_IsShared(listPtr)) {
	Tcl_Panic("%s called with shared object", "Tcl_ListObjAppendElement");
    }

    ListGetIntRep(listPtr, listRepPtr);
    if (listRepPtr == NULL) {
	int result;
	size_t length;

	(void) Tcl_GetStringFromObj(listPtr, &length);
	if (length == 0) {
	    Tcl_SetListObj(listPtr, 1, &objPtr);
	    return TCL_OK;
	}
	result = SetListFromAny(interp, listPtr);
	if (result != TCL_OK) {
	    return result;
	}
	ListGetIntRep(listPtr, listRepPtr);
    }

    numElems = listRepPtr->elemCount;
    numRequired = numElems + 1 ;
    needGrow = (numRequired > listRepPtr->maxElemCount);
    isShared = (listRepPtr->refCount > 1);

    if (numRequired > LIST_MAX) {
	if (interp != NULL) {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "max length of a Tcl list (%d elements) exceeded",
		    LIST_MAX));
	    Tcl_SetErrorCode(interp, "TCL", "MEMORY", NULL);
	}
	return TCL_ERROR;
    }

    if (needGrow && !isShared) {
	/*
	 * Need to grow + unshared internalrep => try to realloc
	 */

	attempt = 2 * numRequired;
	if (attempt <= LIST_MAX) {
	    newPtr = (List *)Tcl_AttemptRealloc(listRepPtr, LIST_SIZE(attempt));
	}
	if (newPtr == NULL) {
	    attempt = numRequired + 1 + TCL_MIN_ELEMENT_GROWTH;
	    if (attempt > LIST_MAX) {
		attempt = LIST_MAX;
	    }
	    newPtr = (List *)Tcl_AttemptRealloc(listRepPtr, LIST_SIZE(attempt));
	}
	if (newPtr == NULL) {
	    attempt = numRequired;
	    newPtr = (List *)Tcl_AttemptRealloc(listRepPtr, LIST_SIZE(attempt));
	}
	if (newPtr) {
	    listRepPtr = newPtr;
	    listRepPtr->maxElemCount = attempt;
	    needGrow = 0;
	}
    }
    if (isShared || needGrow) {
	Tcl_Obj **dst, **src = &listRepPtr->elements;

	/*
	 * Either we have a shared internalrep and we must copy to write, or we
	 * need to grow and realloc attempts failed.  Attempt internalrep copy.
	 */

	attempt = 2 * numRequired;
	newPtr = AttemptNewList(NULL, attempt, NULL);
	if (newPtr == NULL) {
	    attempt = numRequired + 1 + TCL_MIN_ELEMENT_GROWTH;
	    if (attempt > LIST_MAX) {
		attempt = LIST_MAX;
	    }
	    newPtr = AttemptNewList(NULL, attempt, NULL);
	}
	if (newPtr == NULL) {
	    attempt = numRequired;
	    newPtr = AttemptNewList(interp, attempt, NULL);
	}
	if (newPtr == NULL) {
	    /*
	     * All growth attempts failed; throw the error.
	     */

	    return TCL_ERROR;
	}

	dst = &newPtr->elements;
	newPtr->refCount++;
	newPtr->canonicalFlag = listRepPtr->canonicalFlag;
	newPtr->elemCount = listRepPtr->elemCount;

	if (isShared) {
	    /*
	     * The original internalrep must remain undisturbed.  Copy into the new
	     * one and bump refcounts
	     */
	    while (numElems--) {
		*dst = *src++;
		Tcl_IncrRefCount(*dst++);
	    }
	    listRepPtr->refCount--;
	} else {
	    /*
	     * Old internalrep to be freed, re-use refCounts.
	     */

	    memcpy(dst, src, numElems * sizeof(Tcl_Obj *));
	    Tcl_Free(listRepPtr);
	}
	listRepPtr = newPtr;
    }
    ListResetIntRep(listPtr, listRepPtr);
    listRepPtr->refCount++;
    TclFreeInternalRep(listPtr);
    ListSetIntRep(listPtr, listRepPtr);
    listRepPtr->refCount--;

    /*
     * Add objPtr to the end of listPtr's array of element pointers. Increment
     * the ref count for the (now shared) objPtr.
     */

    *(&listRepPtr->elements + listRepPtr->elemCount) = objPtr;
    Tcl_IncrRefCount(objPtr);
    listRepPtr->elemCount++;

    /*
     * Invalidate any old string representation since the list's internal
     * representation has changed.
     */

    TclInvalidateStringRep(listPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ListObjIndex --
 *
 * 	Retrieve a pointer to the element of 'listPtr' at 'index'.  The index
 * 	of the first element is 0.
 *
 * Value
 *
 * 	TCL_OK
 *
 *	    A pointer to the element at 'index' is stored in 'objPtrPtr'.  If
 *	    'index' is out of range, NULL is stored in 'objPtrPtr'.  This
 *	    object should be treated as readonly and its 'refCount' is _not_
 *	    incremented. The caller must do that if it holds on to the
 *	    reference.
 *
 * 	TCL_ERROR
 *
 * 	    'listPtr' is not a valid list. An an error message is left in the
 * 	    interpreter's result if 'interp' is not NULL.
 *
 *  Effect
 *
 * 	If 'listPtr' is not already of type 'tclListType', it is converted.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_ListObjIndex(
    Tcl_Interp *interp,		/* Used to report errors if not NULL. */
    Tcl_Obj *listPtr,	/* List object to index into. */
    int index,		/* Index of element to return. */
    Tcl_Obj **objPtrPtr)	/* The resulting Tcl_Obj* is stored here. */
{
    List *listRepPtr;

    ListGetIntRep(listPtr, listRepPtr);
    if (listRepPtr == NULL) {
	int result;
	size_t length;

	(void) Tcl_GetStringFromObj(listPtr, &length);
	if (length == 0) {
	    *objPtrPtr = NULL;
	    return TCL_OK;
	}
	result = SetListFromAny(interp, listPtr);
	if (result != TCL_OK) {
	    return result;
	}
	ListGetIntRep(listPtr, listRepPtr);
    }

    if ((index < 0) || (index >= listRepPtr->elemCount)) {
	*objPtrPtr = NULL;
    } else {
	*objPtrPtr = (&listRepPtr->elements)[index];
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ListObjLength --
 *
 * 	Retrieve the number of elements in a list.
 *
 * Value
 *
 *	TCL_OK
 *
 *	    A count of list elements is stored at the address provided by
 *	    'intPtr'. If 'listPtr' is not already of type 'tclListPtr', it is
 *	    converted.
 *
 *	TCL_ERROR
 *
 *	    'listPtr' is not a valid list.  An error message will be left in
 *	    the interpreter's result if 'interp' is not NULL.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_ListObjLength(
    Tcl_Interp *interp,		/* Used to report errors if not NULL. */
    Tcl_Obj *listPtr,	/* List object whose #elements to return. */
    int *intPtr)	/* The resulting int is stored here. */
{
    List *listRepPtr;

    ListGetIntRep(listPtr, listRepPtr);
    if (listRepPtr == NULL) {
	int result;
	size_t length;

	(void) Tcl_GetStringFromObj(listPtr, &length);
	if (length == 0) {
	    *intPtr = 0;
	    return TCL_OK;
	}
	result = SetListFromAny(interp, listPtr);
	if (result != TCL_OK) {
	    return result;
	}
	ListGetIntRep(listPtr, listRepPtr);
    }

    *intPtr = listRepPtr->elemCount;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ListObjReplace --
 *
 *	Replace values in a list.
 *
 *	If 'first' is zero or negative, it refers to the first element. If
 *	'first' outside the range of elements in the list, no elements are
 *	deleted.
 *
 *	If 'count' is zero or negative no elements are deleted, and any new
 *	elements are inserted at the beginning of the list.
 *
 * Value
 *
 *	TCL_OK
 *
 *	    The first 'objc' values of 'objv' replaced 'count' elements in 'listPtr'
 *	    starting at 'first'.  If 'objc' 0, no new elements are added.
 *
 *	TCL_ERROR
 *
 *	    'listPtr' is not a valid list.   An error message is left in the
 *	    interpreter's result if 'interp' is not NULL.
 *
 * Effect
 *
 *	If 'listPtr' is not of type 'tclListType', it is converted if possible.
 *
 *	The 'refCount' of each element appended to the list is incremented.
 *	Similarly, the 'refCount' for each replaced element is decremented.
 *
 *	If 'listPtr' is modified, any previous string representation is
 *	invalidated.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_ListObjReplace(
    Tcl_Interp *interp,		/* Used for error reporting if not NULL. */
    Tcl_Obj *listPtr,		/* List object whose elements to replace. */
    int first,			/* Index of first element to replace. */
    int count,			/* Number of elements to replace. */
    int objc,			/* Number of objects to insert. */
    Tcl_Obj *const objv[])	/* An array of objc pointers to Tcl objects to
				 * insert. */
{
    List *listRepPtr;
    Tcl_Obj **elemPtrs;
    int needGrow, numElems, numRequired, numAfterLast, start, i, j, isShared;

    if (Tcl_IsShared(listPtr)) {
	Tcl_Panic("%s called with shared object", "Tcl_ListObjReplace");
    }

    ListGetIntRep(listPtr, listRepPtr);
    if (listRepPtr == NULL) {
	size_t length;

	(void) Tcl_GetStringFromObj(listPtr, &length);
	if (length == 0) {
	    if (objc == 0) {
		return TCL_OK;
	    }
	    Tcl_SetListObj(listPtr, objc, NULL);
	} else {
	    int result = SetListFromAny(interp, listPtr);

	    if (result != TCL_OK) {
		return result;
	    }
	}
	ListGetIntRep(listPtr, listRepPtr);
    }

    /*
     * Note that when count == 0 and objc == 0, this routine is logically a
     * no-op, removing and adding no elements to the list. However, by flowing
     * through this routine anyway, we get the important side effect that the
     * resulting listPtr is a list in canoncial form. This is important.
     * Resist any temptation to optimize this case.
     */

    elemPtrs = &listRepPtr->elements;
    numElems = listRepPtr->elemCount;

    if (first < 0) {
	first = 0;
    }
    if (first >= numElems) {
	first = numElems;	/* So we'll insert after last element. */
    }
    if (count < 0) {
	count = 0;
    } else if (first > INT_MAX - count /* Handle integer overflow */
	    || numElems < first+count) {

	count = numElems - first;
    }

    if (objc > LIST_MAX - (numElems - count)) {
	if (interp != NULL) {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "max length of a Tcl list (%d elements) exceeded",
		    LIST_MAX));
	}
	return TCL_ERROR;
    }
    isShared = (listRepPtr->refCount > 1);
    numRequired = numElems - count + objc; /* Known <= LIST_MAX */
    needGrow = numRequired > listRepPtr->maxElemCount;

    for (i = 0;  i < objc;  i++) {
	Tcl_IncrRefCount(objv[i]);
    }

    if (needGrow && !isShared) {
	/* Try to use realloc */
	List *newPtr = NULL;
	int attempt = 2 * numRequired;
	if (attempt <= LIST_MAX) {
	    newPtr = (List *)Tcl_AttemptRealloc(listRepPtr, LIST_SIZE(attempt));
	}
	if (newPtr == NULL) {
	    attempt = numRequired + 1 + TCL_MIN_ELEMENT_GROWTH;
	    if (attempt > LIST_MAX) {
		attempt = LIST_MAX;
	    }
	    newPtr = (List *)Tcl_AttemptRealloc(listRepPtr, LIST_SIZE(attempt));
	}
	if (newPtr == NULL) {
	    attempt = numRequired;
	    newPtr = (List *)Tcl_AttemptRealloc(listRepPtr, LIST_SIZE(attempt));
	}
	if (newPtr) {
	    listRepPtr = newPtr;
	    ListResetIntRep(listPtr, listRepPtr);
	    elemPtrs = &listRepPtr->elements;
	    listRepPtr->maxElemCount = attempt;
	    needGrow = numRequired > listRepPtr->maxElemCount;
	}
    }
    if (!needGrow && !isShared) {
	int shift;

	/*
	 * Can use the current List struct. First "delete" count elements
	 * starting at first.
	 */

	for (j = first;  j < first + count;  j++) {
	    Tcl_Obj *victimPtr = elemPtrs[j];

	    TclDecrRefCount(victimPtr);
	}

	/*
	 * Shift the elements after the last one removed to their new
	 * locations.
	 */

	start = first + count;
	numAfterLast = numElems - start;
	shift = objc - count;	/* numNewElems - numDeleted */
	if ((numAfterLast > 0) && (shift != 0)) {
	    Tcl_Obj **src = elemPtrs + start;

	    memmove(src+shift, src, numAfterLast * sizeof(Tcl_Obj*));
	}
    } else {
	/*
	 * Cannot use the current List struct; it is shared, too small, or
	 * both. Allocate a new struct and insert elements into it.
	 */

	List *oldListRepPtr = listRepPtr;
	Tcl_Obj **oldPtrs = elemPtrs;
	int newMax;

	if (needGrow) {
	    newMax = 2 * numRequired;
	} else {
	    newMax = listRepPtr->maxElemCount;
	}

	listRepPtr = AttemptNewList(NULL, newMax, NULL);
	if (listRepPtr == NULL) {
	    unsigned int limit = LIST_MAX - numRequired;
	    unsigned int extra = numRequired - numElems
		    + TCL_MIN_ELEMENT_GROWTH;
	    int growth = (int) ((extra > limit) ? limit : extra);

	    listRepPtr = AttemptNewList(NULL, numRequired + growth, NULL);
	    if (listRepPtr == NULL) {
		listRepPtr = AttemptNewList(interp, numRequired, NULL);
		if (listRepPtr == NULL) {
		    for (i = 0;  i < objc;  i++) {
			/* See bug 3598580 */
			Tcl_DecrRefCount(objv[i]);
		    }
		    return TCL_ERROR;
		}
	    }
	}

	ListResetIntRep(listPtr, listRepPtr);
	listRepPtr->refCount++;

	elemPtrs = &listRepPtr->elements;

	if (isShared) {
	    /*
	     * The old struct will remain in place; need new refCounts for the
	     * new List struct references. Copy over only the surviving
	     * elements.
	     */

	    for (i=0; i < first; i++) {
		elemPtrs[i] = oldPtrs[i];
		Tcl_IncrRefCount(elemPtrs[i]);
	    }
	    for (i = first + count, j = first + objc;
		    j < numRequired; i++, j++) {
		elemPtrs[j] = oldPtrs[i];
		Tcl_IncrRefCount(elemPtrs[j]);
	    }

	    oldListRepPtr->refCount--;
	} else {
	    /*
	     * The old struct will be removed; use its inherited refCounts.
	     */

	    if (first > 0) {
		memcpy(elemPtrs, oldPtrs, first * sizeof(Tcl_Obj *));
	    }

	    /*
	     * "Delete" count elements starting at first.
	     */

	    for (j = first;  j < first + count;  j++) {
		Tcl_Obj *victimPtr = oldPtrs[j];

		TclDecrRefCount(victimPtr);
	    }

	    /*
	     * Copy the elements after the last one removed, shifted to their
	     * new locations.
	     */

	    start = first + count;
	    numAfterLast = numElems - start;
	    if (numAfterLast > 0) {
		memcpy(elemPtrs + first + objc, oldPtrs + start,
			(size_t) numAfterLast * sizeof(Tcl_Obj *));
	    }

	    Tcl_Free(oldListRepPtr);
	}
    }

    /*
     * Insert the new elements into elemPtrs before "first".
     */

    for (i=0,j=first ; i<objc ; i++,j++) {
	elemPtrs[j] = objv[i];
    }

    /*
     * Update the count of elements.
     */

    listRepPtr->elemCount = numRequired;

    /*
     * Invalidate and free any old representations that may not agree
     * with the revised list's internal representation.
     */

    listRepPtr->refCount++;
    TclFreeInternalRep(listPtr);
    ListSetIntRep(listPtr, listRepPtr);
    listRepPtr->refCount--;

    TclInvalidateStringRep(listPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclLindexList --
 *
 *	Implements the 'lindex' command when objc==3.
 *
 *	Implemented entirely as a wrapper around 'TclLindexFlat'. Reconfigures
 *	the argument format into required form while taking care to manage
 *	shimmering so as to tend to keep the most useful internalreps
 *	and/or avoid the most expensive conversions.
 *
 * Value
 *
 *	A pointer to the specified element, with its 'refCount' incremented, or
 *	NULL if an error occurred.
 *
 * Notes
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj *
TclLindexList(
    Tcl_Interp *interp,		/* Tcl interpreter. */
    Tcl_Obj *listPtr,		/* List being unpacked. */
    Tcl_Obj *argPtr)		/* Index or index list. */
{

    size_t index;			/* Index into the list. */
    Tcl_Obj *indexListCopy;
    List *listRepPtr;

    /*
     * Determine whether argPtr designates a list or a single index. We have
     * to be careful about the order of the checks to avoid repeated
     * shimmering; see TIP#22 and TIP#33 for the details.
     */

    ListGetIntRep(argPtr, listRepPtr);
    if ((listRepPtr == NULL)
	    && TclGetIntForIndexM(NULL , argPtr, (size_t)WIDE_MAX - 1, &index) == TCL_OK) {
	/*
	 * argPtr designates a single index.
	 */

	return TclLindexFlat(interp, listPtr, 1, &argPtr);
    }

    /*
     * Here we make a private copy of the index list argument to avoid any
     * shimmering issues that might invalidate the indices array below while
     * we are still using it. This is probably unnecessary. It does not appear
     * that any damaging shimmering is possible, and no test has been devised
     * to show any error when this private copy is not made. But it's cheap,
     * and it offers some future-proofing insurance in case the TclLindexFlat
     * implementation changes in some unexpected way, or some new form of
     * trace or callback permits things to happen that the current
     * implementation does not.
     */

    indexListCopy = TclListObjCopy(NULL, argPtr);
    if (indexListCopy == NULL) {
	/*
	 * argPtr designates something that is neither an index nor a
	 * well-formed list. Report the error via TclLindexFlat.
	 */

	return TclLindexFlat(interp, listPtr, 1, &argPtr);
    }

    ListGetIntRep(indexListCopy, listRepPtr);

    assert(listRepPtr != NULL);

    listPtr = TclLindexFlat(interp, listPtr, listRepPtr->elemCount,
		&listRepPtr->elements);
    Tcl_DecrRefCount(indexListCopy);
    return listPtr;
}

/*
 *----------------------------------------------------------------------
 *
 *  TclLindexFlat --
 *
 * 	The core of the 'lindex' command, with all index
 * 	arguments presented as a flat list.
 *
 *  Value
 *
 *	A pointer to the object extracted, with its 'refCount' incremented,  or
 *	NULL if an error occurred.  Thus, the calling code will usually do
 *	something like:
 *
 * 		Tcl_SetObjResult(interp, result);
 * 		Tcl_DecrRefCount(result);
 *
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj *
TclLindexFlat(
    Tcl_Interp *interp,		/* Tcl interpreter. */
    Tcl_Obj *listPtr,		/* Tcl object representing the list. */
    int indexCount,		/* Count of indices. */
    Tcl_Obj *const indexArray[])/* Array of pointers to Tcl objects that
				 * represent the indices in the list. */
{
    int i;

    Tcl_IncrRefCount(listPtr);

    for (i=0 ; i<indexCount && listPtr ; i++) {
	size_t index;
	int listLen = 0;
	Tcl_Obj **elemPtrs = NULL, *sublistCopy;

	/*
	 * Here we make a private copy of the current sublist, so we avoid any
	 * shimmering issues that might invalidate the elemPtr array below
	 * while we are still using it. See test lindex-8.4.
	 */

	sublistCopy = TclListObjCopy(interp, listPtr);
	Tcl_DecrRefCount(listPtr);
	listPtr = NULL;

	if (sublistCopy == NULL) {
	    /*
	     * The sublist is not a list at all => error.
	     */

	    break;
	}
	TclListObjGetElementsM(NULL, sublistCopy, &listLen, &elemPtrs);

	if (TclGetIntForIndexM(interp, indexArray[i], /*endValue*/ listLen-1,
		&index) == TCL_OK) {
	    if (index >= (size_t)listLen) {
		/*
		 * Index is out of range. Break out of loop with empty result.
		 * First check remaining indices for validity
		 */

		while (++i < indexCount) {
		    if (TclGetIntForIndexM(interp, indexArray[i], (size_t)WIDE_MAX - 1, &index)
			!= TCL_OK) {
			Tcl_DecrRefCount(sublistCopy);
			return NULL;
		    }
		}
		TclNewObj(listPtr);
	    } else {
		/*
		 * Extract the pointer to the appropriate element.
		 */

		listPtr = elemPtrs[index];
	    }
	    Tcl_IncrRefCount(listPtr);
	}
	Tcl_DecrRefCount(sublistCopy);
    }

    return listPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TclLsetList --
 *
 *	The core of [lset] when objc == 4. Objv[2] may be either a
 *	scalar index or a list of indices.
 *      It also handles 'lpop' when given a NULL value.
 *
 *	Implemented entirely as a wrapper around 'TclLindexFlat', as described
 *	for 'TclLindexList'.
 *
 * Value
 *
 *	The new list, with the 'refCount' of 'valuPtr' incremented, or NULL if
 *	there was an error.
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj *
TclLsetList(
    Tcl_Interp *interp,		/* Tcl interpreter. */
    Tcl_Obj *listPtr,		/* Pointer to the list being modified. */
    Tcl_Obj *indexArgPtr,	/* Index or index-list arg to 'lset'. */
    Tcl_Obj *valuePtr)		/* Value arg to 'lset' or NULL to 'lpop'. */
{
    int indexCount = 0;		/* Number of indices in the index list. */
    Tcl_Obj **indices = NULL;	/* Vector of indices in the index list. */
    Tcl_Obj *retValuePtr;	/* Pointer to the list to be returned. */
    size_t index;			/* Current index in the list - discarded. */
    Tcl_Obj *indexListCopy;
    List *listRepPtr;

    /*
     * Determine whether the index arg designates a list or a single index.
     * We have to be careful about the order of the checks to avoid repeated
     * shimmering; see TIP #22 and #23 for details.
     */

    ListGetIntRep(indexArgPtr, listRepPtr);
    if (listRepPtr == NULL
	    && TclGetIntForIndexM(NULL, indexArgPtr, (size_t)WIDE_MAX - 1, &index) == TCL_OK) {
	/*
	 * indexArgPtr designates a single index.
	 */

	return TclLsetFlat(interp, listPtr, 1, &indexArgPtr, valuePtr);

    }

    indexListCopy = TclListObjCopy(NULL, indexArgPtr);
    if (indexListCopy == NULL) {
	/*
	 * indexArgPtr designates something that is neither an index nor a
	 * well formed list. Report the error via TclLsetFlat.
	 */

	return TclLsetFlat(interp, listPtr, 1, &indexArgPtr, valuePtr);
    }
    TclListObjGetElementsM(NULL, indexArgPtr, &indexCount, &indices);

    /*
     * Let TclLsetFlat handle the actual lset'ting.
     */

    retValuePtr = TclLsetFlat(interp, listPtr, indexCount, indices, valuePtr);

    Tcl_DecrRefCount(indexListCopy);
    return retValuePtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TclLsetFlat --
 *
 *	Core engine of the 'lset' command.
 *      It also handles 'lpop' when given a NULL value.
 *
 * Value
 *
 *	The resulting list
 *
 *	    The 'refCount' of 'valuePtr' is incremented.  If 'listPtr' was not
 *	    duplicated, its 'refCount' is incremented.  The reference count of
 *	    an unduplicated object is therefore 2 (one for the returned pointer
 *	    and one for the variable that holds it).  The reference count of a
 *	    duplicate object is 1, reflecting that result is the only active
 *	    reference. The caller is expected to store the result in the
 *	    variable and decrement its reference count. (INST_STORE_* does
 *	    exactly this.)
 *
 *	NULL
 *
 *	    An error occurred.  If 'listPtr' was duplicated, the reference
 *	    count on the duplicate is decremented so that it is 0, causing any
 *	    memory allocated by this function to be freed.
 *
 *
 * Effect
 *
 *	On entry, the reference count of 'listPtr' does not reflect any
 *	references held on the stack. The first action of this function is to
 *	determine whether 'listPtr' is shared and to create a duplicate
 *	unshared copy if it is.  The reference count of the duplicate is
 *	incremented. At this point, the reference count is 1 in either case so
 *	that the object is considered unshared.
 *
 *	The unshared list is altered directly to produce the result.
 *	'TclLsetFlat' maintains a linked list of 'Tcl_Obj' values whose string
 *	representations must be spoilt by threading via 'ptr2' of the
 *	two-pointer internal representation. On entry to 'TclLsetFlat', the
 *	values of 'ptr2' are immaterial; on exit, the 'ptr2' field of any
 *	Tcl_Obj that has been modified is set to NULL.
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj *
TclLsetFlat(
    Tcl_Interp *interp,		/* Tcl interpreter. */
    Tcl_Obj *listPtr,		/* Pointer to the list being modified. */
    int indexCount,		/* Number of index args. */
    Tcl_Obj *const indexArray[],
				/* Index args. */
    Tcl_Obj *valuePtr)		/* Value arg to 'lset' or NULL to 'lpop'. */
{
    size_t index;
    int result, len;
    Tcl_Obj *subListPtr, *retValuePtr, *chainPtr;
    Tcl_ObjInternalRep *irPtr;

    /*
     * If there are no indices, simply return the new value.  (Without
     * indices, [lset] is a synonym for [set].
     * [lpop] does not use this but protect for NULL valuePtr just in case.
     */

    if (indexCount == 0) {
	if (valuePtr != NULL) {
	    Tcl_IncrRefCount(valuePtr);
	}
	return valuePtr;
    }

    /*
     * If the list is shared, make a copy we can modify (copy-on-write).  We
     * use Tcl_DuplicateObj() instead of TclListObjCopy() for a few reasons:
     * 1) we have not yet confirmed listPtr is actually a list; 2) We make a
     * verbatim copy of any existing string rep, and when we combine that with
     * the delayed invalidation of string reps of modified Tcl_Obj's
     * implemented below, the outcome is that any error condition that causes
     * this routine to return NULL, will leave the string rep of listPtr and
     * all elements to be unchanged.
     */

    subListPtr = Tcl_IsShared(listPtr) ? Tcl_DuplicateObj(listPtr) : listPtr;

    /*
     * Anchor the linked list of Tcl_Obj's whose string reps must be
     * invalidated if the operation succeeds.
     */

    retValuePtr = subListPtr;
    chainPtr = NULL;
    result = TCL_OK;

    /*
     * Loop through all the index arguments, and for each one dive into the
     * appropriate sublist.
     */

    do {
	int elemCount;
	Tcl_Obj *parentList, **elemPtrs;

	/*
	 * Check for the possible error conditions...
	 */

	if (TclListObjGetElementsM(interp, subListPtr, &elemCount, &elemPtrs)
		!= TCL_OK) {
	    /* ...the sublist we're indexing into isn't a list at all. */
	    result = TCL_ERROR;
	    break;
	}

	/*
	 * WARNING: the macro TclGetIntForIndexM is not safe for
	 * post-increments, avoid '*indexArray++' here.
	 */

	if (TclGetIntForIndexM(interp, *indexArray, elemCount - 1, &index)
		!= TCL_OK)  {
	    /* ...the index we're trying to use isn't an index at all. */
	    result = TCL_ERROR;
	    indexArray++;
	    break;
	}
	indexArray++;

	if (index > (size_t)elemCount
		|| (valuePtr == NULL && index >= (size_t)elemCount)) {
	    /* ...the index points outside the sublist. */
	    if (interp != NULL) {
		Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			"index \"%s\" out of range", Tcl_GetString(indexArray[-1])));
		Tcl_SetErrorCode(interp, "TCL", "VALUE", "INDEX"
			"OUTOFRANGE", NULL);
	    }
	    result = TCL_ERROR;
	    break;
	}

	/*
	 * No error conditions.  As long as we're not yet on the last index,
	 * determine the next sublist for the next pass through the loop, and
	 * take steps to make sure it is an unshared copy, as we intend to
	 * modify it.
	 */

	if (--indexCount) {
	    parentList = subListPtr;
	    if (index == (size_t)elemCount) {
		TclNewObj(subListPtr);
	    } else {
		subListPtr = elemPtrs[index];
	    }
	    if (Tcl_IsShared(subListPtr)) {
		subListPtr = Tcl_DuplicateObj(subListPtr);
	    }

	    /*
	     * Replace the original elemPtr[index] in parentList with a copy
	     * we know to be unshared.  This call will also deal with the
	     * situation where parentList shares its internalrep with other
	     * Tcl_Obj's.  Dealing with the shared internalrep case can cause
	     * subListPtr to become shared again, so detect that case and make
	     * and store another copy.
	     */

	    if (index == (size_t)elemCount) {
		Tcl_ListObjAppendElement(NULL, parentList, subListPtr);
	    } else {
		TclListObjSetElement(NULL, parentList, index, subListPtr);
	    }
	    if (Tcl_IsShared(subListPtr)) {
		subListPtr = Tcl_DuplicateObj(subListPtr);
		TclListObjSetElement(NULL, parentList, index, subListPtr);
	    }

	    /*
	     * The TclListObjSetElement() calls do not spoil the string rep of
	     * parentList, and that's fine for now, since all we've done so
	     * far is replace a list element with an unshared copy.  The list
	     * value remains the same, so the string rep. is still valid, and
	     * unchanged, which is good because if this whole routine returns
	     * NULL, we'd like to leave no change to the value of the lset
	     * variable.  Later on, when we set valuePtr in its proper place,
	     * then all containing lists will have their values changed, and
	     * will need their string reps spoiled.  We maintain a list of all
	     * those Tcl_Obj's (via a little internalrep surgery) so we can spoil
	     * them at that time.
	     */

	    irPtr = TclFetchInternalRep(parentList, &tclListType);
	    irPtr->twoPtrValue.ptr2 = chainPtr;
	    chainPtr = parentList;
	}
    } while (indexCount > 0);

    /*
     * Either we've detected and error condition, and exited the loop with
     * result == TCL_ERROR, or we've successfully reached the last index, and
     * we're ready to store valuePtr.  In either case, we need to clean up our
     * string spoiling list of Tcl_Obj's.
     */

    while (chainPtr) {
	Tcl_Obj *objPtr = chainPtr;
	List *listRepPtr;

	/*
	 * Clear away our internalrep surgery mess.
	 */

	irPtr = TclFetchInternalRep(objPtr, &tclListType);
	listRepPtr = (List *)irPtr->twoPtrValue.ptr1;
	chainPtr = (Tcl_Obj *)irPtr->twoPtrValue.ptr2;

	if (result == TCL_OK) {

	    /*
	     * We're going to store valuePtr, so spoil string reps of all
	     * containing lists.
	     */

	    listRepPtr->refCount++;
	    TclFreeInternalRep(objPtr);
	    ListSetIntRep(objPtr, listRepPtr);
	    listRepPtr->refCount--;

	    TclInvalidateStringRep(objPtr);
	} else {
	    irPtr->twoPtrValue.ptr2 = NULL;
	}
    }

    if (result != TCL_OK) {
	/*
	 * Error return; message is already in interp. Clean up any excess
	 * memory.
	 */

	if (retValuePtr != listPtr) {
	    Tcl_DecrRefCount(retValuePtr);
	}
	return NULL;
    }

    /*
     * Store valuePtr in proper sublist and return. The -1 is to avoid a
     * compiler warning (not a problem because we checked that we have a
     * proper list - or something convertible to one - above).
     */

    len = -1;
    TclListObjLengthM(NULL, subListPtr, &len);
    if (valuePtr == NULL) {
	Tcl_ListObjReplace(NULL, subListPtr, index, 1, 0, NULL);
    } else if (index == (size_t)len) {
	Tcl_ListObjAppendElement(NULL, subListPtr, valuePtr);
    } else {
	TclListObjSetElement(NULL, subListPtr, index, valuePtr);
	TclInvalidateStringRep(subListPtr);
    }
    Tcl_IncrRefCount(retValuePtr);
    return retValuePtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TclListObjSetElement --
 *
 *	Set a single element of a list to a specified value.
 *
 *	It is the caller's responsibility to invalidate the string
 *	representation of the 'listPtr'.
 *
 * Value
 *
 * 	TCL_OK
 *
 *	    Success.
 *
 *	TCL_ERROR
 *
 *	    'listPtr' does not refer to a list object and cannot be converted
 *	    to one.  An error message will be left in the interpreter result if
 *	    interp is not NULL.
 *
 *	TCL_ERROR
 *
 *	    An index designates an element outside the range [0..listLength-1],
 *	    where 'listLength' is the count of elements in the list object
 *	    designated by 'listPtr'.  An error message is left in the
 *	    interpreter result.
 *
 * Effect
 *
 *	If 'listPtr' designates a shared object, 'Tcl_Panic' is called.  If
 *	'listPtr' is not already of type 'tclListType', it is converted and the
 *	internal representation is unshared. The 'refCount' of the element at
 *	'index' is decremented and replaced in the list with the 'valuePtr',
 *	whose 'refCount' in turn is incremented.
 *
 *
 *----------------------------------------------------------------------
 */

int
TclListObjSetElement(
    Tcl_Interp *interp,		/* Tcl interpreter; used for error reporting
				 * if not NULL. */
    Tcl_Obj *listPtr,		/* List object in which element should be
				 * stored. */
    int index,			/* Index of element to store. */
    Tcl_Obj *valuePtr)		/* Tcl object to store in the designated list
				 * element. */
{
    List *listRepPtr;		/* Internal representation of the list being
				 * modified. */
    Tcl_Obj **elemPtrs;		/* Pointers to elements of the list. */
    int elemCount;		/* Number of elements in the list. */

    /*
     * Ensure that the listPtr parameter designates an unshared list.
     */

    if (Tcl_IsShared(listPtr)) {
	Tcl_Panic("%s called with shared object", "TclListObjSetElement");
    }

    ListGetIntRep(listPtr, listRepPtr);
    if (listRepPtr == NULL) {
	int result;
	size_t length;

	(void) Tcl_GetStringFromObj(listPtr, &length);
	if (length == 0) {
	    if (interp != NULL) {
		Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			"index \"%d\" out of range", index));
		Tcl_SetErrorCode(interp, "TCL", "VALUE", "INDEX",
			"OUTOFRANGE", NULL);
	    }
	    return TCL_ERROR;
	}
	result = SetListFromAny(interp, listPtr);
	if (result != TCL_OK) {
	    return result;
	}
	ListGetIntRep(listPtr, listRepPtr);
    }

    elemCount = listRepPtr->elemCount;

    /*
     * Ensure that the index is in bounds.
     */

    if (index<0 || index>=elemCount) {
	if (interp != NULL) {
		Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			"index \"%d\" out of range", index));
	    Tcl_SetErrorCode(interp, "TCL", "VALUE", "INDEX",
		    "OUTOFRANGE", NULL);
	}
	return TCL_ERROR;
    }

    /*
     * If the internal rep is shared, replace it with an unshared copy.
     */

    if (listRepPtr->refCount > 1) {
	Tcl_Obj **dst, **src = &listRepPtr->elements;
	List *newPtr = AttemptNewList(NULL, listRepPtr->maxElemCount, NULL);

	if (newPtr == NULL) {
	    newPtr = AttemptNewList(interp, elemCount, NULL);
	    if (newPtr == NULL) {
		return TCL_ERROR;
	    }
	}
	newPtr->refCount++;
	newPtr->elemCount = elemCount;
	newPtr->canonicalFlag = listRepPtr->canonicalFlag;

	dst = &newPtr->elements;
	while (elemCount--) {
	    *dst = *src++;
	    Tcl_IncrRefCount(*dst++);
	}

	listRepPtr->refCount--;

	listRepPtr = newPtr;
	ListResetIntRep(listPtr, listRepPtr);
    }
    elemPtrs = &listRepPtr->elements;

    /*
     * Add a reference to the new list element.
     */

    Tcl_IncrRefCount(valuePtr);

    /*
     * Remove a reference from the old list element.
     */

    Tcl_DecrRefCount(elemPtrs[index]);

    /*
     * Stash the new object in the list.
     */

    elemPtrs[index] = valuePtr;

    /*
     * Invalidate outdated internalreps.
     */

    ListGetIntRep(listPtr, listRepPtr);
    listRepPtr->refCount++;
    TclFreeInternalRep(listPtr);
    ListSetIntRep(listPtr, listRepPtr);
    listRepPtr->refCount--;

    TclInvalidateStringRep(listPtr);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * FreeListInternalRep --
 *
 *	Deallocate the storage associated with the internal representation of a
 *	a list object.
 *
 * Effect
 *
 *	Frees listPtr's List* internal representation, if no longer shared.
 *	May decrement the ref counts of element objects, which may free them.
 *
 *----------------------------------------------------------------------
 */

static void
FreeListInternalRep(
    Tcl_Obj *listPtr)		/* List object with internal rep to free. */
{
    List *listRepPtr;

    ListGetIntRep(listPtr, listRepPtr);
    assert(listRepPtr != NULL);

    if (listRepPtr->refCount-- <= 1) {
	Tcl_Obj **elemPtrs = &listRepPtr->elements;
	int i, numElems = listRepPtr->elemCount;

	for (i = 0;  i < numElems;  i++) {
	    Tcl_DecrRefCount(elemPtrs[i]);
	}
	Tcl_Free(listRepPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * DupListInternalRep --
 *
 *	Initialize the internal representation of a list 'Tcl_Obj' to share the
 *	internal representation of an existing list object.
 *
 * Effect
 *
 *	The 'refCount' of the List internal rep is incremented.
 *
 *----------------------------------------------------------------------
 */

static void
DupListInternalRep(
    Tcl_Obj *srcPtr,		/* Object with internal rep to copy. */
    Tcl_Obj *copyPtr)		/* Object with internal rep to set. */
{
    List *listRepPtr;

    ListGetIntRep(srcPtr, listRepPtr);
    assert(listRepPtr != NULL);
    ListSetIntRep(copyPtr, listRepPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * SetListFromAny --
 *
 *	Convert any object to a list.
 *
 * Value
 *
 *    TCL_OK
 *
 *	Success.  The internal representation of 'objPtr' is set, and the type
 *	of 'objPtr' is 'tclListType'.
 *
 *    TCL_ERROR
 *
 *	An error occured during conversion. An error message is left in the
 *	interpreter's result if 'interp' is not NULL.
 *
 *
 *----------------------------------------------------------------------
 */

static int
SetListFromAny(
    Tcl_Interp *interp,		/* Used for error reporting if not NULL. */
    Tcl_Obj *objPtr)		/* The object to convert. */
{
    List *listRepPtr;
    Tcl_Obj **elemPtrs;

    /*
     * Dictionaries are a special case; they have a string representation such
     * that *all* valid dictionaries are valid lists. Hence we can convert
     * more directly. Only do this when there's no existing string rep; if
     * there is, it is the string rep that's authoritative (because it could
     * describe duplicate keys).
     */

    if (!TclHasStringRep(objPtr) && TclHasInternalRep(objPtr, &tclDictType)) {
	Tcl_Obj *keyPtr, *valuePtr;
	Tcl_DictSearch search;
	int done, size;

	/*
	 * Create the new list representation. Note that we do not need to do
	 * anything with the string representation as the transformation (and
	 * the reverse back to a dictionary) are both order-preserving. Also
	 * note that since we know we've got a valid dictionary (by
	 * representation) we also know that fetching the size of the
	 * dictionary or iterating over it will not fail.
	 */

	Tcl_DictObjSize(NULL, objPtr, &size);
	listRepPtr = AttemptNewList(interp, size > 0 ? 2*size : 1, NULL);
	if (!listRepPtr) {
	    return TCL_ERROR;
	}
	listRepPtr->elemCount = 2 * size;

	/*
	 * Populate the list representation.
	 */

	elemPtrs = &listRepPtr->elements;
	Tcl_DictObjFirst(NULL, objPtr, &search, &keyPtr, &valuePtr, &done);
	while (!done) {
	    *elemPtrs++ = keyPtr;
	    *elemPtrs++ = valuePtr;
	    Tcl_IncrRefCount(keyPtr);
	    Tcl_IncrRefCount(valuePtr);
	    Tcl_DictObjNext(&search, &keyPtr, &valuePtr, &done);
	}
    } else {
	int estCount;
	size_t length;
	const char *limit, *nextElem = Tcl_GetStringFromObj(objPtr, &length);

	/*
	 * Allocate enough space to hold a (Tcl_Obj *) for each
	 * (possible) list element.
	 */

	estCount = TclMaxListLength(nextElem, length, &limit);
	estCount += (estCount == 0);	/* Smallest list struct holds 1
					 * element. */
	listRepPtr = AttemptNewList(interp, estCount, NULL);
	if (listRepPtr == NULL) {
	    return TCL_ERROR;
	}
	elemPtrs = &listRepPtr->elements;

	/*
	 * Each iteration, parse and store a list element.
	 */

	while (nextElem < limit) {
	    const char *elemStart;
	    char *check;
	    size_t elemSize;
	    int literal;

	    if (TCL_OK != TclFindElement(interp, nextElem, limit - nextElem,
		    &elemStart, &nextElem, &elemSize, &literal)) {
	    fail:
		while (--elemPtrs >= &listRepPtr->elements) {
		    Tcl_DecrRefCount(*elemPtrs);
		}
		Tcl_Free(listRepPtr);
		return TCL_ERROR;
	    }
	    if (elemStart == limit) {
		break;
	    }

	    TclNewObj(*elemPtrs);
	    TclInvalidateStringRep(*elemPtrs);
	    check = Tcl_InitStringRep(*elemPtrs, literal ? elemStart : NULL,
		    elemSize);
	    if (elemSize && check == NULL) {
		if (interp) {
		    Tcl_SetObjResult(interp, Tcl_NewStringObj(
			    "cannot construct list, out of memory", -1));
		    Tcl_SetErrorCode(interp, "TCL", "MEMORY", NULL);
		}
		goto fail;
	    }
	    if (!literal) {
		Tcl_InitStringRep(*elemPtrs, NULL,
			TclCopyAndCollapse(elemSize, elemStart, check));
	    }

	    Tcl_IncrRefCount(*elemPtrs++);/* Since list now holds ref to it. */
	}

 	listRepPtr->elemCount = elemPtrs - &listRepPtr->elements;
    }

    /*
     * Store the new internalRep. We do this as late
     * as possible to allow the conversion code, in particular
     * Tcl_GetStringFromObj, to use the old internalRep.
     */

    ListSetIntRep(objPtr, listRepPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UpdateStringOfList --
 *
 *	Update the string representation for a list object.
 *
 *	Any previously-exising string representation is not invalidated, so
 *	storage is lost if this has not been taken care of.
 *
 * Effect
 *
 *	The string representation of 'listPtr' is set to the resulting string.
 *	This string will be empty if the list has no elements. It is assumed
 *	that the list internal representation is not NULL.
 *
 *----------------------------------------------------------------------
 */

static void
UpdateStringOfList(
    Tcl_Obj *listPtr)		/* List object with string rep to update. */
{
#   define LOCAL_SIZE 64
    char localFlags[LOCAL_SIZE], *flagPtr = NULL;
    int numElems, i;
    size_t length, bytesNeeded = 0;
    const char *elem, *start;
    char *dst;
    Tcl_Obj **elemPtrs;
    List *listRepPtr;

    ListGetIntRep(listPtr, listRepPtr);

    assert(listRepPtr != NULL);

    numElems = listRepPtr->elemCount;

    /*
     * Mark the list as being canonical; although it will now have a string
     * rep, it is one we derived through proper "canonical" quoting and so
     * it's known to be free from nasties relating to [concat] and [eval].
     */

    listRepPtr->canonicalFlag = 1;

    /*
     * Handle empty list case first, so rest of the routine is simpler.
     */

    if (numElems == 0) {
	Tcl_InitStringRep(listPtr, NULL, 0);
	return;
    }

    /*
     * Pass 1: estimate space, gather flags.
     */

    if (numElems <= LOCAL_SIZE) {
	flagPtr = localFlags;
    } else {
	/*
	 * We know numElems <= LIST_MAX, so this is safe.
	 */

	flagPtr = (char *)Tcl_Alloc(numElems);
    }
    elemPtrs = &listRepPtr->elements;
    for (i = 0; i < numElems; i++) {
	flagPtr[i] = (i ? TCL_DONT_QUOTE_HASH : 0);
	elem = Tcl_GetStringFromObj(elemPtrs[i], &length);
	bytesNeeded += TclScanElement(elem, length, flagPtr+i);
    }
    bytesNeeded += numElems - 1;

    /*
     * Pass 2: copy into string rep buffer.
     */

    start = dst = Tcl_InitStringRep(listPtr, NULL, bytesNeeded);
    TclOOM(dst, bytesNeeded);
    for (i = 0; i < numElems; i++) {
	flagPtr[i] |= (i ? TCL_DONT_QUOTE_HASH : 0);
	elem = Tcl_GetStringFromObj(elemPtrs[i], &length);
	dst += TclConvertElement(elem, length, dst, flagPtr[i]);
	*dst++ = ' ';
    }

    /* Set the string length to what was actually written, the safe choice */
    (void) Tcl_InitStringRep(listPtr, NULL, dst - 1 - start);

    if (flagPtr != localFlags) {
	Tcl_Free(flagPtr);
    }
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
