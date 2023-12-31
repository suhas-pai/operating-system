/*
 * kernel/src/sched/thread.h
 * © suhas pai
 */

#pragma once

#include "cpu/info.h"

#include "info.h"
#include "process.h"

struct thread {
    struct process *process;
    struct cpu_info *cpu;

    bool premption_disabled : 1;

    struct array events_hearing;
    struct sched_thread_info sched_info;
};

extern struct thread kernel_main_thread;
struct thread *current_thread();

void prempt_disable();
void prempt_enable();
