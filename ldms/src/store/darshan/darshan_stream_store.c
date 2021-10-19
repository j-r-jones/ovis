/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2021 National Technology & Engineering Solutions
 * of Sandia, LLC (NTESS). Under the terms of Contract DE-NA0003525 with
 * NTESS, the U.S. Government retains certain rights in this software.
 * Copyright (c) 2018 Open Grid Computing, Inc. All rights reserved.
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
 *	Redistributions of source code must retain the above copyright
 *	notice, this list of conditions and the following disclaimer.
 *
 *	Redistributions in binary form must reproduce the above
 *	copyright notice, this list of conditions and the following
 *	disclaimer in the documentation and/or other materials provided
 *	with the distribution.
 *
 *	Neither the name of Sandia nor the names of any contributors may
 *	be used to endorse or promote products derived from this software
 *	without specific prior written permission.
 *
 *	Neither the name of Open Grid Computing nor the names of any
 *	contributors may be used to endorse or promote products derived
 *	from this software without specific prior written permission.
 *
 *	Modified source versions must be plainly marked as such, and
 *	must not be misrepresented as being the original software.
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
#include <inttypes.h>
#include <unistd.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>
#include <coll/htbl.h>
#include <sos/sos.h>
#include <openssl/sha.h>
#include <math.h>
#include <ovis_json/ovis_json.h>
#include "ldms.h"
#include "ldmsd.h"
#include "ldmsd_stream.h"

static ldmsd_msg_log_f msglog;

static sos_schema_t app_schema;
static char path_buff[PATH_MAX];
static char *log_path = "/var/log/ldms/darshan_stream_store.log";
static char *verbosity = "WARN";
static char *stream;

static char *root_path;

static struct ldmsd_plugin darshan_stream_store;

static union sos_timestamp_u
to_timestamp(double d)
{
	union sos_timestamp_u u;
	double secs = floor(d);
	u.fine.secs = secs;
	u.fine.usecs = d - secs;
	return u;
}

static const char *time_job_rank_attrs[] = { "timestamp", "job_id", "rank" };
static const char *job_time_rank_attrs[] = { "job_id", "timestamp", "rank" };
static const char *job_rank_time_attrs[] = { "job_id", "rank", "timestamp" };


/*
 * Example JSON object:
 *
 * { \"job_id\" : 0,
 *   \"rank\" : 0,
 *   \"ProducerName\" : \"nid00021\",
 *   \"file\" : \"/projects/darshan/test/mpi-io-test.tmp.dat\",
 *   \"record_id\" : 6222542600266098259,
 *   \"module\" : \"X_POSIX\",
 *   \"type\" : \"MOD\",
 *   \"max_byte\" : 1,
 *   \"switches\" : 0,
 *   \"cnt\" : 1,
 *   \"op\" : \"writes_segment_0\",
 *   \"seg\" :
 *	[
 *	  { \"off\" : 0,
 *	    \"len\" : 16777216,
 *	    \"dur\" : 966.3333,
 *	    \"timestamp\" : 163000348.3312,
 *	  }
 *     ]
 *  }
 */

//{ "job_id":6582,"rank":0,"ProducerName":"nid00021","file":"N/A","record_id":6222542600266098259,"module":"POSIX","type":"MOD","max_byte":16777215,"switches":0,"cnt":1,"op":"writes_segment_0","seg":[{"off":0,"len":16777216,"dur":0.16,"timestamp":1631904596.737955}]}

