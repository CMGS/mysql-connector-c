/* Copyright (C) 2000-2003 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/*
 * Memory sub-system, written by Bjorn Benson
   Fixed to use my_sys scheme by Michael Widenius

  [This posting refers to an article entitled "oops, corrupted memory
  again!" in net.lang.c.  I am posting it here because it is source.]

  My tool for approaching this problem is to build another level of data
  abstraction on top of malloc() and free() that implements some checking.
  This does a number of things for you:
	- Checks for overruns and underruns on allocated data
	- Keeps track of where in the program the memory was malloc'ed
	- Reports on pieces of memory that were not free'ed
	- Records some statistics such as maximum memory used
	- Marks newly malloc'ed and newly free'ed memory with special values
  You can use this scheme to:
	- Find bugs such as overrun, underrun, etc because you know where
	  a piece of data was malloc'ed and where it was free'ed
	- Find bugs where memory was not free'ed
	- Find bugs where newly malloc'ed memory is used without initializing
	- Find bugs where newly free'ed memory is still used
	- Determine how much memory your program really uses
	- and other things

  To implement my scheme you must have a C compiler that has __LINE__ and
  __FILE__ macros.  If your compiler doesn't have these then (a) buy another:
  compilers that do are available on UNIX 4.2bsd based systems and the PC,
  and probably on other machines; or (b) change my scheme somehow.  I have
  recomendations on both these points if you would like them (e-mail please).

  There are 4 functions in my package:
	char *NEW( uSize )	Allocate memory of uSize bytes
				(equivalent to malloc())
	char *REA( pPtr, uSize) Allocate memory of uSize bytes, move data and
				free pPtr.
				(equivalent to realloc())
	FREE( pPtr )		Free memory allocated by NEW
				(equivalent to free())
	TERMINATE(file,flag)	End system, report errors and stats on file
  I personally use two more functions, but have not included them here:
	char *STRSAVE( sPtr )	Save a copy of the string in dynamic memory
	char *RENEW( pPtr, uSize )
				(equivalent to realloc())

*/

#ifndef SAFEMALLOC
#define SAFEMALLOC			/* Get protos from my_sys */
#endif

#include "mysys_priv.h"
#include <m_string.h>
#include "my_static.h"
#include "mysys_err.h"

ulonglong sf_malloc_mem_limit= ~(ulonglong)0;

#ifndef PEDANTIC_SAFEMALLOC
/*
  Set to 1 after TERMINATE() if we had to fiddle with sf_malloc_count and
  the linked list of blocks so that _sanity() will not fuss when it
  is not supposed to
*/
static int sf_malloc_tampered= 0;
#endif
				   

	/* Static functions prototypes */

static int check_ptr(const char *where, uchar *ptr, const char *sFile,
		     uint uLine);
static int _checkchunk(struct st_irem *pRec, const char *sFile, uint uLine);

/*
  Note: We only fill up the allocated block. This do not include
  malloc() roundoff or the extra space required by the irem
  structures.
*/

/*
  NEW'ed memory is filled with this value so that references to it will
  end up being very strange.
*/
#define ALLOC_VAL	(uchar) 0xA5
/*
  FEEE'ed memory is filled with this value so that references to it will
  end up being very strange.
*/
#define FREE_VAL	(uchar) 0x8F
#define MAGICKEY	0x14235296	/* A magic value for underrun key */

/*
  Warning: do not change the MAGICEND? values to something with the
  high bit set.  Various C compilers (like the 4.2bsd one) do not do
  the sign extension right later on in this code and you will get
  erroneous errors.
*/

#define MAGICEND0	0x68		/* Magic values for overrun keys  */
#define MAGICEND1	0x34		/*		"		  */
#define MAGICEND2	0x7A		/*		"		  */
#define MAGICEND3	0x15		/*		"		  */


/* Allocate some memory. */

