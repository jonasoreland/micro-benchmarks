
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/time.h>
#include <math.h>

#ifndef __GNUC__
#error "Only GCC is supported!"
#endif

#define likely(x)	__builtin_expect((x),1)
#define unlikely(x)	__builtin_expect((x),0)

#define LOCK_VAL   1
#define UNLOCK_VAL 0
#define ALIGN 128
#define MAX_THREADS 256
#define OP_CNT 10

size_t
roundup(size_t val)
{
  size_t tmp = val & (ALIGN - 1);
  if (tmp)
  {
    val += (ALIGN - tmp);
  }
  return val;
}

#if defined(__GNUC__)

/** gcc */

#if defined(__x86_64__) || defined (__i386__)
/* Memory barriers, these definitions are for x64_64. */
#define mb()    asm volatile("mfence":::"memory")
/* According to Intel docs, it does not reorder loads. */
//#define rmb() asm volatile("lfence":::"memory")                               
#define rmb()   asm volatile("" ::: "memory")
#define wmb()   asm volatile("" ::: "memory")
#define read_barrier_depends()  do {} while(0)

static inline
int
xcng(volatile unsigned * addr, int val)
{
  asm volatile ("xchg %0, %1;" : "+r" (val) , "+m" (*addr));
  return val;
}
static
inline
void
cpu_pause()
{
  asm volatile ("rep;nop");
}

#define HAVE_XCNG
#define HAVE_XADD_MP
#define HAVE_XADD_UP
#define HAVE_GCC_INTRINSICS

static
inline
int
xadd_mp(volatile unsigned * addr, int val)
{
  asm volatile ("lock xadd %0,%1;" : "+r" (val) , "+m" (*addr));
} 

static
inline
int
xadd_up(volatile unsigned * addr, int val)
{
  asm volatile ("xadd %0,%1;" : "+r" (val) , "+m" (*addr));
} 

#elif defined(__sparc__)
#define mb()    asm volatile("membar #LoadLoad | #LoadStore | #StoreLoad | #StoreStore":::"memory")
#define rmb()   asm volatile("membar #LoadLoad" ::: "memory")
#define wmb()   asm volatile("membar #StoreStore" ::: "memory")
#define read_barrier_depends()  do {} while(0)

// link error if used incorrectly (i.e wo/ having NDB_HAVE_XCNG)
extern  int xcng(volatile unsigned * addr, int val);
extern void cpu_pause();

#include <atomic.h>
#define xadd_mp(a, v) atomic_add_32(a,v)

#define HAVE_XADD_MP

#else
#error "Unsupported architecture (gcc)"
#endif
#endif

struct thr_spin_lock
{
  volatile unsigned m_lock;
};

void
init(thr_spin_lock * sl)
{
  sl->m_lock = UNLOCK_VAL;
  mb();
}

#ifdef HAVE_XCNG
#define HAVE_SPINLOCK
#endif

#ifdef HAVE_SPINLOCK

static
void
lock_slow(struct thr_spin_lock* sl)
{
  unsigned int lockval = LOCK_VAL;
  volatile unsigned* val = &sl->m_lock;
test:
  do {
    cpu_pause();
  } while (* val == lockval);
  
  if (likely(xcng(val, lockval) == UNLOCK_VAL))
    return;
  
  goto test;
}

static
inline
void
lock(struct thr_spin_lock* sl)
{
  unsigned int lockval = LOCK_VAL;
  volatile unsigned* val = &sl->m_lock;
test:
  if (likely(xcng(val, lockval) == UNLOCK_VAL))
    return;

  lock_slow(sl);
}

static
inline
void
unlock(struct thr_spin_lock* sl)
{
  mb();
  sl->m_lock = UNLOCK_VAL;
}
#endif

struct my_counter
{
  union {
#ifdef HAVE_SPINLOCK
    struct thr_spin_lock spinlock;
#endif
    pthread_mutex_t mutex;
  };
  volatile unsigned counter;
};

int min_threads = 1, max_threads = 8, threads_step = 1;
unsigned long long iter = 10000000;
unsigned long long thr_iter = 1000000;
int locktype = 0; 
int loops = 5;
int duration = 3;

static
inline
unsigned long long now()
{
  struct timeval tmp;
  gettimeofday(&tmp, 0);
  unsigned long long ret = tmp.tv_sec;
  ret *= 1000000;
  ret += tmp.tv_usec;
  return ret;
}

static void * runspin(void*);
static void * runmutex(void*);
static void * runprivatemutex(void*);
static void * runatomic_mp(void*);
static void * runatomic_up(void*);
static void * runnop(void*);
static void * rungcc_sync_fetch_and_add(void*);
static void * runaddmb(void*);
static void * runadd(void*);
static void * (* runfunc)(void*);

