#!/bin/sh

# this auto-generates a chunk of the kernel_specifics.c file.
# comments can be found there... (sorry)

KERNEL_IMG=$1
KERNEL_NAME=$2

if [ ! -f "$KERNEL_IMG" ]; then
	echo "invalid kernel image specified"
	exit 1
fi

if [ -z "$KERNEL_NAME" ]; then
	echo "what is the name of this kernel?"
	exit 1
fi

KERNEL_NAME_LOWER=`echo $KERNEL_NAME | tr '[:upper:]' '[:lower:]'`
KERNEL_NAME_UPPER=`echo $KERNEL_NAME | tr '[:lower:]' '[:upper:]'`

function get_sym {
	objdump -t $KERNEL_IMG | grep " $1$" | cut -d" " -f1
}

function get_func {
	objdump -d $KERNEL_IMG | grep "<$1>:" | cut -d" " -f1
}

function get_func_end {
	objdump -d $KERNEL_IMG | grep -A10000 "<$1>:" | tail -n+2 | grep -m 1 -B10000 ^$ | grep -v ":.*90.*nop$" | tail -n 2 | head -n 1 | sed 's/ //g' | cut -d":" -f1
}

echo "/**"
echo " * @file kernel_specifics_$KERNEL_NAME_LOWER.h"
echo " * @brief #defines for the $KERNEL_NAME guest kernel (automatically generated)"
echo " * @author Ben Blum <bblum@andrew.cmu.edu>"
echo " */"
echo
echo "#ifndef __LS_KERNEL_SPECIFICS_${KERNEL_NAME_UPPER}_H"
echo "#define __LS_KERNEL_SPECIFICS_${KERNEL_NAME_UPPER}_H"
echo

########################
#### TCB management ####
########################


CURRENT_TCB=`get_sym thr_current`
echo "#define GUEST_CURRENT_TCB 0x$CURRENT_TCB"

# TODO: make like the predecessor of that which infected mushroom cooks, and generalised it
echo "#define GUEST_TCB_TID_OFFSET 0"
echo "#define TID_FROM_TCB(cpu, tcb) \\"
echo -e "\tSIM_read_phys_memory(cpu, tcb + GUEST_TCB_TID_OFFSET, WORD_SIZE)"
echo "#define GUEST_TCB_STACK_OFFSET 7"
echo "#define STACK_FROM_TCB(tcb) ((tcb)+(GUEST_TCB_STACK_OFFSET*WORD_SIZE))"
echo "#define GUEST_STACK_SIZE 4096"
echo
echo "#define GUEST_PCB_PID_OFFSET 0"
echo "#define PID_FROM_PCB(cpu, pcb) \\"
echo -e "\tSIM_read_phys_memory(cpu, pcb + GUEST_PCB_PID_OFFSET, WORD_SIZE)"

###################################
#### Thread lifecycle tracking ####
###################################

RQ=`get_sym runqueue`
echo "#define GUEST_RQ_ADDR 0x$RQ"
echo

Q_ADD=`get_func sch_queue_append`
echo "#define GUEST_Q_ADD                0x$Q_ADD"
echo "#define GUEST_Q_ADD_Q_ARGNUM       1"
echo "#define GUEST_Q_ADD_TCB_ARGNUM     2"
Q_REMOVE=`get_func sch_queue_remove`
echo "#define GUEST_Q_REMOVE             0x$Q_REMOVE"
echo "#define GUEST_Q_REMOVE_Q_ARGNUM    1"
echo "#define GUEST_Q_REMOVE_TCB_ARGNUM  2"
Q_POP=`get_func_end sch_queue_pop`
echo "#define GUEST_Q_POP_RETURN         0x$Q_POP"
echo "#define GUEST_Q_POP_Q_ARGNUM       1"

echo

TIMER_WRAP_ENTER=`get_func timer_handler_wrapper`
TIMER_WRAP_EXIT=`get_func_end timer_handler_wrapper`
echo "#define GUEST_TIMER_WRAP_ENTER     0x$TIMER_WRAP_ENTER"
echo "#define GUEST_TIMER_WRAP_EXIT      0x$TIMER_WRAP_EXIT"

CS_ENTER=`get_func context_switch`
CS_EXIT=`get_func_end context_switch`
echo "#define GUEST_CONTEXT_SWITCH_ENTER 0x$CS_ENTER"
echo "#define GUEST_CONTEXT_SWITCH_EXIT  0x$CS_EXIT"

SCHED_INIT_EXIT=`get_func_end sch_init`
echo "#define GUEST_SCHED_INIT_EXIT      0x$SCHED_INIT_EXIT"

SCHED_LOCK=`get_sym scheduler_locked`
echo "#define GUEST_SCHEDULER_LOCK       0x$SCHED_LOCK"
echo "#define GUEST_SCHEDULER_LOCKED(x)  (x)"

echo

function get_fork_window {
	# XXX this is a terrible hack
	MISBEHAVE_MODE=`get_sym misbehave_mode`
	MISBEHAVE_MODE=`echo $MISBEHAVE_MODE | sed 's/^00//'`
	objdump -d $KERNEL_IMG | grep -A10000 "<$1>:" | tail -n+2 | grep -m 1 -B10000 ^$ | grep "cmpl.*$MISBEHAVE_MODE" | sed 's/ //g' | cut -d":" -f1
}

FORK_WINDOW=`get_fork_window fork`
THRFORK_WINDOW=`get_fork_window thread_fork`
SLEEP_WINDOW=`objdump -d $KERNEL_IMG | grep -A10000 "<sleep>:" | tail -n+2 | grep -m 1 -B10000 ^$ | grep "call.*sch_runqueue_remove" | sed 's/ //g' | cut -d":" -f1`
VANISH_WINDOW=`objdump -d $KERNEL_IMG | grep -A100000 '<vanish>:' | tail -n+2 | grep -m 1 -B10000 ^$ | grep "call.*cond_wait" | tail -n 1 | sed 's/ //g' | cut -d":" -f1`