void *_mymalloc(size_t size, const char *filename, uint lineno, myf MyFlags)
{
  struct st_irem *irem;
  uchar *data;
  DBUG_ENTER("_mymalloc");
  DBUG_PRINT("enter",("Size: %lu", (ulong) size));

  if (!sf_malloc_quick)
    (void) _sanity (filename, lineno);

  /*
    Test for memory limit overrun.
    If compiled with DBUG, test for error injection. Described in my_sys.h.
  */
  if ((size + sf_malloc_cur_memory > sf_malloc_mem_limit)
      IF_DBUG(|| my_malloc_error_inject))
  {
    IF_DBUG(if (my_malloc_error_inject)
              errno= ENOMEM;
            my_malloc_error_inject= 0);
    irem= 0;
  }
  else
  {
    /* Allocate the physical memory */
    irem= (struct st_irem *) malloc (ALIGN_SIZE(sizeof(struct st_irem)) +
				     sf_malloc_prehunc +
				     size +	/* size requested */
				     4 +	/* overrun mark */
				     sf_malloc_endhunc);
  }
  /* Check if there isn't anymore memory avaiable */
  if (!irem)
  {
    if (MyFlags & MY_FAE)
      error_handler_hook=fatal_error_handler_hook;
    if (MyFlags & (MY_FAE+MY_WME))
    {
      char buff[MYSYS_ERRMSG_SIZE];
      my_errno=errno;
      my_snprintf(buff, sizeof(buff), "Out of memory at line %d, '%s'",
                  lineno, filename);
      my_message(EE_OUTOFMEMORY, buff, MYF(ME_BELL+ME_WAITTANG+ME_NOREFRESH));
      my_snprintf(buff, sizeof(buff),
                  "needed %lu byte (%luk), memory in use: %lu bytes (%luk)",
	          (ulong) size, (ulong) (size + 1023L) / 1024L,
	          (ulong) sf_malloc_max_memory,
	          (ulong) (sf_malloc_max_memory + 1023L) / 1024L);
      my_message(EE_OUTOFMEMORY, buff, MYF(ME_BELL+ME_WAITTANG+ME_NOREFRESH));
    }
    DBUG_PRINT("error",("Out of memory, in use: %ld at line %d, '%s'",
			sf_malloc_max_memory,lineno, filename));
    if (MyFlags & MY_FAE)
      exit(1);
    DBUG_RETURN ((void*) 0);
  }

  /* Fill up the structure */
  data= (((uchar*) irem) + ALIGN_SIZE(sizeof(struct st_irem)) +
	 sf_malloc_prehunc);
  *((uint32*) (data-sizeof(uint32)))= MAGICKEY;
  data[size + 0]= MAGICEND0;
  data[size + 1]= MAGICEND1;
  data[size + 2]= MAGICEND2;
  data[size + 3]= MAGICEND3;
  irem->filename= (char *) filename;
  irem->linenum= lineno;
  irem->datasize= size;
  irem->prev=	  NULL;

  /* Add this remember structure to the linked list */
  pthread_mutex_lock(&THR_LOCK_malloc);
  if ((irem->next= sf_malloc_root))
    sf_malloc_root->prev= irem;
  sf_malloc_root= irem;

  /* Keep the statistics */
  sf_malloc_cur_memory+= size;
  if (sf_malloc_cur_memory > sf_malloc_max_memory)
    sf_malloc_max_memory= sf_malloc_cur_memory;
  sf_malloc_count++;
  pthread_mutex_unlock(&THR_LOCK_malloc);

  /* Set the memory to the aribtrary wierd value */
  if ((MyFlags & MY_ZEROFILL) || !sf_malloc_quick)
    bfill(data, size, (char) (MyFlags & MY_ZEROFILL ? 0 : ALLOC_VAL));
  /* Return a pointer to the real data */
  DBUG_PRINT("exit",("ptr: %p", data));
  if (sf_min_adress > data)
    sf_min_adress= data;
  if (sf_max_adress < data)
    sf_max_adress= data;
  DBUG_RETURN((void*) data);
}


/*
  Allocate some new memory and move old memoryblock there.
  Free then old memoryblock
*/