int
main(int argc, char** argv)
{
  int i;
  char *name;
  unsigned long long sum, sum2;
  float var, std;
  for (i = 1; i<argc; i++)
  {
    if (strncmp(argv[i], "--threads=", sizeof("--threads")) == 0)
    {
      min_threads = atoi(argv[i] + sizeof("--threads"));
      max_threads = min_threads + 1;
    }
    else if (strncmp(argv[i], "--threads_lo=", sizeof("--threads_lo")) == 0)
    {
      min_threads = atoi(argv[i] + sizeof("--threads_lo"));
      if (max_threads <= min_threads)
        max_threads = min_threads + 1;
    }
    else if (strncmp(argv[i], "--threads_hi=", sizeof("--threads_hi")) == 0)
    {
      max_threads = atoi(argv[i] + sizeof("--threads_hi"));
      if (min_threads >= max_threads)
        min_threads = max_threads - 1;
      if (min_threads <= 0)
        min_threads = 1;
    }
    else if (strncmp(argv[i], "--threads_step=", sizeof("--threads_step")) == 0)
    {
      threads_step = atoi(argv[i] + sizeof("--threads_step"));
    }
    else if (strncmp(argv[i], "--time=", sizeof("--time")) == 0)
    {
      duration = atoi(argv[i] + sizeof("--time"));
    }
    else if (strncmp(argv[i], "--loops=", sizeof("--loops")) == 0)
    {
      loops = atoi(argv[i] + sizeof("--loops"));
    }
    else if (strcmp(argv[i], "--mutex_align") == 0)
    {
      locktype |= 1 << 0;
    }
    else if (strcmp(argv[i], "--mutex_non_align") == 0)
    {
      locktype |= 1 << 1;
    }
    else if (strcmp(argv[i], "--spin_align") == 0)
    {
      locktype |= 1 << 2;
    }
    else if (strcmp(argv[i], "--spin_non_align") == 0)
    {
      locktype |= 1 << 3;
    }
    else if (strcmp(argv[i], "--lock_xadd") == 0)
    {
      locktype |= 1 << 4;
    }
    else if (strcmp(argv[i], "--xadd") == 0)
    {
      locktype |= 1 << 5;
    }
    else if (strcmp(argv[i], "--gcc_sync_fetch_and_add") == 0)
    {
      locktype |= 1 << 6;
    }
    else if (strcmp(argv[i], "--add_mb") == 0)
    {
      locktype |= 1 << 7;
    }
    else if (strcmp(argv[i], "--add") == 0)
    {
      locktype |= 1 << 8;
    }
    else if (strcmp(argv[i], "--nop") == 0)
    {
      locktype |= 1 << 9;
    }
    else
    {
      printf("unknown argument >%s<\n", argv[i]); fflush(stdout);
      exit(1);
    }
  }

  if (locktype == 0)
    locktype = -1;

  printf("sizeof(pthread_mutex_t): %u\n", sizeof(pthread_mutex_t)); fflush(stdout);

  char* ptr0 = (char*)roundup((size_t)malloc(max_threads*roundup(sizeof(my_counter))+2*ALIGN));
  char* ptr1 = (char*)malloc(max_threads * sizeof(my_counter));

  struct my_counter *align[MAX_THREADS];
  struct my_counter *compact[MAX_THREADS];
  
  for (i = 0; i<max_threads; i++)
  {
    align[i] = (struct my_counter*)(ptr0 + i * roundup(sizeof(my_counter)));
    compact[i] = (struct my_counter*)(ptr1 + i * sizeof(my_counter));
  }

  /**
   * calibrate iteration by using mutex_align
   */
  {
    pthread_mutex_init(&align[0]->mutex, 0);
    printf("calibrating..."); fflush(stdout);
    unsigned long long start = now();
    iter = 0;
    thr_iter = 100000;
    do
    {
      runmutex(align[0]);
      iter += thr_iter;
    } while (now() < (start + 1000000*duration));
    pthread_mutex_destroy(&align[0]->mutex);
    printf("done. using %llu lock/unlock pairs\n", iter); fflush(stdout);
  }
  
  for (int lt = 0; lt < OP_CNT; lt++)
  {
    if ((locktype & (1 << lt)) == 0)
      continue;

    pthread_t thr_id[MAX_THREADS];
    struct my_counter ** base = 0;

    name = 0;
    switch(lt){
    case 0:
      name = "mutex_align";
      runfunc = runmutex;
      base = align;
      break;
    case 1:
      name = "mutex_non_align";
      runfunc = runmutex;
      base = compact;
      break;
    case 2:
#ifdef HAVE_SPINLOCK
      name = "spin_align";
      runfunc = runspin;
      base = align;
#endif
      break;
    case 3:
#ifdef HAVE_SPINLOCK
      name = "spin_non_align";
      runfunc = runspin;
      base = compact;
#endif
      break;
    case 4:
#ifdef HAVE_XADD_MP
      name = "lock_xadd";
      runfunc = runatomic_mp;
      base = align;
#endif
      break;
    case 5:
#ifdef HAVE_XADD_UP
      name = "xadd";
      runfunc = runatomic_up;
      base = align;
#endif
      break;
    case 6:
#ifdef HAVE_GCC_INTRINSICS
      name = "gcc_sync_fetch_and_add";
      runfunc = rungcc_sync_fetch_and_add;
      base = align;
#endif
      break;
    case 7:
      name = "add_mb";
      runfunc = runaddmb;
      base = align;
      break;
    case 8:
      name = "add";
      runfunc = runadd;
      base = align;
      break;
    case 9:
      name = "nop";
      runfunc = runnop;
      base = align;
      break;
    }

    if (name == 0)
      continue;

    for (i = 0; i<max_threads; i++)
    {
      base[i]->counter = 0;
      switch(lt){
      case 0:
      case 1:
        pthread_mutex_init(&base[i]->mutex, 0);
        break;
      case 2:
      case 3:
        init(&base[i]->spinlock);
        break;
      case 4:
      case 5:
      case 6:
      case 7:
      case 8:
        break;
      }
    }
    
    for (int t = min_threads; t < max_threads; t += threads_step)
    {
      int threads = t;
      thr_iter = (iter / threads);

      sum = 0;
      sum2 = 0;

      printf("%s threads: %u ", name, threads); fflush(stdout);
      
      for (int nn = 0; nn<loops; nn++)
      {
        unsigned long long start = now();
        
        for (i = 0; i<threads; i++)
        {
          void * arg = base[i];
          pthread_create(thr_id + i, 0, 
                         runfunc, arg); 
        }
        
        for (i = 0; i<threads; i++)
        {
          void * arg;
          pthread_join(thr_id[i], &arg);
        }
        
        unsigned long long stop = now();
        unsigned long long diff = (stop - start);
        sum += diff;
        sum2 += (diff * diff);
      }
      
      unsigned long long div = loops;
      var = sum2/div - (sum/div)*(sum/div);
      var *= 10/9;
      
      unsigned long long mops = (iter * div) / (sum ? sum : 1);
      unsigned long long ns = ((1000 * sum) / iter) / div;
      
      if (threads == 1)
      {
        printf("time: %llu (us) stddev: %u %u%% mops: %llu ns/op: %llu\n", 
               sum / div, 
               (unsigned)sqrt(var), 
               (unsigned)((100*sqrt(var)*div)/sum),
               mops,
               ns);
      }
      else
      {
        printf("time: %llu (us) stddev: %u %u%% mops: %llu\n", 
               sum / div, 
               (unsigned)sqrt(var), 
               (unsigned)((100*sqrt(var)*div)/sum),
               mops);
      }
    }
    
    for (i = 0; i<max_threads; i++)
    {
      switch(lt){
      case 0:
      case 1:
        pthread_mutex_destroy(&base[i]->mutex);
        break;
      }
    }
  }
  return 0;
}

