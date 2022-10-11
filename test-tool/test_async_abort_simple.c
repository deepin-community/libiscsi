/*
   Copyright (C) SUSE LINUX GmbH 2016

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <signal.h>
#include <poll.h>

#include <CUnit/CUnit.h>

#include "iscsi.h"
#include "scsi-lowlevel.h"
#include "iscsi-support.h"
#include "iscsi-test-cu.h"

struct tests_async_abort_state {
	struct scsi_task *wtask;
	uint32_t wr_cancelled;
	uint32_t wr_good;
	uint32_t abort_ok;
	uint32_t abort_bad_itt;
};

static void
test_async_write_cb(struct iscsi_context *iscsi __attribute__((unused)),
		    int status, void *command_data,
		    void *private_data)
{
	struct scsi_task *wtask = command_data;
	struct tests_async_abort_state *state = private_data;

	if (status == SCSI_STATUS_GOOD) {
		state->wr_good++;
		logging(LOG_VERBOSE, "WRITE10 successful: (CmdSN=0x%x, "
			"ITT=0x%x)", wtask->cmdsn, wtask->itt);
	} else if (status == SCSI_STATUS_CANCELLED) {
		state->wr_cancelled++;
		logging(LOG_VERBOSE, "WRITE10 cancelled: (CmdSN=0x%x, "
			"ITT=0x%x)", wtask->cmdsn, wtask->itt);
	} else {
		CU_ASSERT_NOT_EQUAL(status, SCSI_STATUS_CHECK_CONDITION);
	}
}

static void
test_async_abort_cb(struct iscsi_context *iscsi __attribute__((unused)),
		    int status, void *command_data,
		    void *private_data)
{
	uint32_t tmf_response;
	struct tests_async_abort_state *state = private_data;

	/* command_data NULL if a reconnect occured. see iscsi_reconnect_cb() */
	CU_ASSERT_PTR_NOT_NULL_FATAL(command_data);
	tmf_response = *(uint32_t *)command_data;

	logging(LOG_VERBOSE, "ABORT TASK: TMF response %d for"
		" RefCmdSN=0x%x, RefITT=0x%x",
		tmf_response, state->wtask->cmdsn, state->wtask->itt);
	if (tmf_response == ISCSI_TMR_FUNC_COMPLETE) {
		state->abort_ok++;
		logging(LOG_VERBOSE, "ABORT TASK completed");
	} else if (tmf_response == ISCSI_TMR_TASK_DOES_NOT_EXIST) {
		/* expected if the write has already been handled by the tgt */
		state->abort_bad_itt++;
		logging(LOG_VERBOSE, "ABORT TASK bad ITT");
	} else {
		logging(LOG_NORMAL, "ABORT TASK: unexpected TMF response %d for"
			" RefCmdSN=0x%x, RefITT=0x%x",
			tmf_response, state->wtask->cmdsn, state->wtask->itt);
		CU_ASSERT_FATAL((tmf_response != ISCSI_TMR_FUNC_COMPLETE)
			    && (tmf_response != ISCSI_TMR_TASK_DOES_NOT_EXIST));
	}
	CU_ASSERT_NOT_EQUAL(status, SCSI_STATUS_CHECK_CONDITION);
}

