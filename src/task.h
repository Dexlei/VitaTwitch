#ifndef VT_TASK_H
#define VT_TASK_H

/* Runs one blocking function (network request) on a worker thread so the UI
 * keeps animating and can cancel it. */

typedef int (*task_fn)(void *arg, volatile int *abort_flag);

typedef struct {
    task_fn fn;
    void *arg;
    volatile int done;
    volatile int abort_flag;
    volatile int busy;
    int result;
} task_t;

/* Returns 0 if started. The task struct must stay alive until task_done(). */
int task_start(task_t *t, task_fn fn, void *arg);
static inline int task_done(const task_t *t) { return t->done; }
static inline void task_cancel(task_t *t) { t->abort_flag = 1; }
/* Blocks until the worker has returned (after task_cancel, returns quickly). */
void task_wait(task_t *t);

#endif
