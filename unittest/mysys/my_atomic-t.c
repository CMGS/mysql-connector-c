/* Copyright (C) 2006-2008 MySQL AB, 2008 Sun Microsystems, Inc.

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

#include "thr_template.c"

/* at least gcc 3.4.5 and 3.4.6 (but not 3.2.3) on RHEL */
#if __GNUC__ == 3 && __GNUC_MINOR__ == 4
#define GCC_BUG_WORKAROUND volatile
#else
#define GCC_BUG_WORKAROUND
#endif

volatile uint32 b32;
volatile int32  c32;
my_atomic_rwlock_t rwl;

/* add and sub a random number in a loop. Must get 0 at the end */
pthread_handler_t test_atomic_add(void *arg)
{
  int    m= (*(int *)arg)/2;
  GCC_BUG_WORKAROUND int32 x;
  for (x= ((int)(intptr)(&m)); m ; m--)
  {
    x= (x*m+0x87654321) & INT_MAX32;
    my_atomic_rwlock_wrlock(&rwl);
    my_atomic_add32(&bad, x);
    my_atomic_rwlock_wrunlock(&rwl);

    my_atomic_rwlock_wrlock(&rwl);
    my_atomic_add32(&bad, -x);
    my_atomic_rwlock_wrunlock(&rwl);
  }
  pthread_mutex_lock(&mutex);
  if (!--running_threads) pthread_cond_signal(&cond);
  pthread_mutex_unlock(&mutex);
  return 0;
}

/*
  1. generate thread number 0..N-1 from b32
  2. add it to bad
  3. swap thread numbers in c32
  4. (optionally) one more swap to avoid 0 as a result
  5. subtract result from bad
  must get 0 in bad at the end
*/
pthread_handler_t test_atomic_fas(void *arg)
{
  int    m= *(int *)arg;
  int32  x;

  my_atomic_rwlock_wrlock(&rwl);
  x= my_atomic_add32(&b32, 1);
  my_atomic_rwlock_wrunlock(&rwl);

  my_atomic_rwlock_wrlock(&rwl);
  my_atomic_add32(&bad, x);
  my_atomic_rwlock_wrunlock(&rwl);

  for (; m ; m--)
  {
    my_atomic_rwlock_wrlock(&rwl);
    x= my_atomic_fas32(&c32, x);
    my_atomic_rwlock_wrunlock(&rwl);
  }

  if (!x)
  {
    my_atomic_rwlock_wrlock(&rwl);
    x= my_atomic_fas32(&c32, x);
    my_atomic_rwlock_wrunlock(&rwl);
  }

  my_atomic_rwlock_wrlock(&rwl);
  my_atomic_add32(&bad, -x);
  my_atomic_rwlock_wrunlock(&rwl);

  pthread_mutex_lock(&mutex);
  if (!--running_threads) pthread_cond_signal(&cond);
  pthread_mutex_unlock(&mutex);
  return 0;
}

/*
  same as test_atomic_add, but my_atomic_add32 is emulated with
  my_atomic_cas32 - notice that the slowdown is proportional to the
  number of CPUs
*/
pthread_handler_t test_atomic_cas(void *arg)
{
  int    m= (*(int *)arg)/2, ok= 0;
  GCC_BUG_WORKAROUND int32 x, y;
  for (x= ((int)(intptr)(&m)); m ; m--)
  {
    my_atomic_rwlock_wrlock(&rwl);
    y= my_atomic_load32(&bad);
    my_atomic_rwlock_wrunlock(&rwl);
    x= (x*m+0x87654321) & INT_MAX32;
    do {
      my_atomic_rwlock_wrlock(&rwl);
      ok= my_atomic_cas32(&bad, &y, (uint32)y+x);
      my_atomic_rwlock_wrunlock(&rwl);
    } while (!ok) ;
    do {
      my_atomic_rwlock_wrlock(&rwl);
      ok= my_atomic_cas32(&bad, &y, y-x);
      my_atomic_rwlock_wrunlock(&rwl);
    } while (!ok) ;
  }
  pthread_mutex_lock(&mutex);
  if (!--running_threads) pthread_cond_signal(&cond);
  pthread_mutex_unlock(&mutex);
  return 0;
}


void do_tests()
{
  plan(4);

  bad= my_atomic_initialize();
  ok(!bad, "my_atomic_initialize() returned %d", bad);

  my_atomic_rwlock_init(&rwl);

  b32= c32= 0;
  test_concurrently("my_atomic_add32", test_atomic_add, THREADS, CYCLES);
  b32= c32= 0;
  test_concurrently("my_atomic_fas32", test_atomic_fas, THREADS, CYCLES);
  b32= c32= 0;
  test_concurrently("my_atomic_cas32", test_atomic_cas, THREADS, CYCLES);

  my_atomic_rwlock_destroy(&rwl);
}
