/*****************************************************************************
 * thread.c : pthread back-end for LibVLC
 *****************************************************************************
 * Copyright (C) 1999-2009 VLC authors and VideoLAN
 *
 * Authors: Jean-Marc Dressler <polux@via.ecp.fr>
 *          Samuel Hocevar <sam@zoy.org>
 *          Gildas Bazin <gbazin@netcourrier.com>
 *          Clément Sténac
 *          Rémi Denis-Courmont
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_atomic.h>

#include "libvlc.h"
#include <stdarg.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

#include <sys/types.h>
#include <unistd.h> /* fsync() */
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>

#if defined (_POSIX_PRIORITY_SCHEDULING) && (_POSIX_PRIORITY_SCHEDULING >= 0) \
 && defined (_POSIX_THREAD_PRIORITY_SCHEDULING) \
 && (_POSIX_THREAD_PRIORITY_SCHEDULING >= 0)
# define VLC_RT_SCHEDULING 1
#endif

#if defined (VLC_RT_SCHEDULING) && defined (HAVE_DBUS)
# include <dbus/dbus.h>
# include <sys/syscall.h>
# define VLC_RTKIT 1
#endif

#ifdef HAVE_EXECINFO_H
# include <execinfo.h>
#endif
#if defined(__SunOS)
# include <sys/processor.h>
# include <sys/pset.h>
#endif

#if !defined (_POSIX_TIMERS)
# define _POSIX_TIMERS (-1)
#endif
#if !defined (_POSIX_CLOCK_SELECTION)
/* Clock selection was defined in 2001 and became mandatory in 2008. */
# define _POSIX_CLOCK_SELECTION (-1)
#endif
#if !defined (_POSIX_MONOTONIC_CLOCK)
# define _POSIX_MONOTONIC_CLOCK (-1)
#endif

#if (_POSIX_TIMERS > 0)
static unsigned vlc_clock_prec;

# if (_POSIX_MONOTONIC_CLOCK > 0) && (_POSIX_CLOCK_SELECTION > 0)
/* Compile-time POSIX monotonic clock support */
#  define vlc_clock_id (CLOCK_MONOTONIC)

# elif (_POSIX_MONOTONIC_CLOCK == 0) && (_POSIX_CLOCK_SELECTION > 0)
/* Run-time POSIX monotonic clock support (see clock_setup() below) */
static clockid_t vlc_clock_id;

# else
/* No POSIX monotonic clock support */
#   define vlc_clock_id (CLOCK_REALTIME)
#   warning Monotonic clock not available. Expect timing issues.

# endif /* _POSIX_MONOTONIC_CLOKC */

static void vlc_clock_setup_once (void)
{
# if (_POSIX_MONOTONIC_CLOCK == 0)
    long val = sysconf (_SC_MONOTONIC_CLOCK);
    assert (val != 0);
    vlc_clock_id = (val < 0) ? CLOCK_REALTIME : CLOCK_MONOTONIC;
# endif

    struct timespec res;
    if (unlikely(clock_getres (vlc_clock_id, &res) != 0 || res.tv_sec != 0))
        abort ();
    vlc_clock_prec = (res.tv_nsec + 500) / 1000;
}

static pthread_once_t vlc_clock_once = PTHREAD_ONCE_INIT;

# define vlc_clock_setup() \
    pthread_once(&vlc_clock_once, vlc_clock_setup_once)

#else /* _POSIX_TIMERS */

# include <sys/time.h> /* gettimeofday() */

# define vlc_clock_setup() (void)0
# warning Monotonic clock not available. Expect timing issues.
#endif /* _POSIX_TIMERS */

static struct timespec mtime_to_ts (vlc_tick_t date)
{
    lldiv_t d = lldiv (date, CLOCK_FREQ);
    struct timespec ts = { d.quot, d.rem * (1000000000 / CLOCK_FREQ) };

    return ts;
}

/**
 * Print a backtrace to the standard error for debugging purpose.
 */