static struct sos_schema_template darshan_data_template = {
	.name = "darshan_data",
	.uuid = "3a650cf7-0b83-44dc-accc-e44acaa81740",
	.attrs = {
		{ .name = "job_id", .type = SOS_TYPE_UINT64 },
		{ .name = "rank", .type = SOS_TYPE_UINT64 },
		{ .name = "ProducerName", .type = SOS_TYPE_STRING },
		{ .name = "file", .type = SOS_TYPE_STRING },
		{ .name = "record_id", .type = SOS_TYPE_UINT64 },
		{ .name = "module", .type = SOS_TYPE_STRING },
		{ .name = "type", .type = SOS_TYPE_STRING },
		{ .name = "max_byte", .type = SOS_TYPE_UINT64 },
		{ .name = "switches", .type = SOS_TYPE_UINT64 },
		{ .name = "cnt", .type = SOS_TYPE_UINT64 },
		{ .name = "op", .type = SOS_TYPE_STRING },
		{ .name = "off", .type = SOS_TYPE_UINT64 },
		{ .name = "len", .type = SOS_TYPE_UINT64 },
		{ .name = "dur", .type = SOS_TYPE_DOUBLE },
		{ .name = "timestamp", .type = SOS_TYPE_DOUBLE },
		{ .name = "time_job_rank", .type = SOS_TYPE_JOIN,
		  .size = 3,
		  .indexed = 1,
		  .join_list = time_job_rank_attrs
		},
		{ .name = "job_rank_time", .type = SOS_TYPE_JOIN,
		  .size = 3,
		  .indexed = 1,
		  .join_list = job_rank_time_attrs
		},
		{ .name = "job_time_rank", .type = SOS_TYPE_JOIN,
		  .size = 3,
		  .indexed = 1,
		  .join_list = job_time_rank_attrs
		},

		{ 0 }
	}
};

enum attr_ids {
       JOB_ID,
       RANK_ID,
       PRODUCERNAME_ID,
       FILE_ID,
       RECORD_ID,
       MODULE_ID,
       TYPE_ID,
       MAX_BYTE_ID,
       SWITCHES_ID,
       COUNT_ID,
       OPERATION_ID,
       OFFSET_ID,
       LENGTH_ID,
       DURATION_ID,
       TIMESTAMP_ID,
};

static int create_schema(sos_t sos, sos_schema_t *app)
{
	int rc;
	sos_schema_t schema;

	/* Create and add the App schema */
	schema = sos_schema_from_template(&darshan_data_template);
	if (!schema) {
		msglog(LDMSD_LERROR, "%s: Error %d creating Darshan data schema.\n",
		       darshan_stream_store.name, errno);
		rc = errno;
		goto err;
	}
	rc = sos_schema_add(sos, schema);
	if (rc) {
		msglog(LDMSD_LERROR, "%s: Error %d adding Darshan data schema.\n",
		       darshan_stream_store.name, rc);
		goto err;
	}
	*app = schema;
	return 0;

err:
	if (schema) {
		free(schema);
	}
	return rc;

}

static int container_mode = 0660;	/* Default container permission bits */
static sos_t sos;
static int reopen_container(char *path)
{
	int rc = 0;
	sos_schema_t schema;

	/* Close the container if it already exists */
	if (sos)
		sos_container_close(sos, SOS_COMMIT_ASYNC);


	/* Creates the container if it doesn't already exist  */
	sos = sos_container_open(path, SOS_PERM_RW|SOS_PERM_CREAT, container_mode);
	if (!sos) {
		return errno;
	}

	app_schema = sos_schema_by_name(sos, darshan_data_template.name);
	if (!app_schema) {
		rc = create_schema(sos, &app_schema);
		if (rc)
			return rc;
	}
	return rc;
}

static const char *usage(struct ldmsd_plugin *self)
{
	return	"config name=darshan_stream_store path=<path> port=<port_no> log=<path>\n"
		"     path	The path to the root of the SOS container store (required).\n"
		"     stream	The stream name to subscribe to (defaults to 'darshan Connector').\n"
		"     mode	The container permission mode for create, (defaults to 0660).\n";
}

static int stream_recv_cb(ldmsd_stream_client_t c, void *ctxt,
			 ldmsd_stream_type_t stream_type,
			 const char *msg, size_t msg_len,
			 json_entity_t entity);
