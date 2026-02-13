/*
 * Samba rate limiting coordination daemon
 *
 * Copyright (c) 2026 Avan Thakkar <athakkar@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "includes.h"
#include "smbd_ratelimitd.h"
#include "source3/include/ratelimit_protocol.h"
#include "lib/util/time.h"
#include "lib/util/tevent_ntstatus.h"
#include "lib/util/tevent_unix.h"
#include "messages.h"
#include "librpc/gen_ndr/messaging.h"
#include "system/filesys.h"
#include <sys/socket.h>
#include <sys/un.h>

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_VFS

#define MODULE_NAME "ratelimitd"

/* Activity timeout - 5 seconds */
#define RATELIMITD_ACTIVITY_TIMEOUT_US (5000000L)

/* Broadcast interval - 1 second */
#define RATELIMITD_BROADCAST_INTERVAL_US (1000000L)

/* Remove processes inactive for more than 1 hour */
#define INACTIVE_CLEANUP_THRESHOLD_US (3600000000L)

/* Per-process activity tracking */
struct process_activity {
	struct process_activity *prev, *next;
	pid_t pid;
	int32_t snum;
	char op[8];
	int64_t recent_iops;
	uint32_t inflight_ios;
	uint64_t last_seen_usec;
	bool is_active;
};

struct smbd_ratelimitd_state {
	struct tevent_context *ev;
	struct messaging_context *msg_ctx;
	int unix_sock;
	char *socket_path;
	uint32_t my_vnn;

	struct process_activity *processes;

	struct tevent_timer *broadcast_timer;
	uint64_t last_broadcast_usec;

	uint64_t total_reports_received;
	uint64_t total_broadcasts_sent;
};