void *_myrealloc(register void *ptr, register size_t size,
                 const char *filename, uint lineno, myf MyFlags)
{
  struct st_irem *irem;
  char *data;
  DBUG_ENTER("_myrealloc");

  if (!ptr && (MyFlags & MY_ALLOW_ZERO_PTR))
    DBUG_RETURN(_mymalloc(size, filename, lineno, MyFlags));

  if (!sf_malloc_quick)
    (void) _sanity (filename, lineno);

  if (check_ptr("Reallocating", (uchar*) ptr, filename, lineno))
    DBUG_RETURN((uchar*) NULL);

  irem= (struct st_irem *) (((char*) ptr) - ALIGN_SIZE(sizeof(struct st_irem))-
			    sf_malloc_prehunc);
  if (*((uint32*) (((char*) ptr)- sizeof(uint32))) != MAGICKEY)
  {
    fprintf(stderr, "Error: Reallocating unallocated data at line %d, '%s'\n",
	    lineno, filename);
    DBUG_PRINT("safe",("Reallocating unallocated data at line %d, '%s'",
		       lineno, filename));
    (void) fflush(stderr);
    DBUG_RETURN((uchar*) NULL);
  }

  if ((data= _mymalloc(size,filename,lineno,MyFlags))) /* Allocate new area */
  {
    size=min(size, irem->datasize);		/* Move as much as possibly */
    memcpy((uchar*) data, ptr, (size_t) size);	/* Copy old data */
    _myfree(ptr, filename, lineno, 0);		/* Free not needed area */
  }
  else
  {
    if (MyFlags & MY_HOLD_ON_ERROR)
      DBUG_RETURN(ptr);
    if (MyFlags & MY_FREE_ON_ERROR)
      _myfree(ptr, filename, lineno, 0);
  }
  DBUG_RETURN(data);
} /* _myrealloc */


/* Deallocate some memory. */

void _myfree(void *ptr, const char *filename, uint lineno, myf myflags)
{
  struct st_irem *irem;
  DBUG_ENTER("_myfree");
  DBUG_PRINT("enter",("ptr: %p", ptr));

  if (!sf_malloc_quick)
    (void) _sanity (filename, lineno);

  if ((!ptr && (myflags & MY_ALLOW_ZERO_PTR)) ||
      check_ptr("Freeing",(uchar*) ptr,filename,lineno))
    DBUG_VOID_RETURN;

  /* Calculate the address of the remember structure */
  irem= (struct st_irem *) ((char*) ptr- ALIGN_SIZE(sizeof(struct st_irem))-
			    sf_malloc_prehunc);

  /*
    Check to make sure that we have a real remember structure.
    Note: this test could fail for four reasons:
    (1) The memory was already free'ed
    (2) The memory was never new'ed
    (3) There was an underrun
    (4) A stray pointer hit this location
  */

  if (*((uint32*) ((char*) ptr- sizeof(uint32))) != MAGICKEY)
  {
    fprintf(stderr, "Error: Freeing unallocated data at line %d, '%s'\n",
	    lineno, filename);
    DBUG_PRINT("safe",("Unallocated data at line %d, '%s'",lineno,filename));
    (void) fflush(stderr);
    DBUG_VOID_RETURN;
  }

  /* Remove this structure from the linked list */
  pthread_mutex_lock(&THR_LOCK_malloc);
  if (irem->prev)
    irem->prev->next= irem->next;
   else
    sf_malloc_root= irem->next;

  if (irem->next)
    irem->next->prev= irem->prev;
  /* Handle the statistics */
  sf_malloc_cur_memory-= irem->datasize;
  sf_malloc_count--;
  pthread_mutex_unlock(&THR_LOCK_malloc);

#ifndef HAVE_purify
  /* Mark this data as free'ed */
  if (!sf_malloc_quick)
    bfill(ptr, irem->datasize, (pchar) FREE_VAL);
#endif
  *((uint32*) ((char*) ptr- sizeof(uint32)))= ~MAGICKEY;
  /* Actually free the memory */
  free((char*) irem);
  DBUG_VOID_RETURN;
}

	/* Check if we have a wrong  pointer */