static int config(struct ldmsd_plugin *self, struct attr_value_list *kwl, struct attr_value_list *avl)
{
	char *value;
	char *producer_name;
	int rc;
	value = av_value(avl, "mode");
	if (value)
		container_mode = strtol(value, NULL, 0);
	if (!container_mode) {
		msglog(LDMSD_LERROR,
		       "%s: ignoring bogus container permission mode of %s, using 0660.\n",
		       darshan_stream_store.name, value);
	}

	value = av_value(avl, "stream");
	if (value)
		stream = strdup(value);
	else
		stream = strdup("darshanConnector");
	ldmsd_stream_subscribe(stream, stream_recv_cb, self);

	value = av_value(avl, "path");
	if (!value) {
		msglog(LDMSD_LERROR,
		       "%s: the path to the container (path=) must be specified.\n",
		       darshan_stream_store.name);
		return ENOENT;
	}

	if (root_path)
		free(root_path);
	root_path = strdup(value);
	if (!root_path) {
		msglog(LDMSD_LERROR,
		       "%s: Error allocating %d bytes for the container path.\n",
		       strlen(value) + 1);
		return ENOMEM;
	}

	rc = reopen_container(root_path);
	if (rc) {
		msglog(LDMSD_LERROR, "%s: Error opening %s.\n",
		       darshan_stream_store.name, root_path);
		return ENOENT;
	}
	return 0;
}

static int get_json_value(json_entity_t e, char *name, int expected_type, json_entity_t *out)
{
	int v_type;
	json_entity_t a = json_attr_find(e, name);
	json_entity_t v;
	if (!a) {
		msglog(LDMSD_LERROR,
		       "%s: The JSON entity is missing the '%s' attribute.\n",
		       darshan_stream_store.name,
		       name);
		return EINVAL;
	}
	v = json_attr_value(a);
	v_type = json_entity_type(v);
	if (v_type != expected_type) {
		msglog(LDMSD_LERROR,
		       "%s: The '%s' JSON entity is the wrong type. "
		       "Expected %d, received %d\n",
		       darshan_stream_store.name,
		       name, expected_type, v_type);
		return EINVAL;
	}
	*out = v;
	return 0;
}


/* Metadata */
/* { "job_id":6594,"rank":2,"ProducerName":"nid00021","file":"/projects/darshan/test/mpi-io-test.tmp.dat","record_id":6222542600266098259,"module":"POSIX","type":"MET","max_byte":-1,"switches":-1,"cnt":1,"op":"opens_segment_0","seg":[{"off":-1,"len":-1,"dur":0.00,"timestamp":1631904596.556221}]} */
/* Module data */
/* { "job_id":6582,"rank":0,"ProducerName":"nid00021","file":"N/A","record_id":6222542600266098259,"module":"POSIX","type":"MOD","max_byte":16777215,"switches":0,"cnt":1,"op":"writes_segment_0","seg":[{"off":0,"len":16777216,"dur":0.16,"timestamp":1631904596.737955}]} */

