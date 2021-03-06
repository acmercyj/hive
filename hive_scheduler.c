#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "hive_cell.h"
#include "hive_env.h"
#include "hive_scheduler.h"
#include "hive_system_lib.h"

#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/time.h>

#define DEFAULT_THREAD 4
#define MAX_GLOBAL_MQ 0x10000
#define GP(p) ((p) % MAX_GLOBAL_MQ)

struct global_queue {
	uint32_t head;
	uint32_t tail;
	int total;
	struct cell * queue[MAX_GLOBAL_MQ];
	bool flag[MAX_GLOBAL_MQ];
};

struct timer {
	uint32_t current;
	struct cell * sys;
	struct global_queue * mq;
};

static void 
globalmq_push(struct global_queue *q, struct cell * c) {
	uint32_t tail = GP(__sync_fetch_and_add(&q->tail,1));
	q->queue[tail] = c;
	__sync_synchronize();
	q->flag[tail] = true;
	__sync_synchronize();
}

static struct cell * 
globalmq_pop(struct global_queue *q) {
	uint32_t head =  q->head;
	uint32_t head_ptr = GP(head);
	if (head_ptr == GP(q->tail)) {
		return NULL;
	}

	if(!q->flag[head_ptr]) {
		return NULL;
	}

	struct cell * c = q->queue[head_ptr];
	if (!__sync_bool_compare_and_swap(&q->head, head, head+1)) {
		return NULL;
	}
	q->flag[head_ptr] = false;
	__sync_synchronize();

	return c;
}

static void
globalmq_init(struct global_queue *q) {
	memset(q, 0, sizeof(*q));
}

static inline void
globalmq_inc(struct global_queue *q) {
	__sync_add_and_fetch(&q->total,1);
}

static inline void
globalmq_dec(struct global_queue *q) {
	__sync_sub_and_fetch(&q->total,1);
}

static int
_message_dispatch(struct global_queue *q) {
	struct cell *c = globalmq_pop(q);
	if (c == NULL)
		return 1;
	int r =  cell_dispatch_message(c);
	switch(r) {
	case CELL_EMPTY:
	case CELL_MESSAGE:
		break;
	case CELL_QUIT:
		return 1;
	}
	globalmq_push(q, c);
	return r;
}

static uint32_t
_gettime(void) {
	uint32_t t;
	struct timeval tv;
	gettimeofday(&tv, NULL);
	t = (uint32_t)(tv.tv_sec & 0xffffff) * 100;
	t += tv.tv_usec / 10000;
	return t;
}

static void
timer_init(struct timer *t, struct cell * sys, struct global_queue *mq) {
	t->current = _gettime();
	t->sys = sys;
	t->mq = mq;
}

static inline void
send_tick(struct cell * c) {
	cell_send(c, 0, NULL);
}

static void
_updatetime(struct timer * t) {
	uint32_t ct = _gettime();
	if (ct > t->current) {
		int diff = ct-t->current;
		t->current = ct;
		int i;
		for (i=0;i<diff;i++) {
			send_tick(t->sys);
		}
	}
}

static void *
_timer(void *p) {
	struct timer * t = p;
	for (;;) {
		_updatetime(t);
		usleep(2500);
		if (t->mq->total <= 1)
			return NULL;
	}
}

static void *
_worker(void *p) {
	struct global_queue * mq = p;
	for (;;) {
		if (_message_dispatch(mq)) {
			usleep(1000);
			if (mq->total <= 1)
				return NULL;
		} 
	}
	return NULL;
}

static void
_start(struct global_queue *gmq, int thread, struct timer *t) {
	pthread_t pid[thread+1];
	int i;

	pthread_create(&pid[0], NULL, _timer, t);

	for (i=1;i<=thread;i++) {
		pthread_create(&pid[i], NULL, _worker, gmq);
	}

	for (i=0;i<=thread;i++) {
		pthread_join(pid[i], NULL); 
	}
}

lua_State *
scheduler_newtask(lua_State *pL) {
	lua_State *L = luaL_newstate();
	luaL_openlibs(L);
	hive_createenv(L);

	struct global_queue * mq = hive_copyenv(L, pL, "message_queue");
	globalmq_inc(mq);
	hive_copyenv(L, pL, "system_pointer");

	lua_newtable(L);
	lua_newtable(L);
	lua_pushliteral(L, "v");
	lua_setfield(L, -2, "__mode");
	lua_setmetatable(L,-2);
	hive_setenv(L, "cell_map");

	return L;
}

void
scheduler_deletetask(lua_State *L) {
	hive_getenv(L, "message_queue");
	struct global_queue *mq = lua_touserdata(L, -1);
	globalmq_dec(mq);
	lua_close(L);
}

void
scheduler_starttask(lua_State *L) {
	hive_getenv(L, "message_queue");
	struct global_queue * gmq = lua_touserdata(L, -1);
	lua_pop(L,1);
	hive_getenv(L, "cell_pointer");
	struct cell * c= lua_touserdata(L, -1);
	lua_pop(L,1);
	globalmq_push(gmq, c);
}

int 
scheduler_start(lua_State *L) {
	luaL_checktype(L,1,LUA_TTABLE);
	hive_createenv(L);
	struct global_queue * gmq = lua_newuserdata(L, sizeof(*gmq));
	globalmq_init(gmq);

	lua_pushvalue(L,-1);
	hive_setenv(L, "message_queue");

	lua_State *sL;

	sL = scheduler_newtask(L);
	luaL_requiref(sL, "cell.system", cell_system_lib, 0);
	lua_pop(sL,1);
	struct cell * sys = cell_new(sL, "system.lua");
	if (sys == NULL) {
		scheduler_deletetask(sL);
		return 0;
	}
	lua_pushlightuserdata(L, sys);
	hive_setenv(L, "system_pointer");
	scheduler_starttask(sL);
	
	lua_getfield(L,1,"main");
	const char * mainfile = luaL_checkstring(L, -1);
	sL = scheduler_newtask(L);
	void * c = cell_new(sL, mainfile);
	if (c == NULL) {
		cell_close(sys);
		return 0;
	}
	scheduler_starttask(sL);
	lua_pop(L,1);	// pop string mainfile
	
	lua_getfield(L,1, "thread");
	int thread = luaL_optinteger(L, -1, DEFAULT_THREAD);
	lua_pop(L,1);

	struct timer * t = lua_newuserdata(L, sizeof(*t));
	timer_init(t,sys,gmq);

	_start(gmq,thread,t);
	cell_close(sys);

	return 0;
}

