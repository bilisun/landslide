/**
 * @file arbiter.c
 * @author Ben Blum
 * @brief decision-making routines for landslide
 */

#include <stdio.h>
#include <stdlib.h>

#define MODULE_NAME "ARBITER"
#define MODULE_COLOUR COLOUR_YELLOW

#include "common.h"
#include "found_a_bug.h"
#include "kernel_specifics.h"
#include "kspec.h"
#include "landslide.h"
#include "memory.h"
#include "rand.h"
#include "schedule.h"
#include "user_specifics.h"
#include "user_sync.h"
#include "x86.h"

void arbiter_init(struct arbiter_state *r)
{
	Q_INIT_HEAD(&r->choices);
}

// FIXME: do these need to be threadsafe?
void arbiter_append_choice(struct arbiter_state *r, unsigned int tid)
{
	struct choice *c = MM_XMALLOC(1, struct choice);
	c->tid = tid;
	Q_INSERT_FRONT(&r->choices, c, nobe);
}

bool arbiter_pop_choice(struct arbiter_state *r, unsigned int *tid)
{
	struct choice *c = Q_GET_TAIL(&r->choices);
	if (c) {
		lsprintf(DEV, "using requested tid %d\n", c->tid);
		Q_REMOVE(&r->choices, c, nobe);
		*tid = c->tid;
		MM_FREE(c);
		return true;
	} else {
		return false;
	}
}

// TODO: move this to a data_race.c when that factor is done
static bool suspected_data_race(struct ls_state *ls)
{
	/* [i][0] is instruction pointer of the data race, and [i][1] is the
	 * most_recent_syscall value recorded when the race was observed. */
	static const unsigned int data_race_info[][2] = DATA_RACE_INFO;

	if (!check_user_address_space(ls)) {
		return false;
	}

	for (int i = 0; i < ARRAY_SIZE(data_race_info); i++) {
		if (KERNEL_MEMORY(data_race_info[i][0])) {
			assert(data_race_info[i][1] != 0);
		} else {
			assert(data_race_info[i][1] == 0);
		}

		if (ls->eip == data_race_info[i][0] &&
		    ls->sched.cur_agent->most_recent_syscall ==
		    data_race_info[i][1]) {
			return true;
		}
	}
	return false;
}

#define ASSERT_ONE_THREAD_PER_PP(ls) do {					\
		assert((/* root pp not created yet */				\
		        (ls)->save.next_tid == -1 ||				\
		        /* thread that was chosen is still running */		\
		        (ls)->save.next_tid == (ls)->sched.cur_agent->tid) &&	\
		       "One thread per preemption point invariant violated!");	\
	} while (0)

bool arbiter_interested(struct ls_state *ls, bool just_finished_reschedule,
			bool *voluntary, bool *need_handle_sleep, bool *data_race)
{
	*voluntary = false;
	*need_handle_sleep = false;
	*data_race = false;

	// TODO: more interesting choice points

	/* Attempt to see if a "voluntary" reschedule is just ending - did the
	 * last thread context switch not because of a timer?
	 * Also make sure to ignore null switches (timer-driven or not). */
	if (ls->sched.last_agent != NULL &&
	    !ls->sched.last_agent->action.handling_timer &&
	    ls->sched.last_agent != ls->sched.cur_agent &&
	    just_finished_reschedule) {
		lsprintf(DEV, "a voluntary reschedule: ");
		print_agent(DEV, ls->sched.last_agent);
		printf(DEV, " to ");
		print_agent(DEV, ls->sched.cur_agent);
		printf(DEV, "\n");
		if (ls->save.next_tid != ls->sched.last_agent->tid) {
			ASSERT_ONE_THREAD_PER_PP(ls);
		}
		assert(ls->sched.voluntary_resched_tid != -1);
		*voluntary = true;
		return true;
	/* is the kernel idling, e.g. waiting for keyboard input? */
	} else if (READ_BYTE(ls->cpu0, ls->eip) == OPCODE_HLT) {
		lskprintf(INFO, "What are you waiting for? (HLT state)\n");
		*need_handle_sleep = true;
		ASSERT_ONE_THREAD_PER_PP(ls);
		return true;
	/* Skip the instructions before the test case itself gets started. In
	 * many kernels' cases this will be redundant, but just in case. */
	} else if (!ls->test.test_ever_caused ||
		   ls->test.start_population == ls->sched.most_agents_ever) {
		return false;
	/* check for data races */
	} else if (suspected_data_race(ls)) {
		// FIXME: #88
		assert(!instruction_is_atomic_swap(ls->cpu0, ls->eip) &&
		       "Data races on xchg/atomic instructions is unsupported "
		       "-- see issue #88. Sorry!");
		*data_race = true;
		ASSERT_ONE_THREAD_PER_PP(ls);
		return true;
	/* user-mode-only preemption points */
	} else if (testing_userspace()) {
		unsigned int mutex_addr;
		if (KERNEL_MEMORY(ls->eip)) {
			return false;
		} else if (instruction_is_atomic_swap(ls->cpu0, ls->eip) &&
			   check_user_xchg(&ls->user_sync, ls->sched.cur_agent)) {
			/* User thread is blocked on an "xchg-continue" mutex.
			 * Analogous to HLT state -- need to preempt it. */
			ASSERT_ONE_THREAD_PER_PP(ls);
			return true;
		// FIXME Non-atomic busy loops should be handled more generally,
		// with the infinite loop detector, as detailed in #96.
		// This is a hack to make make_runnable work as a special case.
		} else if (user_make_runnable_entering(ls->eip) &&
			   check_user_xchg(&ls->user_sync, ls->sched.cur_agent)) {
			/* treat busy make-runnable loop same as xchg loop, in
			 * case of a misbehave mode that makes make-runnable NOT
			 * yield. if it does yield, NBD - the pp that arises
			 * will cause this spurious increment to get cleared. */
			ASSERT_ONE_THREAD_PER_PP(ls);
			return true;
		// TODO
		} else if ((user_mutex_lock_entering(ls->cpu0, ls->eip, &mutex_addr) ||
			    user_mutex_unlock_exiting(ls->eip)) &&
			   user_within_functions(ls)) {
			ASSERT_ONE_THREAD_PER_PP(ls);
			return true;
		} else {
			return false;
		}
	/* kernel-mode-only preemption points */
	} else if (kern_decision_point(ls->eip) &&
		   kern_within_functions(ls)) {
		ASSERT_ONE_THREAD_PER_PP(ls);
		return true;
	} else {
		return false;
	}
}

