/*
 * Zero Install -- user space helper
 *
 * Copyright (C) 2003  Thomas Leonard
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <assert.h>

#include "global.h"
#include "support.h"
#include "task.h"
#include "index.h"

Task *all_tasks = NULL;
static int n = 0;

/* Create a new task of the given type. Returns NULL on OOM.
 * The task will be in the main list.
 */
Task *task_new(TaskType type)
{
	Task *task;

	task = my_malloc(sizeof(Task));
	if (!task)
		return NULL;

	task->type = type;
	task->child_task = NULL;
	task->child_pid = -1;

	task->n = ++n;

	task->step = NULL;
	task->data = NULL;
	task->str = NULL;
	task->index = NULL;
	task->size = -1;

	task->next = all_tasks;
	all_tasks = task;

	printf("Created task %d (%s)\n", task->n,
		type == TASK_KERNEL ? "kernel" :
		type == TASK_CLIENT ? "client" :
		type == TASK_INDEX ? "index" :
		type == TASK_ARCHIVE ? "archive" :
		"unknown");

	return task;
}

/* Removes 'task' from all_tasks and calls the 'next' method on every task
 * that depends on this one. Finally, task is freed.
 */
void task_destroy(Task *task, int success)
{
	Task *t;

	printf("Finished task %d\n", task->n);

	if (all_tasks == task) {
		all_tasks = task->next;
	} else {
		for (t = all_tasks; t; t = t->next) {
			if (t->next == task) {
				t->next = task->next;
				break;
			}
		}
		assert(t != NULL);
	}

	task->next = NULL;

	t = all_tasks;
	while (t) {
		if (t->child_task == task) {
			printf("Move forward with task %d\n", t->n);
			t->child_task = NULL;
			assert(t->step);
			t->step(t, success);
			t = all_tasks;
		} else
			t = t->next;
	}

	task_set_string(task, NULL);
	task_set_index(task, NULL);
	free(task);
}

/* Call 'next' on the task waiting for this pid */
void task_process_done(pid_t pid, int success)
{
	Task *t;

	for (t = all_tasks; t; t = t->next) {
		if (t->child_pid == pid) {
			printf("Move forward with task %d\n", t->n);
			t->child_pid = -1;
			t->step(t, success);
			return;
		}
	}

	printf("No task for process %ld!\n", (long) pid);
}

/* Stores a copy of 'str' in task->str (freeing any existing one).
 * If 'str' is NULL, task->str becomes NULL. Otherwise, this indicates
 * OOM.
 */
void task_set_string(Task *task, const char *str)
{
	if (task->str)
		free(task->str);
	task->str = str ? my_strdup(str) : NULL;
}

/* Sets task->index. Ref count is incremented. */
void task_set_index(Task *task, Index *index)
{
	if (index)
		index->ref++;
	task_steal_index(task, index);
}

/* Sets task->index. Ref count ownership is transferred. */
void task_steal_index(Task *task, Index *index)
{
	if (task->index)
		index_free(task->index);

	if (index)
		assert(index->ref > 0);

	task->index = index;
}