static int check_ptr(const char *where, uchar *ptr, const char *filename,
		     uint lineno)
{
  if (!ptr)
  {
    fprintf(stderr, "Error: %s NULL pointer at line %d, '%s'\n",
	    where,lineno, filename);
    DBUG_PRINT("safe",("Null pointer at line %d '%s'", lineno, filename));
    (void) fflush(stderr);
    return 1;
  }
#ifndef _MSC_VER
  if ((long) ptr & (ALIGN_SIZE(1)-1))
  {
    fprintf(stderr, "Error: %s wrong aligned pointer at line %d, '%s'\n",
	    where,lineno, filename);
    DBUG_PRINT("safe",("Wrong aligned pointer at line %d, '%s'",
		       lineno,filename));
    (void) fflush(stderr);
    return 1;
  }
#endif
  if (ptr < sf_min_adress || ptr > sf_max_adress)
  {
    fprintf(stderr, "Error: %s pointer out of range at line %d, '%s'\n",
	    where,lineno, filename);
    DBUG_PRINT("safe",("Pointer out of range at line %d '%s'",
		       lineno,filename));
    (void) fflush(stderr);
    return 1;
  }
  return 0;
}


/*
  Report on all the memory pieces that have not been free'ed

  SYNOPSIS
    TERMINATE()
    file   Write output to this file
    flag   If <> 0, also write statistics 
 */

void TERMINATE(FILE *file, uint flag)
{
  struct st_irem *irem;
  DBUG_ENTER("TERMINATE");
  pthread_mutex_lock(&THR_LOCK_malloc);

  /*
    Report the difference between number of calls to
    NEW and the number of calls to FREE.  >0 means more
    NEWs than FREEs.  <0, etc.
  */

  if (sf_malloc_count)
  {
    if (file)
    {
      fprintf(file, "Warning: Not freed memory segments: %u\n", sf_malloc_count);
      (void) fflush(file);
    }
    DBUG_PRINT("safe",("sf_malloc_count: %u", sf_malloc_count));
  }

  /*
    Report on all the memory that was allocated with NEW
    but not free'ed with FREE.
  */

  if ((irem= sf_malloc_root))
  {
    if (file)
    {
      fprintf(file, "Warning: Memory that was not free'ed (%lu bytes):\n",
	      (ulong) sf_malloc_cur_memory);
      (void) fflush(file);
    }
    DBUG_PRINT("safe",("Memory that was not free'ed (%lu bytes):",
		       (ulong) sf_malloc_cur_memory));
    while (irem)
    {
      char *data= (((char*) irem) + ALIGN_SIZE(sizeof(struct st_irem)) +
		   sf_malloc_prehunc);
      if (file)
      {
	fprintf(file,
		"\t%6lu bytes at %p, allocated at line %4u in '%s'",
		(ulong) irem->datasize, data, irem->linenum, irem->filename);
	fprintf(file, "\n");
	(void) fflush(file);
      }
      DBUG_PRINT("safe",
		 ("%6lu bytes at %p, allocated at line %4d in '%s'",
		  (ulong) irem->datasize,
		  data, irem->linenum, irem->filename));
      irem= irem->next;
    }
  }
  /* Report the memory usage statistics */
  if (file && flag)
  {
    fprintf(file, "Maximum memory usage: %lu bytes (%luk)\n",
	    (ulong) sf_malloc_max_memory,
	    (ulong) (sf_malloc_max_memory + 1023L) / 1024L);
    (void) fflush(file);
  }
  DBUG_PRINT("safe",("Maximum memory usage: %lu bytes (%luk)",
		     (ulong) sf_malloc_max_memory,
		     (ulong) (sf_malloc_max_memory + 1023L) /1024L));
  pthread_mutex_unlock(&THR_LOCK_malloc);
  DBUG_VOID_RETURN;
}


/*
  Report where a piece of memory was allocated

  This is usefull to call from withing a debugger
*/