void vlc_trace (const char *fn, const char *file, unsigned line)
{
     fprintf (stderr, "at %s:%u in %s\n", file, line, fn);
     fflush (stderr); /* needed before switch to low-level I/O */
#ifdef HAVE_BACKTRACE
     void *stack[20];
     int len = backtrace (stack, sizeof (stack) / sizeof (stack[0]));
     backtrace_symbols_fd (stack, len, 2);
#endif
     fsync (2);
}

#ifndef NDEBUG
/**
 * Reports a fatal error from the threading layer, for debugging purposes.
 */
static void
vlc_thread_fatal (const char *action, int error,
                  const char *function, const char *file, unsigned line)
{
    int canc = vlc_savecancel ();
    fprintf (stderr, "LibVLC fatal error %s (%d) in thread %lu ",
             action, error, vlc_thread_id ());
    vlc_trace (function, file, line);
    perror ("Thread error");
    fflush (stderr);

    vlc_restorecancel (canc);
    abort ();
}

# define VLC_THREAD_ASSERT( action ) \
    if (unlikely(val)) \
        vlc_thread_fatal (action, val, __func__, __FILE__, __LINE__)
#else
# define VLC_THREAD_ASSERT( action ) ((void)val)
#endif

void vlc_mutex_init( vlc_mutex_t *p_mutex )
{
    pthread_mutexattr_t attr;

    if (unlikely(pthread_mutexattr_init (&attr)))
        abort();
#ifdef NDEBUG
    pthread_mutexattr_settype (&attr, PTHREAD_MUTEX_DEFAULT);
#else
    pthread_mutexattr_settype (&attr, PTHREAD_MUTEX_ERRORCHECK);
#endif
    if (unlikely(pthread_mutex_init (p_mutex, &attr)))
        abort();
    pthread_mutexattr_destroy( &attr );
}

void vlc_mutex_init_recursive( vlc_mutex_t *p_mutex )
{
    pthread_mutexattr_t attr;

    if (unlikely(pthread_mutexattr_init (&attr)))
        abort();
    pthread_mutexattr_settype (&attr, PTHREAD_MUTEX_RECURSIVE);
    if (unlikely(pthread_mutex_init (p_mutex, &attr)))
        abort();
    pthread_mutexattr_destroy( &attr );
}

void vlc_mutex_destroy (vlc_mutex_t *p_mutex)
{
    int val = pthread_mutex_destroy( p_mutex );
    VLC_THREAD_ASSERT ("destroying mutex");
}

#ifndef NDEBUG
# ifdef HAVE_VALGRIND_VALGRIND_H
#  include <valgrind/valgrind.h>
# else
#  define RUNNING_ON_VALGRIND (0)
# endif

/**
 * Asserts that a mutex is locked by the calling thread.
 */
void vlc_assert_locked (vlc_mutex_t *p_mutex)
{
    if (RUNNING_ON_VALGRIND > 0)
        return;
    assert (pthread_mutex_lock (p_mutex) == EDEADLK);
}
#endif

void vlc_mutex_lock (vlc_mutex_t *p_mutex)
{
    int val = pthread_mutex_lock( p_mutex );
    VLC_THREAD_ASSERT ("locking mutex");
}

int vlc_mutex_trylock (vlc_mutex_t *p_mutex)
{
    int val = pthread_mutex_trylock( p_mutex );

    if (val != EBUSY)
        VLC_THREAD_ASSERT ("locking mutex");
    return val;
}

void vlc_mutex_unlock (vlc_mutex_t *p_mutex)
{
    int val = pthread_mutex_unlock( p_mutex );
    VLC_THREAD_ASSERT ("unlocking mutex");
}

void vlc_cond_init (vlc_cond_t *p_condvar)
{
    pthread_condattr_t attr;

    if (unlikely(pthread_condattr_init (&attr)))
        abort ();
#if (_POSIX_CLOCK_SELECTION > 0)
    vlc_clock_setup ();
    pthread_condattr_setclock (&attr, vlc_clock_id);
#endif
    if (unlikely(pthread_cond_init (p_condvar, &attr)))
        abort ();
    pthread_condattr_destroy (&attr);
}

