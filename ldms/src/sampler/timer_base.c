/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2016-2017 Open Grid Computing, Inc. All rights reserved.
 * Copyright (c) 2016-2017 Sandia Corporation. All rights reserved.
 * Under the terms of Contract DE-AC04-94AL85000, there is a non-exclusive
 * license for use of this work by or on behalf of the U.S. Government.
 * Export of this program may require a license from the United States
 * Government.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the BSD-type
 * license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *      Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *      Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *
 *      Neither the name of Sandia nor the names of any contributors may
 *      be used to endorse or promote products derived from this software
 *      without specific prior written permission.
 *
 *      Neither the name of Open Grid Computing nor the names of any
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 *      Modified source versions must be plainly marked as such, and
 *      must not be misrepresented as being the original software.
 *
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file timer_base.c
 * \brief a base class of ldmsd sampler with additional timer.
 *
 * This is a base class of ldmsd sampler that uses tsampler utility (see \ref
 * tsampler.h and \ref tsampler.c) as additional timer to sample high-frequency
 * data.
 *
 * This sampler plugin will create metric arrays of timestamps, each array
 * associated with the 'interval' parameter supplied in the configuration (see
 * usage() and config()). The arrays then populated with 64-bit timestamp values
 * [(high_32_bit) sec | (low_32_bit) usec].
 *
 * The extra-timer is setup in create_metric_set(), and are started in sample().
 */

#include <assert.h>
#include <stdint.h>
#include <pthread.h>
#include <errno.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <stdlib.h>
#include "ldms.h"
#include "ldmsd.h"
#include "tsampler.h"
#include "timer_base.h"
#include "ldms_jobid.h"

static ldmsd_msg_log_f msglog;

LJI_GLOBALS;

/* state of the sampler */
typedef enum {
	TBS_INIT,
	TBS_CONFIGURED,
	TBS_RUNNING,
} timer_base_state_e;

/* Convenient functions to convert timeval <--> u64 */
static inline
struct timeval u64_to_tv(uint64_t x)
{
	struct timeval tv;
	tv.tv_sec = x >> 32;
	tv.tv_usec = x & 0xFFFFFFFF;
	return tv;
}

static inline
uint64_t tv_to_u64(struct timeval tv)
{
	uint64_t x;
	x = (((uint64_t)tv.tv_sec) << 32) | (tv.tv_usec & 0xFFFFFFFF);
	return x;
}

ldms_set_t timer_base_get_set(struct ldmsd_sampler *self)
{
	struct timer_base *tb = (void*)self;
	return tb->set;
}

int timer_base_create_metric_set(struct timer_base *tb)
{
	int rc = 0;
	pthread_mutex_lock(&tb->mutex);
	tb->set = ldms_set_new(tb->iname, tb->schema);
	if (!tb->set) {
		rc = errno;
		goto out;
	}
	ldms_metric_set_u64(tb->set, 0, tb->compid);
	ldms_metric_set_u64(tb->set, 1, 0); /* default jobid */
	tb->state = TBS_CONFIGURED;
out:
	pthread_mutex_unlock(&tb->mutex);
	return rc;
}

int timer_base_add_hfmetric(struct timer_base *tb,
				const char *name,
				enum ldms_value_type type,
				int n,
				const struct timeval *interval,
				tsampler_sample_cb cb,
				void *ctxt)
{
	int rc = 0;
	enum ldms_value_type atype;
	struct tsampler_timer_entry *t = calloc(1, sizeof(*t));
	if (!t) {
		rc = errno;
		goto out;
	}
	if (type < LDMS_V_CHAR_ARRAY || LDMS_V_D64_ARRAY < type) {
		rc = EINVAL;
		goto out;
	}
	t->timer.n = n;
	t->timer.mid = ldms_schema_metric_array_add(tb->schema, name, type, t->timer.n);
	if (t->timer.mid < 0) {
		rc = -t->timer.mid;
		goto out;
	}
	snprintf(tb->buff, sizeof(tb->buff), "%s_timeval", name);
	t->timer.tid = ldms_schema_metric_array_add(tb->schema, tb->buff,
				LDMS_V_U64_ARRAY, t->timer.n*2);
	if (t->timer.tid < 0) {
		rc = -t->timer.tid;
		goto out;
	}
	t->timer.sampler = &tb->base;
	t->timer.cb = cb;
	t->timer.ctxt = ctxt;
	t->timer.interval = *interval;
	TAILQ_INSERT_TAIL(&tb->timer_list, t, entry);
	/* timer will be activated later in sample() function */
out:
	return rc;
}

int timer_base_config(struct ldmsd_plugin *self, struct attr_value_list *kwl,
		struct attr_value_list *avl)
{
	struct timer_base *tb;
	char *v;
	char *pname;
	char *sname;
	char *iname;
	char *timer;
	int rc = 0;

	tb = (void*)self;

	pthread_mutex_lock(&tb->mutex);
	switch (tb->state) {
	case TBS_INIT:
		/* OK */
		break;
	case TBS_CONFIGURED:
	case TBS_RUNNING:
		rc = EEXIST;
		goto out;
	}

	if (tb->schema) {
		msglog(LDMSD_LERROR, "%s: schema existed.\n", tb->base.base.name);
		rc = EEXIST;
		goto out;
	}

	if (tb->set) {
		msglog(LDMSD_LERROR, "%s: set existed.\n", tb->base.base.name);
		rc = EEXIST;
		goto out;
	}