static 
void * 
runaddmb (void* arg)
{
  struct my_counter * ptr = (struct my_counter*)arg;
  int i = thr_iter;
  while(i--) 
  {
    ptr->counter++;
    mb();
  }
  return 0;
}

static 
void * 
runadd (void* arg)
{
  struct my_counter * ptr = (struct my_counter*)arg;
  int i = thr_iter;
  while(i--) 
  {
    ptr->counter++;
  }
  return 0;
}

static 
void * 
runnop (void* unused)
{
  return 0;
}

static 
void * 
runatomic_mp (void* arg)
{
#ifdef HAVE_XADD_MP
  struct my_counter * ptr = (struct my_counter*)arg;
  int i = thr_iter;
  while(i--) xadd_mp(&ptr->counter, 1);
  return 0;
#else
  abort();
#endif
}

static
void *
runatomic_up (void* arg)
{
#ifdef HAVE_XADD_UP
  struct my_counter * ptr = (struct my_counter*)arg;
  int i = thr_iter;
  while(i--) xadd_up(&ptr->counter, 1);
  return 0;
#else
  abort();
#endif
}

static
void *
rungcc_sync_fetch_and_add (void* arg)
{
#ifdef HAVE_GCC_INTRINSICS
  struct my_counter * ptr = (struct my_counter*)arg;
  int i = thr_iter;
  while(i--) __sync_fetch_and_add(&ptr->counter, 1);
  return 0;
#else
  abort();
#endif
}

static
void *
runspin (void* arg)
{
#ifdef HAVE_SPINLOCK
  struct my_counter * ptr = (struct my_counter*)arg;
  int i = thr_iter;
  while (i--)
  {
    lock(&ptr->spinlock);
    ptr->counter++;
    unlock(&ptr->spinlock);
  }
  return 0;
#else
  abort();
#endif
}

static
void *
runmutex (void* arg)
{
  struct my_counter * ptr = (struct my_counter*)arg;
  int i = thr_iter;
  while (i--)
  {
    pthread_mutex_lock(&ptr->mutex);
    ptr->counter++;
    pthread_mutex_unlock(&ptr->mutex);
  }
  return 0;
}