void vlc_cond_init_daytime (vlc_cond_t *p_condvar)
{
    if (unlikely(pthread_cond_init (p_condvar, NULL)))
        abort ();
}

void vlc_cond_destroy (vlc_cond_t *p_condvar)
{
    int val = pthread_cond_destroy( p_condvar );
    VLC_THREAD_ASSERT ("destroying condition");
}

void vlc_cond_signal (vlc_cond_t *p_condvar)
{
    int val = pthread_cond_signal( p_condvar );
    VLC_THREAD_ASSERT ("signaling condition variable");
}

void vlc_cond_broadcast (vlc_cond_t *p_condvar)
{
    pthread_cond_broadcast (p_condvar);
}

void vlc_cond_wait (vlc_cond_t *p_condvar, vlc_mutex_t *p_mutex)
{
    int val = pthread_cond_wait( p_condvar, p_mutex );
    VLC_THREAD_ASSERT ("waiting on condition");
}

int vlc_cond_timedwait (vlc_cond_t *p_condvar, vlc_mutex_t *p_mutex,
                        vlc_tick_t deadline)
{
    struct timespec ts = mtime_to_ts (deadline);
    int val = pthread_cond_timedwait (p_condvar, p_mutex, &ts);
    if (val != ETIMEDOUT)
        VLC_THREAD_ASSERT ("timed-waiting on condition");
    return val;
}

int vlc_cond_timedwait_daytime (vlc_cond_t *p_condvar, vlc_mutex_t *p_mutex,
                                time_t deadline)
{
    struct timespec ts = { deadline, 0 };
    int val = pthread_cond_timedwait (p_condvar, p_mutex, &ts);
    if (val != ETIMEDOUT)
        VLC_THREAD_ASSERT ("timed-waiting on condition");
    return val;
}

void vlc_sem_init (vlc_sem_t *sem, unsigned value)
{
    if (unlikely(sem_init (sem, 0, value)))
        abort ();
}

void vlc_sem_destroy (vlc_sem_t *sem)
{
    int val;

    if (likely(sem_destroy (sem) == 0))
        return;

    val = errno;

    VLC_THREAD_ASSERT ("destroying semaphore");
}

int vlc_sem_post (vlc_sem_t *sem)
{
    int val;

    if (likely(sem_post (sem) == 0))
        return 0;

    val = errno;

    if (unlikely(val != EOVERFLOW))
        VLC_THREAD_ASSERT ("unlocking semaphore");
    return val;
}

void vlc_sem_wait (vlc_sem_t *sem)
{
    int val;

    do
        if (likely(sem_wait (sem) == 0))
            return;
    while ((val = errno) == EINTR);

    VLC_THREAD_ASSERT ("locking semaphore");
}

void vlc_rwlock_init (vlc_rwlock_t *lock)
{
    if (unlikely(pthread_rwlock_init (lock, NULL)))
        abort ();
}

void vlc_rwlock_destroy (vlc_rwlock_t *lock)
{
    int val = pthread_rwlock_destroy (lock);
    VLC_THREAD_ASSERT ("destroying R/W lock");
}

void vlc_rwlock_rdlock (vlc_rwlock_t *lock)
{
    int val = pthread_rwlock_rdlock (lock);
    VLC_THREAD_ASSERT ("acquiring R/W lock for reading");
}

void vlc_rwlock_wrlock (vlc_rwlock_t *lock)
{
    int val = pthread_rwlock_wrlock (lock);
    VLC_THREAD_ASSERT ("acquiring R/W lock for writing");
}

void vlc_rwlock_unlock (vlc_rwlock_t *lock)
{
    int val = pthread_rwlock_unlock (lock);
    VLC_THREAD_ASSERT ("releasing R/W lock");
}

int vlc_threadvar_create (vlc_threadvar_t *key, void (*destr) (void *))
{
    return pthread_key_create (key, destr);
}