void sf_malloc_report_allocated(void *memory)
{
  struct st_irem *irem;  
  for (irem= sf_malloc_root ; irem ; irem=irem->next)
  {
    char *data= (((char*) irem) + ALIGN_SIZE(sizeof(struct st_irem)) +
                 sf_malloc_prehunc);
    if (data <= (char*) memory && (char*) memory <= data + irem->datasize)
    {
      printf("%lu bytes at %p, allocated at line %u in '%s'\n",
             (ulong) irem->datasize, data, irem->linenum, irem->filename);
      break;
    }
  }
}

	/* Returns 0 if chunk is ok */

static int _checkchunk(register struct st_irem *irem, const char *filename,
		       uint lineno)
{
  int flag=0;
  char *magicp, *data;

  data= (((char*) irem) + ALIGN_SIZE(sizeof(struct st_irem)) +
	 sf_malloc_prehunc);
  /* Check for a possible underrun */
  if (*((uint32*) (data- sizeof(uint32))) != MAGICKEY)
  {
    fprintf(stderr, "Error: Memory allocated at %s:%d was underrun,",
	    irem->filename, irem->linenum);
    fprintf(stderr, " discovered at %s:%d\n", filename, lineno);
    (void) fflush(stderr);
    DBUG_PRINT("safe",("Underrun at %p, allocated at %s:%d",
		       data, irem->filename, irem->linenum));
    flag=1;
  }

  /* Check for a possible overrun */
  magicp= data + irem->datasize;
  if (*magicp++ != MAGICEND0 ||
      *magicp++ != MAGICEND1 ||
      *magicp++ != MAGICEND2 ||
      *magicp++ != MAGICEND3)
  {
    fprintf(stderr, "Error: Memory allocated at %s:%d was overrun,",
	    irem->filename, irem->linenum);
    fprintf(stderr, " discovered at '%s:%d'\n", filename, lineno);
    (void) fflush(stderr);
    DBUG_PRINT("safe",("Overrun at %p, allocated at %s:%d",
		       data, irem->filename, irem->linenum));
    flag=1;
  }
  return(flag);
}


	/* Returns how many wrong chunks */

int _sanity(const char *filename, uint lineno)
{
  reg1 struct st_irem *irem;
  reg2 int flag=0;
  uint count=0;

  pthread_mutex_lock(&THR_LOCK_malloc);
#ifndef PEDANTIC_SAFEMALLOC  
  if (sf_malloc_tampered && (int) sf_malloc_count < 0)
    sf_malloc_count=0;
#endif  
  count=sf_malloc_count;
  for (irem= sf_malloc_root; irem != NULL && count-- ; irem= irem->next)
    flag+= _checkchunk (irem, filename, lineno);
  pthread_mutex_unlock(&THR_LOCK_malloc);
  if (count || irem)
  {
    const char *format="Error: Safemalloc link list destroyed, discovered at '%s:%d'";
    fprintf(stderr, format, filename, lineno); fputc('\n',stderr);
    fprintf(stderr, "root=%p,count=%d,irem=%p\n", sf_malloc_root,count,irem);
    (void) fflush(stderr);
    DBUG_PRINT("safe",(format, filename, lineno));
    flag=1;
  }
  return flag;
} /* _sanity */


	/* malloc and copy */

void *_my_memdup(const void *from, size_t length, const char *filename,
                  uint lineno, myf MyFlags)
{
  void *ptr;
  if ((ptr= _mymalloc(length,filename,lineno,MyFlags)) != 0)
    memcpy(ptr, from, length);
  return(ptr);
} /*_my_memdup */


char *_my_strdup(const char *from, const char *filename, uint lineno,
		 myf MyFlags)
{
  char *ptr;
  size_t length= strlen(from)+1;
  if ((ptr= (char*) _mymalloc(length,filename,lineno,MyFlags)) != 0)
    memcpy((uchar*) ptr, (uchar*) from, (size_t) length);
  return(ptr);
} /* _my_strdup */


char *_my_strndup(const char *from, size_t length,
                  const char *filename, uint lineno,
                  myf MyFlags)
{
  char *ptr;
  if ((ptr= (char*) _mymalloc(length+1,filename,lineno,MyFlags)) != 0)
  {
    memcpy((uchar*) ptr, (uchar*) from, (size_t) length);
    ptr[length]=0;
  }
  return(ptr);
}