	pname = av_value(avl, "producer");
	if (!pname) {
		msglog(LDMSD_LERROR, "%s: missing producer.\n", tb->base.base.name);
		rc = EINVAL;
		goto out;
	}
	snprintf(tb->pname, sizeof(tb->pname), "%s", pname);
	iname = av_value(avl, "instance");
	if (!iname) {
		msglog(LDMSD_LERROR, "%s: missing instance.\n", tb->base.base.name);
		rc = EINVAL;
		goto out;
	}
	snprintf(tb->iname, sizeof(tb->iname), "%s", iname);

	v = av_value(avl, "component_id");
	if (!v)
		tb->compid = 0;
	else
		tb->compid = strtoull(v, NULL, 0);

	LJI_CONFIG(v,avl);

	sname = av_value(avl, "schema");
	if (!sname)
		sname = tb->base.base.name;

	tb->schema = ldms_schema_new(sname);
	if (!tb->schema) {
		rc = errno;
		goto out;
	}

	rc = ldms_schema_metric_add(tb->schema, "component_id", LDMS_V_U64);
	if (rc < 0) {
		msglog(LDMSD_LERROR, "%s: ldms_schema_metric_add() failed, rc: %d.\n",
				tb->base.base.name, rc);
		goto cleanup;
	}
	rc = LJI_ADD_JOBID(tb->schema);
	if (rc < 0) {
		msglog(LDMSD_LERROR, "%s: ldms_schema_metric_add() failed, rc: %d.\n",
				tb->base.base.name, rc);
		goto cleanup;
	}

	rc = 0;
	tb->state = TBS_CONFIGURED;

	goto out;

cleanup:
	ldms_schema_delete(tb->schema);
	tb->schema = NULL;

out:
	pthread_mutex_unlock(&tb->mutex);
	return rc;
}

int timer_base_create_set(struct timer_base *tb)
{
	tb->set = ldms_set_new(tb->iname, tb->schema);
	if (!tb->set) {
		msglog(LDMSD_LERROR, "%s: ldms_set_new() failed, errno: %d.\n",
				tb->base.base.name, errno);
		return errno;
	}
	ldms_set_producer_name_set(tb->set, tb->pname);
	if (tb->compid) {
		ldms_metric_set_u64(tb->set, 0, tb->compid);
	}
	return 0;
}

void timer_base_cleanup(struct timer_base *tb);

void timer_base_term(struct ldmsd_plugin *self)
{
	/* remove all timers when we terminate */
	timer_base_cleanup((void*)self);
}

int timer_base_sample(struct ldmsd_sampler *self)
{
	struct timer_base *tb = (void*)self;
	struct timeval tv;
	uint64_t ts;
	ldms_mval_t mval;
	ldms_value_t v;
	int i, rc = 0;
	static uint64_t count;
	struct tsampler_timer_entry *ent;

	ldms_transaction_begin(tb->set);

	LJI_SAMPLE(tb->set,1);

	pthread_mutex_lock(&tb->mutex);
	switch (tb->state) {
	case TBS_INIT:
		assert(0);
		break;
	case TBS_CONFIGURED:
		/* add timers if this is the first sample() */
		count = 0;
		TAILQ_FOREACH(ent, &tb->timer_list, entry) {
			ent->timer.set = tb->set;
			rc = tsampler_timer_add(&ent->timer);
			if (rc)
				goto timer_cleanup;
		}
		tb->state = TBS_RUNNING;
		break;
	case TBS_RUNNING:
		/* do nothing */
		break;
	}
	pthread_mutex_unlock(&tb->mutex);

	goto out;

timer_cleanup:
	/* remove added timers at cleanup */
	while ((ent = TAILQ_FIRST(&tb->timer_list))) {
		TAILQ_REMOVE(&tb->timer_list, ent, entry);
		tsampler_timer_remove(&ent->timer);
	}
	pthread_mutex_unlock(&tb->mutex);

out:
	ldms_transaction_end(tb->set);
	return rc;
}

const char *timer_base_usage(struct ldmsd_plugin *self)
{
	return "timer_base is a base-class sampler that cannot be used by itself.";
}

void timer_base_remove_timers(struct timer_base *tb)
{
	struct tsampler_timer_entry *ent;
	while ((ent = TAILQ_FIRST(&tb->timer_list))) {
		TAILQ_REMOVE(&tb->timer_list, ent, entry);
		tsampler_timer_remove(&ent->timer);
		free(ent);
	}
}

void timer_base_cleanup(struct timer_base *tb)
{
	timer_base_remove_timers(tb);
	if (tb->set) {
		ldms_set_delete(tb->set);
		tb->set = NULL;
	}
	if (tb->schema) {
		ldms_schema_delete(tb->schema);
		tb->schema = NULL;
	}
	tb->state = TBS_INIT;
}

void timer_base_init(struct timer_base *tb)
{
	/* sub-class can override these values after calling this function */
	snprintf(tb->base.base.name, sizeof(tb->base.base.name), "timer_base");
	tb->base.base.type = LDMSD_PLUGIN_SAMPLER;
	tb->base.base.term = timer_base_term;
	tb->base.base.config = timer_base_config;
	tb->base.base.usage = timer_base_usage;
	tb->base.get_set = timer_base_get_set;
	tb->base.sample = timer_base_sample;
	tb->state = TBS_INIT;
	TAILQ_INIT(&tb->timer_list);
	pthread_mutex_init(&tb->mutex, NULL);
}

struct ldmsd_plugin *get_plugin(ldmsd_msg_log_f pf)
{
	msglog = pf;
	struct timer_base *tb = calloc(1, sizeof(*tb));
	if (!tb)
		return NULL;
	timer_base_init(tb);
	return (void*)tb;
}