void vlc_threadvar_delete (vlc_threadvar_t *p_tls)
{
    pthread_key_delete (*p_tls);
}

int vlc_threadvar_set (vlc_threadvar_t key, void *value)
{
    return pthread_setspecific (key, value);
}

void *vlc_threadvar_get (vlc_threadvar_t key)
{
    return pthread_getspecific (key);
}

#ifdef VLC_RT_SCHEDULING
static bool rt_priorities = false;
static int rt_offset;
static int rt_max; /* the highest real-time priority we may ask for */
#ifdef VLC_RTKIT
static bool rt_via_rtkit = false; /* ask RealtimeKit rather than the kernel */
#endif

/* Turn one of the VLC_THREAD_PRIORITY_* values into a policy and a priority,
 * never above what this process is allowed to ask for. */
static int vlc_sched_param (int priority, struct sched_param *restrict sp)
{
    int policy;

    sp->sched_priority = priority + rt_offset;

    if (sp->sched_priority <= 0)
        sp->sched_priority += sched_get_priority_max (policy = SCHED_OTHER);
    else
    {
        sp->sched_priority += sched_get_priority_min (policy = SCHED_RR);

        if (sp->sched_priority > rt_max)
            sp->sched_priority = rt_max;
    }

    return policy;
}

/* Whether the kernel will grant this process a real-time priority at all.
 * Asked rather than inferred: RLIMIT_RTPRIO is not the only way to have it,
 * CAP_SYS_NICE and running as root both work with the limit at zero. */
static bool vlc_can_schedule_rt (void)
{
    struct sched_param sp = {
        .sched_priority = sched_get_priority_min (SCHED_RR),
    }, old_sp;
    int old_policy;

    if (pthread_getschedparam (pthread_self (), &old_policy, &old_sp) != 0
     || pthread_setschedparam (pthread_self (), SCHED_RR, &sp) != 0)
        return false;

    pthread_setschedparam (pthread_self (), old_policy, &old_sp);
    return true;
}

/* A real-time thread that stops yielding locks a processor out for as long
 * as it runs. RLIMIT_RTTIME is the kernel's answer to that: a thread that
 * holds a processor for this long without blocking is signalled. A second
 * of that is nothing an audio thread does, and killing the process beats
 * wedging the machine. Only lowered from infinity, so a figure someone set
 * deliberately is left alone. */
static void vlc_bound_rt_runtime (void)
{
    struct rlimit rl;

#ifdef RLIMIT_RTTIME
    if (getrlimit (RLIMIT_RTTIME, &rl) != 0 || rl.rlim_cur != RLIM_INFINITY)
        return;

    rl.rlim_cur = 1000000;
    setrlimit (RLIMIT_RTTIME, &rl);
#else
    (void) rl;
#endif
}
#endif

#ifdef VLC_RTKIT
/* RealtimeKit hands out real-time priorities to processes that may not take
 * them themselves, which on an ordinary desktop is all of them. It only does
 * so for a thread whose run time is bounded (RLIMIT_RTTIME), and only up to
 * a priority of its own choosing, so both have to be asked for first. */
# define RTKIT_NAME "org.freedesktop.RealtimeKit1"
# define RTKIT_PATH "/org/freedesktop/RealtimeKit1"

static DBusConnection *rtkit_bus = NULL;
static vlc_mutex_t rtkit_lock = VLC_STATIC_MUTEX;