static int stream_recv_cb(ldmsd_stream_client_t c, void *ctxt,
			  ldmsd_stream_type_t stream_type,
			  const char *msg, size_t msg_len,
			  json_entity_t entity)
{
	int rc;
	json_entity_t v, list, item;
	double timestamp;
	uint64_t record_id, count, rank, offset, length, job_id, duration, max_byte, switches;
	char *module_name, *file_name, *type, *hostname, *operation, *producer_name;

	if (!entity) {
		msglog(LDMSD_LERROR,
		       "%s: NULL entity received in stream callback.\n",
		       darshan_stream_store.name);
		return 0;
	}

	rc = get_json_value(entity, "job_id", JSON_INT_VALUE, &v);
	if (rc)
		goto err;
	job_id = json_value_int(v);

	rc = get_json_value(entity, "rank", JSON_INT_VALUE, &v);
	if (rc)
		goto err;
	rank = json_value_int(v);

	rc = get_json_value(entity, "ProducerName", JSON_STRING_VALUE, &v);
	if (rc)
		goto err;
	producer_name = json_value_str(v)->str;

	rc = get_json_value(entity, "file", JSON_STRING_VALUE, &v);
	if (rc)
		goto err;
	file_name = json_value_str(v)->str;

	rc = get_json_value(entity, "record_id", JSON_INT_VALUE, &v);
	if (rc)
		goto err;
	record_id = json_value_int(v);

	rc = get_json_value(entity, "module", JSON_STRING_VALUE, &v);
	if (rc)
		goto err;
	module_name = json_value_str(v)->str;

	rc = get_json_value(entity, "type", JSON_STRING_VALUE, &v);
	if (rc)
		goto err;
	type = json_value_str(v)->str;

	rc = get_json_value(entity, "max_byte", JSON_INT_VALUE, &v);
	if (rc)
		goto err;
	max_byte = json_value_int(v);

	rc = get_json_value(entity, "switches", JSON_INT_VALUE, &v);
	if (rc)
		goto err;
	switches = json_value_int(v);

	rc = get_json_value(entity, "cnt", JSON_INT_VALUE, &v);
	if (rc)
		goto err;
	count = json_value_int(v);

	rc = get_json_value(entity, "op", JSON_STRING_VALUE, &v);
	if (rc)
		goto err;
	operation = json_value_str(v)->str;

	rc = get_json_value(entity, "seg", JSON_LIST_VALUE, &list);
	if (rc)
		goto err;
	for (item = json_item_first(list); item; item = json_item_next(item)) {

		if (json_entity_type(item) != JSON_DICT_VALUE) {
			msglog(LDMSD_LERROR,
			       "%s: Items in segment must all be dictionaries.\n",
			       darshan_stream_store.name);
			rc = EINVAL;
			goto err;
		}

		rc = get_json_value(item, "off", JSON_INT_VALUE, &v);
		if (rc)
			goto err;
		offset = json_value_int(v);

		rc = get_json_value(item, "len", JSON_INT_VALUE, &v);
		if (rc)
			goto err;
		length= json_value_int(v);

		rc = get_json_value(item, "dur", JSON_FLOAT_VALUE, &v);
		if (rc)
			goto err;
		duration = json_value_float(v);

		rc = get_json_value(item, "timestamp", JSON_FLOAT_VALUE, &v);
		if (rc)
			goto err;
		timestamp = json_value_float(v);

		sos_obj_t obj = sos_obj_new(app_schema);
		if (!obj) {
			rc = errno;
			msglog(LDMSD_LERROR,
			       "%s: Error %d creating Darshan data object.\n",
			       darshan_stream_store.name, errno);
			goto err;
		}

		msglog(LDMSD_LDEBUG, "%s: Got a record from stream (%s), module_name = %s\n",
				darshan_stream_store.name, stream, module_name);

		sos_obj_attr_by_id_set(obj, JOB_ID, job_id);
		sos_obj_attr_by_id_set(obj, RANK_ID, rank);
		sos_obj_attr_by_id_set(obj, PRODUCERNAME_ID, strlen(producer_name)+1, producer_name);
		sos_obj_attr_by_id_set(obj, FILE_ID, strlen(file_name)+1,  file_name);
		sos_obj_attr_by_id_set(obj, RECORD_ID, record_id);
		sos_obj_attr_by_id_set(obj, MODULE_ID, strlen(module_name)+1, module_name);
		sos_obj_attr_by_id_set(obj, TYPE_ID, strlen(type)+1, type);
		sos_obj_attr_by_id_set(obj, MAX_BYTE_ID, max_byte);
		sos_obj_attr_by_id_set(obj, SWITCHES_ID, switches);
		sos_obj_attr_by_id_set(obj, COUNT_ID, count);
		sos_obj_attr_by_id_set(obj, RANK_ID, rank);
		sos_obj_attr_by_id_set(obj, OPERATION_ID, strlen(operation)+1, operation);
		sos_obj_attr_by_id_set(obj, OFFSET_ID, offset);
		sos_obj_attr_by_id_set(obj, LENGTH_ID, length);
		sos_obj_attr_by_id_set(obj, DURATION_ID, duration);
		sos_obj_attr_by_id_set(obj, TIMESTAMP_ID, timestamp);

		sos_obj_index(obj);
		sos_obj_put(obj);
	}
	rc = 0;
 err:
	return rc;
}

static void term(struct ldmsd_plugin *self)
{
	if (sos)
		sos_container_close(sos, SOS_COMMIT_ASYNC);
	if (root_path)
		free(root_path);
}

static struct ldmsd_plugin darshan_stream_store = {
	.name = "darshan_stream_store",
	.term = term,
	.config = config,
	.usage = usage,
};

struct ldmsd_plugin *get_plugin(ldmsd_msg_log_f pf)
{
	msglog = pf;
	return &darshan_stream_store;
}