static uint64_t time_now_usec(void)
{
	struct timespec ts;

	clock_gettime_mono(&ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int smbd_ratelimitd_state_destructor(
	struct smbd_ratelimitd_state *state)
{
	int ret;

	if (state->unix_sock != -1) {
		close(state->unix_sock);
		state->unix_sock = -1;
	}

	if (state->socket_path != NULL) {
		ret = unlink(state->socket_path);
		if (ret == 0) {
			DBG_DEBUG("[%s] Removed socket: %s\n",
				  MODULE_NAME,
				  state->socket_path);
		} else if (errno != ENOENT) {
			DBG_WARNING("[%s] Failed to remove socket %s: %s\n",
				    MODULE_NAME,
				    state->socket_path,
				    strerror(errno));
		}
	}

	return 0;
}

static struct process_activity *find_process_activity(
	struct smbd_ratelimitd_state *state,
	pid_t pid,
	int32_t snum,
	const char *op)
{
	struct process_activity *proc;

	for (proc = state->processes; proc != NULL; proc = proc->next) {
		if (proc->pid == pid && proc->snum == snum &&
		    strcmp(proc->op, op) == 0)
		{
			DBG_DEBUG("[%s] find_process_activity: FOUND\n",
				  MODULE_NAME);
			return proc;
		}
	}

	DBG_DEBUG("[%s] find_process_activity: NOT FOUND\n", MODULE_NAME);
	return NULL;
}

static void update_process_activity(
	struct smbd_ratelimitd_state *state,
	const struct ratelimit_activity_report *report)
{
	struct process_activity *proc;
	const char *op_str;
	bool is_new = false;

	op_str = ratelimit_op_to_string(report->operation);

	proc = find_process_activity(state,
				     (pid_t)report->pid,
				     report->snum,
				     op_str);

	if (proc == NULL) {
		DBG_DEBUG("[%s] update_process_activity: process not found, "
			  "creating new\n",
			  MODULE_NAME);

		proc = talloc_zero(state, struct process_activity);
		if (proc == NULL) {
			DBG_ERR("[%s] update_process_activity: failed to "
				"allocate process entry\n",
				MODULE_NAME);
			return;
		}

		proc->pid = report->pid;
		proc->snum = report->snum;
		strlcpy(proc->op, op_str, sizeof(proc->op));
		is_new = true;

		DLIST_ADD(state->processes, proc);

		DBG_DEBUG("[%s] NEW process tracked: pid=%d snum=%d op=%s\n",
			  MODULE_NAME,
			  proc->pid,
			  proc->snum,
			  proc->op);
	} else {
		DBG_DEBUG("[%s] update_process_activity: updating existing "
			  "process\n",
			  MODULE_NAME);
	}

	proc->recent_iops = report->recent_iops;
	proc->inflight_ios = report->inflight_ios;
	proc->last_seen_usec = report->timestamp_usec;
	proc->is_active = (report->recent_iops > 0 ||
			   report->inflight_ios > 0);

	DBG_DEBUG("[%s] Updated: pid=%d snum=%d op=%s iops=%" PRId64 " "
		  "inflight=%u active=%d %s\n",
		  MODULE_NAME,
		  proc->pid,
		  proc->snum,
		  proc->op,
		  proc->recent_iops,
		  proc->inflight_ios,
		  proc->is_active,
		  is_new ? "[NEW]" : "[EXISTING]");
}

static void handle_unix_socket_read(struct tevent_context *ev,
				    struct tevent_fd *fde,
				    uint16_t flags,
				    void *private_data)
{
	struct smbd_ratelimitd_state *state = private_data;
	struct ratelimit_activity_report report;
	struct sockaddr_un from;
	socklen_t fromlen = sizeof(from);
	ssize_t ret;

	ret = recvfrom(state->unix_sock,
		       &report,
		       sizeof(report),
		       0,
		       (struct sockaddr *)&from,
		       &fromlen);

	if (ret < 0) {
		DBG_ERR("[%s] recvfrom() failed: %s\n",
			MODULE_NAME,
			strerror(errno));
		return;
	}

	if (ret != sizeof(report)) {
		DBG_ERR("[%s] Short read: got %zd, expected %zu\n",
			MODULE_NAME,
			ret,
			sizeof(report));
		return;
	}

	DBG_DEBUG("[%s] handle_unix_socket_read: received report "
		  "pid=%d snum=%d op=%s recent_iops=%" PRId64 "\n",
		  MODULE_NAME,
		  report.pid,
		  report.snum,
		  ratelimit_op_to_string(report.operation),
		  report.recent_iops);

	if (report.protocol_version != RATELIMIT_PROTOCOL_VERSION) {
		DBG_ERR("[%s] Protocol mismatch: got %u, expected %u\n",
			MODULE_NAME,
			report.protocol_version,
			RATELIMIT_PROTOCOL_VERSION);
		return;
	}

	update_process_activity(state, &report);
	state->total_reports_received++;

	DBG_DEBUG("[%s] handle_unix_socket_read: DONE total_reports=%" PRIu64
		  "\n",
		  MODULE_NAME,
		  state->total_reports_received);
}

static void count_active_by_operation(struct smbd_ratelimitd_state *state,
				      int32_t *read_count,
				      int32_t *write_count)
{
	uint64_t now = time_now_usec();
	struct process_activity *proc;
	int timed_out = 0;

	*read_count = 0;
	*write_count = 0;

	for (proc = state->processes; proc != NULL; proc = proc->next) {
		uint64_t age_us = now - proc->last_seen_usec;

		if (age_us > RATELIMITD_ACTIVITY_TIMEOUT_US) {
			if (proc->is_active) {
				DBG_NOTICE("[%s] Process TIMED OUT "
					   "(pid=%d snum=%d op=%s "
					   "age=%" PRIu64 " ms)\n",
					   MODULE_NAME,
					   proc->pid,
					   proc->snum,
					   proc->op,
					   age_us / 1000);
				proc->is_active = false;
				timed_out++;
			}
			DBG_DEBUG("[%s] count_active_by_operation: process "
				  "TIMED OUT, skipping\n",
				  MODULE_NAME);
			continue;
		}

		if (!proc->is_active) {
			DBG_DEBUG("[%s] count_active_by_operation: process "
				  "INACTIVE, skipping\n",
				  MODULE_NAME);
			continue;
		}

		if (strcmp(proc->op, "read") == 0) {
			(*read_count)++;
		} else if (strcmp(proc->op, "write") == 0) {
			(*write_count)++;
		}
	}

	DBG_DEBUG("[%s] Active processes: read=%d write=%d (timed_out=%d)\n",
		  MODULE_NAME,
		  *read_count,
		  *write_count,
		  timed_out);
}

static void cleanup_inactive_processes(struct smbd_ratelimitd_state *state)
{
	uint64_t now = time_now_usec();
	struct process_activity *proc, *next;
	unsigned int removed = 0;

	for (proc = state->processes; proc != NULL; proc = next) {
		uint64_t age_us = now - proc->last_seen_usec;
		next = proc->next;

		if (!proc->is_active && age_us > INACTIVE_CLEANUP_THRESHOLD_US)
		{
			DLIST_REMOVE(state->processes, proc);
			TALLOC_FREE(proc);
			removed++;
		}
	}

	if (removed > 0) {
		DBG_NOTICE("[%s] Cleaned up %d inactive processes\n",
			   MODULE_NAME,
			   removed);
	}
}

static void broadcast_summary(struct smbd_ratelimitd_state *state,
			      const char *op,
			      int32_t count,
			      uint64_t timestamp_usec)
{
	struct ratelimit_node_summary summary = {0};
	DATA_BLOB blob;

	if (count <= 0) {
		return;
	}

	summary.vnn = state->my_vnn;
	summary.process_count = count;
	summary.timestamp_usec = timestamp_usec;

	blob = data_blob_const(&summary, sizeof(summary));

	messaging_send_all(state->msg_ctx,
			   ratelimit_msg_type_summary(
				   ratelimit_op_from_string(op)),
			   blob.data,
			   blob.length);

	state->total_broadcasts_sent++;
}

static void broadcast_timer_handler(struct tevent_context *ev,
				    struct tevent_timer *te,
				    struct timeval current_time,
				    void *private_data)
{
	struct smbd_ratelimitd_state *state = private_data;
	uint64_t now = time_now_usec();
	int32_t read_count = 0;
	int32_t write_count = 0;

	count_active_by_operation(state, &read_count, &write_count);

	broadcast_summary(state, "read", read_count, now);
	broadcast_summary(state, "write", write_count, now);

	if (read_count == 0 && write_count == 0) {
		DBG_DEBUG("[%s] No active processes, skipped broadcasts\n",
			  MODULE_NAME);
	}

	state->last_broadcast_usec = now;

	/* Cleanup old inactive entries every 100 broadcasts (~100 seconds) */
	if (state->total_broadcasts_sent % 100 == 0) {
		cleanup_inactive_processes(state);
	}

	DBG_DEBUG("[%s] broadcast_timer_handler: rescheduling timer\n",
		  MODULE_NAME);

	state->broadcast_timer = tevent_add_timer(
		state->ev,
		state,
		timeval_current_ofs(RATELIMITD_BROADCAST_INTERVAL_US / 1000000,
				    RATELIMITD_BROADCAST_INTERVAL_US %
					    1000000),
		broadcast_timer_handler,
		state);
}

struct tevent_req *smbd_ratelimitd_send(TALLOC_CTX *mem_ctx,
					struct tevent_context *ev,
					struct messaging_context *msg,
					pid_t parent_pid)
{
	struct tevent_req *req;
	struct smbd_ratelimitd_state *state;
	struct sockaddr_un addr;
	struct tevent_fd *fde;
	char *socket_path = NULL;
	size_t len;
	int ret;

	req = tevent_req_create(mem_ctx, &state, struct smbd_ratelimitd_state);
	if (req == NULL) {
		return NULL;
	}

	state->ev = ev;
	state->msg_ctx = msg;
	state->my_vnn = get_my_vnn();
	state->unix_sock = -1;
	state->socket_path = NULL;

	talloc_set_destructor(state, smbd_ratelimitd_state_destructor);

	state->processes = NULL;

	socket_path = state_path(state, RATELIMITD_SOCKET_NAME);
	if (socket_path == NULL) {
		DBG_ERR("[%s] Failed to allocate socket path\n", MODULE_NAME);
		tevent_req_error(req, ENOMEM);
		return tevent_req_post(req, ev);
	}

	state->socket_path = talloc_strdup(state, socket_path);
	if (state->socket_path == NULL) {
		DBG_ERR("[%s] Failed to duplicate socket path\n", MODULE_NAME);
		tevent_req_error(req, ENOMEM);
		return tevent_req_post(req, ev);
	}

	state->unix_sock = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (state->unix_sock < 0) {
		DBG_ERR("[%s] socket() failed: %s\n",
			MODULE_NAME,
			strerror(errno));
		tevent_req_error(req, errno);
		return tevent_req_post(req, ev);
	}

	/* Remove stale socket from previous daemon instance */
	ret = unlink(socket_path);
	if (ret == 0) {
		DBG_DEBUG("[%s] Removed stale socket: %s\n",
			  MODULE_NAME,
			  socket_path);
	} else if (errno != ENOENT) {
		DBG_DEBUG("[%s] unlink(%s) failed: %s (continuing anyway)\n",
			  MODULE_NAME,
			  socket_path,
			  strerror(errno));
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;

	len = strlcpy(addr.sun_path, socket_path, sizeof(addr.sun_path));
	if (len >= sizeof(addr.sun_path)) {
		DBG_ERR("[%s] Socket path too long: %s\n",
			MODULE_NAME,
			socket_path);
		close(state->unix_sock);
		state->unix_sock = -1;
		tevent_req_error(req, ENAMETOOLONG);
		return tevent_req_post(req, ev);
	}

	ret = bind(state->unix_sock, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		DBG_ERR("[%s] bind() failed on %s: %s\n",
			MODULE_NAME,
			socket_path,
			strerror(errno));
		close(state->unix_sock);
		state->unix_sock = -1;
		tevent_req_error(req, errno);
		return tevent_req_post(req, ev);
	}

	ret = fchmod(state->unix_sock, 0666);
	if (ret < 0) {
		DBG_WARNING("[%s] fchmod() failed: %s\n",
			    MODULE_NAME,
			    strerror(errno));
	}

	fde = tevent_add_fd(state->ev,
			    state,
			    state->unix_sock,
			    TEVENT_FD_READ,
			    handle_unix_socket_read,
			    state);
	if (fde == NULL) {
		DBG_ERR("[%s] tevent_add_fd failed\n", MODULE_NAME);
		close(state->unix_sock);
		state->unix_sock = -1;
		tevent_req_error(req, ENOMEM);
		return tevent_req_post(req, ev);
	}

	state->broadcast_timer = tevent_add_timer(
		state->ev,
		state,
		timeval_current_ofs(RATELIMITD_BROADCAST_INTERVAL_US / 1000000,
				    RATELIMITD_BROADCAST_INTERVAL_US %
					    1000000),
		broadcast_timer_handler,
		state);
	if (state->broadcast_timer == NULL) {
		DBG_ERR("[%s] tevent_add_timer failed\n", MODULE_NAME);
		close(state->unix_sock);
		state->unix_sock = -1;
		tevent_req_error(req, ENOMEM);
		return tevent_req_post(req, ev);
	}

	DBG_NOTICE("[%s] Daemon initialized: vnn=%u socket=%s\n",
		   MODULE_NAME,
		   state->my_vnn,
		   socket_path);

	return req;
}

NTSTATUS smbd_ratelimitd_recv(struct tevent_req *req)
{
	return tevent_req_simple_recv_ntstatus(req);
}