static bool rtkit_get_int (const char *name, int64_t *restrict value)
{
    DBusMessage *req = dbus_message_new_method_call (RTKIT_NAME, RTKIT_PATH,
                                     "org.freedesktop.DBus.Properties", "Get");
    if (unlikely(req == NULL))
        return false;

    const char *iface = RTKIT_NAME;
    bool ok = false;

    if (dbus_message_append_args (req, DBUS_TYPE_STRING, &iface,
                                  DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID))
    {
        DBusMessage *ans = dbus_connection_send_with_reply_and_block (rtkit_bus,
                                                             req, 1000, NULL);
        if (ans != NULL)
        {
            DBusMessageIter it, var;

            if (dbus_message_iter_init (ans, &it)
             && dbus_message_iter_get_arg_type (&it) == DBUS_TYPE_VARIANT)
            {
                dbus_message_iter_recurse (&it, &var);

                switch (dbus_message_iter_get_arg_type (&var))
                {
                    case DBUS_TYPE_INT32:
                    {
                        dbus_int32_t i32;
                        dbus_message_iter_get_basic (&var, &i32);
                        *value = i32;
                        ok = true;
                        break;
                    }
                    case DBUS_TYPE_INT64:
                    {
                        dbus_int64_t i64;
                        dbus_message_iter_get_basic (&var, &i64);
                        *value = i64;
                        ok = true;
                        break;
                    }
                }
            }
            dbus_message_unref (ans);
        }
    }

    dbus_message_unref (req);
    return ok;
}

/* Connects to RealtimeKit and takes it at its word: its ceiling becomes ours,
 * and the run time it insists on is set before anything is asked for. */
static bool rtkit_setup (void)
{
    int64_t max_priority, max_runtime;
    struct rlimit rl;

    dbus_threads_init_default (); /* libdbus 1.6 does not do this itself */
    rtkit_bus = dbus_bus_get_private (DBUS_BUS_SYSTEM, NULL);
    if (rtkit_bus == NULL)
        return false;

    dbus_connection_set_exit_on_disconnect (rtkit_bus, FALSE);

    if (!rtkit_get_int ("MaxRealtimePriority", &max_priority)
     || !rtkit_get_int ("RTTimeUSecMax", &max_runtime)
     || max_priority < 1 || max_runtime < 1)
        goto error;

    if (getrlimit (RLIMIT_RTTIME, &rl) != 0)
        goto error;

    if (rl.rlim_cur > (rlim_t)max_runtime)
    {
        rl.rlim_cur = max_runtime;
        if (rl.rlim_max > (rlim_t)max_runtime)
            rl.rlim_max = max_runtime;
        if (setrlimit (RLIMIT_RTTIME, &rl) != 0)
            goto error;
    }

    if (max_priority < rt_max)
        rt_max = max_priority;
    return true;

error:
    dbus_connection_close (rtkit_bus);
    dbus_connection_unref (rtkit_bus);
    rtkit_bus = NULL;
    return false;
}

/* Asks for the calling thread, which is the only one whose kernel thread
 * identifier is to hand. A refusal is not worth reporting: the thread keeps
 * the priority it already has and plays on. */
static void rtkit_make_realtime (int priority)
{
    DBusMessage *req = dbus_message_new_method_call (RTKIT_NAME, RTKIT_PATH,
                                     RTKIT_NAME, "MakeThreadRealtimeWithPID");
    if (unlikely(req == NULL))
        return;

    dbus_uint64_t pid = getpid ();
    dbus_uint64_t tid = syscall (SYS_gettid);
    dbus_uint32_t prio = priority;

    if (dbus_message_append_args (req, DBUS_TYPE_UINT64, &pid,
                                  DBUS_TYPE_UINT64, &tid,
                                  DBUS_TYPE_UINT32, &prio, DBUS_TYPE_INVALID))
    {
        int canc = vlc_savecancel ();

        vlc_mutex_lock (&rtkit_lock);
        DBusMessage *ans = dbus_connection_send_with_reply_and_block (rtkit_bus,
                                                             req, 1000, NULL);
        vlc_mutex_unlock (&rtkit_lock);
        vlc_restorecancel (canc);

        if (ans != NULL)
            dbus_message_unref (ans);
    }

    dbus_message_unref (req);
}

struct vlc_thread_rt
{
    void *(*entry) (void *);
    void *data;
    int priority;
};