void
test_async_abort_simple(void)
{
	int ret;
	struct tests_async_abort_state state = { NULL, 0, 0, 0, 0 };
	int blocksize = 512;
	int blocks_per_io = 8;
	unsigned char buf[blocksize * blocks_per_io];
	uint64_t timeout_sec;

	CHECK_FOR_DATALOSS;
	CHECK_FOR_SBC;
	if (sd->iscsi_ctx == NULL) {
                CU_PASS("[SKIPPED] Non-iSCSI");
		return;
	}

	if (maximum_transfer_length
	 && (maximum_transfer_length < (int)(blocks_per_io))) {
		CU_PASS("[SKIPPED] device too small for async_abort test");
		return;
	}

	memset(buf, 0, blocksize * blocks_per_io);

	/* queue and dispatch write before the abort */
	state.wtask = scsi_cdb_write10(0, blocks_per_io * blocksize,
				 blocksize, 0, 0, 0, 0, 0);
	CU_ASSERT_PTR_NOT_NULL_FATAL(state.wtask);

	ret = scsi_task_add_data_out_buffer(state.wtask,
					    blocks_per_io * blocksize,
					    buf);
	CU_ASSERT_EQUAL(ret, 0);

	ret = iscsi_scsi_command_async(sd->iscsi_ctx, sd->iscsi_lun,
				       state.wtask, test_async_write_cb, NULL,
				       &state);
	CU_ASSERT_EQUAL(ret, 0);

	logging(LOG_VERBOSE, "WRITE10 queued: (CmdSN=0x%x, ITT=0x%x)",
		state.wtask->cmdsn, state.wtask->itt);

	CU_ASSERT_EQUAL(iscsi_out_queue_length(sd->iscsi_ctx), 1);

	logging(LOG_VERBOSE, "dispatching out queue...");
	while ((uint32_t)iscsi_out_queue_length(sd->iscsi_ctx) > 0) {
		struct pollfd pfd;

		pfd.fd = iscsi_get_fd(sd->iscsi_ctx);
		pfd.events = POLLOUT;	/* only send */

		ret = poll(&pfd, 1, 1000);
		CU_ASSERT_NOT_EQUAL(ret, -1);

		ret = iscsi_service(sd->iscsi_ctx, pfd.revents);
		CU_ASSERT_EQUAL(ret, 0);
	}
	logging(LOG_VERBOSE, "dispatched");

	/*
	 * queue abort - shouldn't cancel the dispatched task. TMF req should
	 * be sent to the target.
	 */
	ret = iscsi_task_mgmt_async(sd->iscsi_ctx,
				    state.wtask->lun, ISCSI_TM_ABORT_TASK,
				    state.wtask->itt, state.wtask->cmdsn,
				    test_async_abort_cb, &state);
	CU_ASSERT_EQUAL(ret, 0);

	logging(LOG_VERBOSE, "ABORT queued: (RefCmdSN=0x%x, "
		"RefITT=0x%x)", state.wtask->cmdsn, state.wtask->itt);

	/*
	 * wait for all responses, timeout in 5 seconds. Expected responses:
	 * + WRITE:good, ABORT:bad_itt - write completed before abort
	 * + WRITE:no-response, ABORT:ok - write aborted
	 */
	logging(LOG_VERBOSE, "dispatching abort and handling responses...");
	timeout_sec = test_get_clock_sec() + 5;
	while (test_get_clock_sec() <= timeout_sec) {
		struct pollfd pfd;

		pfd.fd = iscsi_get_fd(sd->iscsi_ctx);
		pfd.events = iscsi_which_events(sd->iscsi_ctx);

		ret = poll(&pfd, 1, 1000);
		CU_ASSERT_NOT_EQUAL(ret, -1);

		ret = iscsi_service(sd->iscsi_ctx, pfd.revents);
		CU_ASSERT_EQUAL(ret, 0);

		if (((state.wr_good == 1) && (state.abort_bad_itt == 1))
		 || (state.abort_ok == 1)) {
			logging(LOG_VERBOSE, "received all expected responses");
			break;
		}
	}

	logging(LOG_VERBOSE, "%d IOs completed, %d aborts successful, "
		"%d aborts unsuccessful",
		state.wr_good, state.abort_ok, state.abort_bad_itt);

	if (state.abort_ok == 1) {
		CU_ASSERT_EQUAL(state.wr_good, 0);
		CU_ASSERT_EQUAL(state.wr_cancelled, 0);
		CU_ASSERT_EQUAL(state.abort_bad_itt, 0);
	} else if (state.abort_bad_itt == 1) {
		CU_ASSERT_EQUAL(state.wr_good, 1);
		CU_ASSERT_EQUAL(state.wr_cancelled, 0);
		CU_ASSERT_EQUAL(state.abort_ok, 0);
	} else {
		CU_FAIL("unexpected WRITE/ABORT state");
	}

	scsi_free_scsi_task(state.wtask);

	/* Avoid that callbacks get invoked after this test finished */
        iscsi_logout_sync(sd->iscsi_ctx);
        iscsi_destroy_context(sd->iscsi_ctx);
	sd->iscsi_ctx = NULL;
}