echo "#define GUEST_FORK_WINDOW_ENTER    0x$FORK_WINDOW"
echo "#define GUEST_THRFORK_WINDOW_ENTER 0x$THRFORK_WINDOW"
echo "#define GUEST_SLEEP_WINDOW_ENTER   0x$SLEEP_WINDOW"
echo "#define GUEST_VANISH_WINDOW_ENTER  0x$VANISH_WINDOW"

READLINE_WINDOW=`get_func readline`
READLINE_WINDOW_END=`get_func_end readline`

echo "#define GUEST_READLINE_WINDOW_ENTER 0x$READLINE_WINDOW"
echo "#define GUEST_READLINE_WINDOW_EXIT 0x$READLINE_WINDOW_END"

echo

######################################
#### Mutexes / Deadlock detection ####
######################################

BLOCKED_WINDOW=`objdump -d $KERNEL_IMG | grep -A10000 "<mutex_lock>:" | grep -m 1 "call.*<yield>" | sed 's/ //g' | cut -d":" -f1`
echo "#define GUEST_MUTEX_LOCK_ENTER   0x`get_func mutex_lock`"
echo "#define GUEST_MUTEX_LOCK_MUTEX_ARGNUM 1"
echo "#define GUEST_MUTEX_BLOCKED      0x$BLOCKED_WINDOW"
echo "#define GUEST_MUTEX_LOCK_EXIT    0x`get_func_end mutex_lock`"
echo "#define GUEST_MUTEX_UNLOCK_ENTER 0x`get_func mutex_unlock`"
echo "#define GUEST_MUTEX_UNLOCK_MUTEX_ARGNUM 1"
echo "#define GUEST_MUTEX_UNLOCK_EXIT  0x`get_func_end mutex_unlock`"

echo

###################################
#### Dynamic memory allocation ####
###################################

echo "#define GUEST_LMM_ALLOC_ENTER      0x`get_func lmm_alloc`"
echo "#define GUEST_LMM_ALLOC_EXIT       0x`get_func_end lmm_alloc`"
echo "#define GUEST_LMM_ALLOC_SIZE_ARGNUM 2"
echo "#define GUEST_LMM_ALLOC_GEN_ENTER  0x`get_func lmm_alloc_gen`"
echo "#define GUEST_LMM_ALLOC_GEN_EXIT   0x`get_func_end lmm_alloc_gen`"
echo "#define GUEST_LMM_ALLOC_GEN_SIZE_ARGNUM 2"
echo "#define GUEST_LMM_FREE_ENTER       0x`get_func lmm_free`"
echo "#define GUEST_LMM_FREE_EXIT        0x`get_func_end lmm_free`"
echo "#define GUEST_LMM_FREE_BASE_ARGNUM 2"
echo "#define GUEST_LMM_FREE_SIZE_ARGNUM 3"

echo

##############################
#### Kernel image regions ####
##############################

echo "#define GUEST_IMG_END 0x`get_sym _end`"
echo "#define GUEST_DATA_START 0x`get_sym .data`"
echo "#define GUEST_DATA_END 0x`get_sym _edata`" # Everything is awful forever.
echo "#define GUEST_BSS_START 0x`get_sym __bss_start`"
echo "#define GUEST_BSS_END GUEST_IMG_END"

echo

##############################
#### Scheduler boundaries ####
##############################

function add_sched_func {
	echo -e "\t{ 0x`get_func $1`, 0x`get_func_end $1` }, \\"
}

function add_sched_sym {
	echo -e "\t{ 0x`get_sym $1`, $2 }, \\"
}

echo "#define GUEST_SCHEDULER_FUNCTIONS { \\"
add_sched_func sch_queue_append
add_sched_func sch_queue_pop
add_sched_func sch_queue_remove
add_sched_func sch_queue_search
add_sched_func sch_runqueue_add_nolock
add_sched_func sch_runqueue_add
add_sched_func sch_runqueue_remove
add_sched_func sch_runqueue_next
add_sched_func context_switch
add_sched_func magic_jar
add_sched_func timer_handler
echo -e "\t}"

INT_SIZE=4
PTR_SIZE=4
MUTEX_SIZE=`echo $(($INT_SIZE+$INT_SIZE))`
RQ_SIZE=`echo $(($PTR_SIZE+$PTR_SIZE+$MUTEX_SIZE))`

echo "#define GUEST_SCHEDULER_GLOBALS { \\"
add_sched_sym scheduler_locked $INT_SIZE
add_sched_sym thr_current $PTR_SIZE
add_sched_sym timer_count $INT_SIZE
add_sched_sym sleeping $RQ_SIZE
add_sched_sym cas2i_queue $RQ_SIZE
add_sched_sym runqueue $RQ_SIZE
echo -e "\t}"

echo

#######################
#### Choice points ####
#######################

echo "#define GUEST_MUTEX_LOCK GUEST_MUTEX_LOCK_ENTER"
echo "#define GUEST_VANISH 0x`get_func vanish`"
echo "#define GUEST_VANISH_END 0x`get_func_end vanish`"

echo "#define GUEST_MUTEX_IGNORES { \\"
echo -e "\t0x`get_sym vm_mutex`, \\"
echo -e "\t0x`get_sym malloc_lock`, \\"
echo -e "\t0x`get_sym cmutex`, \\"
echo -e "\t}"

echo

echo "#endif"