static void *vlc_thread_rt_entry (void *opaque)
{
    struct vlc_thread_rt *boot = opaque;
    void *(*entry) (void *) = boot->entry;
    void *data = boot->data;

    rtkit_make_realtime (boot->priority);
    free (boot);

    return entry (data);
}
#endif

void vlc_threads_setup (libvlc_int_t *p_libvlc)
{
    static vlc_mutex_t lock = VLC_STATIC_MUTEX;
    static bool initialized = false;

    vlc_mutex_lock (&lock);
    /* Initializes real-time priorities before any thread is created,
     * just once per process. */
    if (!initialized)
    {
#ifdef VLC_RT_SCHEDULING
        if (var_InheritBool (p_libvlc, "rt-priority"))
        {
            struct rlimit rl;

            rt_offset = var_InheritInteger (p_libvlc, "rt-offset");
            rt_max = sched_get_priority_max (SCHED_RR);

#ifdef RLIMIT_RTPRIO
            if (getrlimit (RLIMIT_RTPRIO, &rl) == 0
             && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur > 0
             && (int)rl.rlim_cur < rt_max)
                rt_max = rl.rlim_cur;
#else
            (void) rl;
#endif

            rt_priorities = vlc_can_schedule_rt ();

            if (rt_priorities)
                vlc_bound_rt_runtime ();
#ifdef VLC_RTKIT
            else if (rtkit_setup ())
            {
                rt_priorities = true;
                rt_via_rtkit = true;
                msg_Dbg (p_libvlc, "asking RealtimeKit for real-time "
                         "priorities up to %d", rt_max);
            }
#endif
            else
                msg_Dbg (p_libvlc, "real-time scheduling not permitted, "
                         "running at the default priority");
        }
#endif
        initialized = true;
    }
    vlc_mutex_unlock (&lock);
}


static int vlc_clone_attr (vlc_thread_t *th, pthread_attr_t *attr,
                           void *(*entry) (void *), void *data, int priority)
{
    int ret;

    /* Block the signals that signals interface plugin handles.
     * If the LibVLC caller wants to handle some signals by itself, it should
     * block these before whenever invoking LibVLC. And it must obviously not
     * start the VLC signals interface plugin.
     *
     * LibVLC will normally ignore any interruption caused by an asynchronous
     * signal during a system call. But there may well be some buggy cases
     * where it fails to handle EINTR (bug reports welcome). Some underlying
     * libraries might also not handle EINTR properly.
     */
    sigset_t oldset;
    {
        sigset_t set;
        sigemptyset (&set);
        sigdelset (&set, SIGHUP);
        sigaddset (&set, SIGINT);
        sigaddset (&set, SIGQUIT);
        sigaddset (&set, SIGTERM);

        sigaddset (&set, SIGPIPE); /* We don't want this one, really! */
        pthread_sigmask (SIG_BLOCK, &set, &oldset);
    }

#ifdef VLC_RT_SCHEDULING
    if (rt_priorities)
    {
        struct sched_param sp;
        int policy = vlc_sched_param (priority, &sp);

#ifdef VLC_RTKIT
        if (rt_via_rtkit)
        {   /* RealtimeKit promotes a running thread by its kernel thread
             * identifier, so the thread has to ask for itself once it is
             * running. Only the ones that would be real-time need to. */
            struct vlc_thread_rt *boot = (policy == SCHED_RR)
                                         ? malloc (sizeof (*boot)) : NULL;

            if (boot != NULL)
            {
                boot->entry = entry;
                boot->data = data;
                boot->priority = sp.sched_priority;
                entry = vlc_thread_rt_entry;
                data = boot;
            }
        }
        else
#endif
        {
            pthread_attr_setschedpolicy (attr, policy);
            pthread_attr_setschedparam (attr, &sp);
            pthread_attr_setinheritsched (attr, PTHREAD_EXPLICIT_SCHED);
        }
    }
#else
    (void) priority;
#endif

    /* The thread stack size.
     * The lower the value, the less address space per thread, the highest
     * maximum simultaneous threads per process. Too low values will cause
     * stack overflows and weird crashes. Set with caution. Also keep in mind
     * that 64-bits platforms consume more stack than 32-bits one.
     *
     * Thanks to on-demand paging, thread stack size only affects address space
     * consumption. In terms of memory, threads only use what they need
     * (rounded up to the page boundary).
     *
     * For example, on Linux i386, the default is 2 mega-bytes, which supports
     * about 320 threads per processes. */
#define VLC_STACKSIZE (128 * sizeof (void *) * 1024)

#ifdef VLC_STACKSIZE
    ret = pthread_attr_setstacksize (attr, VLC_STACKSIZE);
    assert (ret == 0); /* fails iif VLC_STACKSIZE is invalid */
#endif

    ret = pthread_create(&th->handle, attr, entry, data);
#ifdef VLC_RT_SCHEDULING
    if (ret == EPERM && rt_priorities)
    {   /* The kernel refused the priority, not the thread. Playing at the
         * default priority beats not playing. */
        pthread_attr_setinheritsched (attr, PTHREAD_INHERIT_SCHED);
        ret = pthread_create(&th->handle, attr, entry, data);
    }
#endif
    pthread_sigmask (SIG_SETMASK, &oldset, NULL);
    pthread_attr_destroy (attr);
    return ret;
}

