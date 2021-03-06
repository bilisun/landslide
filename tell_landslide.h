/**
 * @file tell_landslide.h
 * @brief Specification for pebbles kernels to tell landslide about themselves.
 * @author Ben Blum <bblum@andrew.cmu.edu>
 */

#ifndef __LS_TELL_LANDSLIDE_H
#define __LS_TELL_LANDSLIDE_H

/* Call this to indicate a "preemption point" in your kernel's execution. */
void tell_landslide_preempt(void);

void tell_landslide_thread_switch(int new_tid);
void tell_landslide_sched_init_done(void);
void tell_landslide_forking(void);
void tell_landslide_vanishing(void);
void tell_landslide_sleeping(void);
void tell_landslide_thread_on_rq(int tid);
void tell_landslide_thread_off_rq(int tid);

/* You should use this one IF AND ONLY IF you have mutexes that leave blocked
 * threads on the runqueue (e.g., they yield-to-owner). */
void tell_landslide_mutex_blocking(int owner_tid);

/* You should use these if you used blocking(), above, or if you wish to turn
 * on the data race detector (PRINT_DATA_RACES=1). Otherwise not necessary. */
void tell_landslide_mutex_locking(void *mutex_addr);
void tell_landslide_mutex_locking_done(void *mutex_addr);
void tell_landslide_mutex_unlocking(void *mutex_addr);
void tell_landslide_mutex_unlocking_done(void);
void tell_landslide_mutex_trylocking(void *mutex_addr);
void tell_landslide_mutex_trylocking_done(void *mutex_addr, int succeeded); /* 1 true, 0 false */

/* Bonus annotations */
void tell_landslide_dump_stack(void);

#endif