/* Returns true if a thread was chosen. If true, sets 'target' (to either the
 * current thread or any other thread), and sets 'our_choice' to false if
 * somebody else already made this choice for us, true otherwise. */
#define IS_IDLE(ls, a)							\
	(TID_IS_IDLE((a)->tid) &&					\
	 BUG_ON_THREADS_WEDGED != 0 && (ls)->test.test_ever_caused &&	\
	 (ls)->test.start_population != (ls)->sched.most_agents_ever)
bool arbiter_choose(struct ls_state *ls, struct agent *current,
		    struct agent **result, bool *our_choice)
{
	struct agent *a;
	unsigned int count = 0;
	bool current_is_legal_choice = false;

	/* We shouldn't be asked to choose if somebody else already did. */
	assert(Q_GET_SIZE(&ls->arbiter.choices) == 0);

	lsprintf(DEV, "Available choices: ");

	/* Count the number of available threads. */
	FOR_EACH_RUNNABLE_AGENT(a, &ls->sched,
		if (!BLOCKED(a) && !IS_IDLE(ls, a)) {
			print_agent(DEV, a);
			printf(DEV, " ");
			count++;
			if (a == current) {
				current_is_legal_choice = true;
			}
		}
	);

//#define CHOOSE_RANDOMLY
#define KEEP_RUNNING_YIELDING_THREADS

#ifdef CHOOSE_RANDOMLY
	// with given odds, will make the "forwards" choice.
	const int numerator   = 19;
	const int denominator = 20;
	if (rand64(&ls->rand) % denominator < numerator) {
		count = 1;
	}
#else
	if (EXPLORE_BACKWARDS == 0) {
		count = 1;
	}
#endif

#ifdef KEEP_RUNNING_YIELDING_THREADS
	if (current_is_legal_choice && agent_has_yielded(&current->user_yield)) {
		printf(DEV, "- Must run yielding thread %d\n", current->tid);
		*result = current;
		*our_choice = true;
		return true;
	}
#endif

	/* Find the count-th thread. */
	unsigned int i = 0;
	FOR_EACH_RUNNABLE_AGENT(a, &ls->sched,
		if (!BLOCKED(a) && !IS_IDLE(ls, a) && ++i == count) {
			printf(DEV, "- Figured I'd look at TID %d next.\n",
			       a->tid);
			*result = a;
			*our_choice = true;
			return true;
		}
	);

	/* No runnable threads. Is this a bug, or is it expected? */
	if (BUG_ON_THREADS_WEDGED != 0 &&
	    anybody_alive(ls->cpu0, &ls->test, &ls->sched, true)) {
		FOUND_A_BUG(ls, "Deadlock -- no threads are runnable!\n");
	} else {
		printf(DEV, "Deadlock -- no threads are runnable!");
	}
	return false;
}