int vlc_clone (vlc_thread_t *th, void *(*entry) (void *), void *data,
               int priority)
{
    pthread_attr_t attr;

    pthread_attr_init (&attr);
    return vlc_clone_attr (th, &attr, entry, data, priority);
}

void vlc_join(vlc_thread_t th, void **result)
{
    int val = pthread_join(th.handle, result);
    VLC_THREAD_ASSERT ("joining thread");
}

/**
 * Creates and starts new detached thread.
 * A detached thread cannot be joined. Its resources will be automatically
 * released whenever the thread exits (in particular, its call stack will be
 * reclaimed).
 *
 * Detached thread are particularly useful when some work needs to be done
 * asynchronously, that is likely to be completed much earlier than the thread
 * can practically be joined. In this case, thread detach can spare memory.
 *
 * A detached thread may be cancelled, so as to expedite its termination.
 * Be extremely careful if you do this: while a normal joinable thread can
 * safely be cancelled after it has already exited, cancelling an already
 * exited detached thread is undefined: The thread handle would is destroyed
 * immediately when the detached thread exits. So you need to ensure that the
 * detached thread is still running before cancellation is attempted.
 *
 * @warning Care must be taken that any resources used by the detached thread
 * remains valid until the thread completes.
 *
 * @note A detached thread must eventually exit just like another other
 * thread. In practice, LibVLC will wait for detached threads to exit before
 * it unloads the plugins.
 *
 * @param th [OUT] pointer to hold the thread handle, or NULL
 * @param entry entry point for the thread
 * @param data data parameter given to the entry point
 * @param priority thread priority value
 * @return 0 on success, a standard error code on error.
 */
int vlc_clone_detach (vlc_thread_t *th, void *(*entry) (void *), void *data,
                      int priority)
{
    vlc_thread_t dummy;
    pthread_attr_t attr;

    if (th == NULL)
        th = &dummy;

    pthread_attr_init (&attr);
    pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_DETACHED);
    return vlc_clone_attr (th, &attr, entry, data, priority);
}

vlc_thread_t vlc_thread_self (void)
{
    vlc_thread_t thread = { pthread_self() };
    return thread;
}

#if !defined (__linux__)
unsigned long vlc_thread_id (void)
{
     return -1;
}
#endif

int vlc_set_priority (vlc_thread_t th, int priority)
{
#ifdef VLC_RT_SCHEDULING
    if (rt_priorities)
    {
        struct sched_param sp;
        int policy = vlc_sched_param (priority, &sp);

        if (pthread_setschedparam(th.handle, policy, &sp))
            return VLC_EGENERIC;
    }
#else
    (void) th; (void) priority;
#endif
    return VLC_SUCCESS;
}

void vlc_cancel(vlc_thread_t th)
{
    pthread_cancel(th.handle);
}

int vlc_savecancel (void)
{
    int state;
    int val = pthread_setcancelstate (PTHREAD_CANCEL_DISABLE, &state);

    VLC_THREAD_ASSERT ("saving cancellation");
    return state;
}

void vlc_restorecancel (int state)
{
#ifndef NDEBUG
    int oldstate, val;

    val = pthread_setcancelstate (state, &oldstate);
    /* This should fail if an invalid value for given for state */
    VLC_THREAD_ASSERT ("restoring cancellation");

    if (unlikely(oldstate != PTHREAD_CANCEL_DISABLE))
         vlc_thread_fatal ("restoring cancellation while not disabled", EINVAL,
                           __func__, __FILE__, __LINE__);
#else
    pthread_setcancelstate (state, NULL);
#endif
}

void vlc_testcancel (void)
{
    pthread_testcancel ();
}

void vlc_control_cancel (int cmd, ...)
{
    (void) cmd;
    vlc_assert_unreachable ();
}

vlc_tick_t mdate (void)
{
#if (_POSIX_TIMERS > 0)
    struct timespec ts;

    vlc_clock_setup ();
    if (unlikely(clock_gettime (vlc_clock_id, &ts) != 0))
        abort ();

    return (INT64_C(1000000) * ts.tv_sec) + (ts.tv_nsec / 1000);

#else
    struct timeval tv;

    if (unlikely(gettimeofday (&tv, NULL) != 0))
        abort ();
    return (INT64_C(1000000) * tv.tv_sec) + tv.tv_usec;

#endif
}

#undef mwait
void mwait (vlc_tick_t deadline)
{
#if (_POSIX_CLOCK_SELECTION > 0)
    vlc_clock_setup ();
    /* If the deadline is already elapsed, or within the clock precision,
     * do not even bother the system timer. */
    deadline -= vlc_clock_prec;

    struct timespec ts = mtime_to_ts (deadline);

    while (clock_nanosleep (vlc_clock_id, TIMER_ABSTIME, &ts, NULL) == EINTR);

#else
    deadline -= mdate ();
    if (deadline > 0)
        msleep (deadline);

#endif
}

#undef msleep
void msleep (vlc_tick_t delay)
{
    struct timespec ts = mtime_to_ts (delay);

#if (_POSIX_CLOCK_SELECTION > 0)
    vlc_clock_setup ();
    while (clock_nanosleep (vlc_clock_id, 0, &ts, &ts) == EINTR);

#else
    while (nanosleep (&ts, &ts) == -1)
        assert (errno == EINTR);

#endif
}

unsigned vlc_GetCPUCount(void)
{
#if defined(HAVE_SCHED_GETAFFINITY)
    cpu_set_t cpu;

    CPU_ZERO(&cpu);
    if (sched_getaffinity (0, sizeof (cpu), &cpu) < 0)
        return 1;

    return CPU_COUNT (&cpu);

#elif defined(__SunOS)
    unsigned count = 0;
    int type;
    u_int numcpus;
    processor_info_t cpuinfo;

    processorid_t *cpulist = vlc_alloc (sysconf(_SC_NPROCESSORS_MAX), sizeof (*cpulist));
    if (unlikely(cpulist == NULL))
        return 1;

    if (pset_info(PS_MYID, &type, &numcpus, cpulist) == 0)
    {
        for (u_int i = 0; i < numcpus; i++)
            if (processor_info (cpulist[i], &cpuinfo) == 0)
                count += (cpuinfo.pi_state == P_ONLINE);
    }
    else
        count = sysconf (_SC_NPROCESSORS_ONLN);
    free (cpulist);
    return count ? count : 1;
#elif defined(_SC_NPROCESSORS_ONLN)
    return sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(_SC_NPROCESSORS_CONF)
    return sysconf(_SC_NPROCESSORS_CONF);
#else
#   warning "vlc_GetCPUCount is not implemented for your platform"
    return 1;
#endif
}
