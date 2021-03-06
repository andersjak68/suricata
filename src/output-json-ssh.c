/* Copyright (C) 2014 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 *
 * Implements SSH JSON logging portion of the engine.
 */

#include "suricata-common.h"
#include "debug.h"
#include "detect.h"
#include "pkt-var.h"
#include "conf.h"

#include "threads.h"
#include "threadvars.h"
#include "tm-threads.h"

#include "util-print.h"
#include "util-unittest.h"

#include "util-debug.h"
#include "app-layer-parser.h"
#include "output.h"
#include "app-layer-ssh.h"
#include "app-layer.h"
#include "util-privs.h"
#include "util-buffer.h"

#include "util-logopenfile.h"
#include "util-crypt.h"

#include "output-json.h"

#ifdef HAVE_LIBJANSSON
#include <jansson.h>

#define MODULE_NAME "LogSshLog"

typedef struct OutputSshCtx_ {
    LogFileCtx *file_ctx;
    uint32_t flags; /** Store mode */
} OutputSshCtx;


typedef struct JsonSshLogThread_ {
    OutputSshCtx *sshlog_ctx;
    MemBuffer *buffer;
} JsonSshLogThread;


void JsonSshLogJSON(json_t *tjs, SshState *ssh_state)
{
    json_t *cjs = json_object();
    if (cjs != NULL) {
        json_object_set_new(cjs, "proto_version",
                json_string((char *)ssh_state->cli_hdr.proto_version));

        json_object_set_new(cjs, "software_version",
                json_string((char *)ssh_state->cli_hdr.software_version));
    }
    json_object_set_new(tjs, "client", cjs);

    json_t *sjs = json_object();
    if (sjs != NULL) {
        json_object_set_new(sjs, "proto_version",
                json_string((char *)ssh_state->srv_hdr.proto_version));

        json_object_set_new(sjs, "software_version",
                json_string((char *)ssh_state->srv_hdr.software_version));
    }
    json_object_set_new(tjs, "server", sjs);

}

static int JsonSshLogger(ThreadVars *tv, void *thread_data, const Packet *p)
{
    JsonSshLogThread *aft = (JsonSshLogThread *)thread_data;
    MemBuffer *buffer = (MemBuffer *)aft->buffer;
    OutputSshCtx *ssh_ctx = aft->sshlog_ctx;

    if (unlikely(p->flow == NULL)) {
        return 0;
    }

    /* check if we have SSH state or not */
    FLOWLOCK_WRLOCK(p->flow);
    uint16_t proto = FlowGetAppProtocol(p->flow);
    if (proto != ALPROTO_SSH)
        goto end;

    SshState *ssh_state = (SshState *)FlowGetAppState(p->flow);
    if (unlikely(ssh_state == NULL)) {
        goto end;
    }

    if (ssh_state->cli_hdr.software_version == NULL || ssh_state->srv_hdr.software_version == NULL)
        goto end;

    json_t *js = CreateJSONHeader((Packet *)p, 1, "ssh");//TODO
    if (unlikely(js == NULL))
        goto end;

    json_t *tjs = json_object();
    if (tjs == NULL) {
        free(js);
        goto end;
    }

    /* reset */
    MemBufferReset(buffer);

    JsonSshLogJSON(tjs, ssh_state);

    json_object_set_new(js, "ssh", tjs);

    OutputJSONBuffer(js, ssh_ctx->file_ctx, buffer);
    json_object_clear(js);
    json_decref(js);

    /* we only log the state once */
    ssh_state->cli_hdr.flags |= SSH_FLAG_STATE_LOGGED;
end:
    FLOWLOCK_UNLOCK(p->flow);
    return 0;
}

#define OUTPUT_BUFFER_SIZE 65535
static TmEcode JsonSshLogThreadInit(ThreadVars *t, void *initdata, void **data)
{
    JsonSshLogThread *aft = SCMalloc(sizeof(JsonSshLogThread));
    if (unlikely(aft == NULL))
        return TM_ECODE_FAILED;
    memset(aft, 0, sizeof(JsonSshLogThread));

    if(initdata == NULL)
    {
        SCLogDebug("Error getting context for HTTPLog.  \"initdata\" argument NULL");
        SCFree(aft);
        return TM_ECODE_FAILED;
    }

    /* Use the Ouptut Context (file pointer and mutex) */
    aft->sshlog_ctx = ((OutputCtx *)initdata)->data;

    aft->buffer = MemBufferCreateNew(OUTPUT_BUFFER_SIZE);
    if (aft->buffer == NULL) {
        SCFree(aft);
        return TM_ECODE_FAILED;
    }

    *data = (void *)aft;
    return TM_ECODE_OK;
}

static TmEcode JsonSshLogThreadDeinit(ThreadVars *t, void *data)
{
    JsonSshLogThread *aft = (JsonSshLogThread *)data;
    if (aft == NULL) {
        return TM_ECODE_OK;
    }

    MemBufferFree(aft->buffer);
    /* clear memory */
    memset(aft, 0, sizeof(JsonSshLogThread));

    SCFree(aft);
    return TM_ECODE_OK;
}

static void OutputSshLogDeinit(OutputCtx *output_ctx)
{
    OutputSshLoggerDisable();

    OutputSshCtx *ssh_ctx = output_ctx->data;
    LogFileCtx *logfile_ctx = ssh_ctx->file_ctx;
    LogFileFreeCtx(logfile_ctx);
    SCFree(ssh_ctx);
    SCFree(output_ctx);
}

#define DEFAULT_LOG_FILENAME "ssh.json"
OutputCtx *OutputSshLogInit(ConfNode *conf)
{
    if (OutputSshLoggerEnable() != 0) {
        SCLogError(SC_ERR_CONF_YAML_ERROR, "only one 'ssh' logger "
            "can be enabled");
        return NULL;
    }

    LogFileCtx *file_ctx = LogFileNewCtx();
    if(file_ctx == NULL) {
        SCLogError(SC_ERR_HTTP_LOG_GENERIC, "couldn't create new file_ctx");
        return NULL;
    }

    if (SCConfLogOpenGeneric(conf, file_ctx, DEFAULT_LOG_FILENAME, 1) < 0) {
        LogFileFreeCtx(file_ctx);
        return NULL;
    }

    OutputSshCtx *ssh_ctx = SCMalloc(sizeof(OutputSshCtx));
    if (unlikely(ssh_ctx == NULL)) {
        LogFileFreeCtx(file_ctx);
        return NULL;
    }

    OutputCtx *output_ctx = SCCalloc(1, sizeof(OutputCtx));
    if (unlikely(output_ctx == NULL)) {
        LogFileFreeCtx(file_ctx);
        SCFree(ssh_ctx);
        return NULL;
    }

    ssh_ctx->file_ctx = file_ctx;

    output_ctx->data = ssh_ctx;
    output_ctx->DeInit = OutputSshLogDeinit;

    return output_ctx;
}

static void OutputSshLogDeinitSub(OutputCtx *output_ctx)
{
    OutputSshLoggerDisable();

    OutputSshCtx *ssh_ctx = output_ctx->data;
    SCFree(ssh_ctx);
    SCFree(output_ctx);
}

OutputCtx *OutputSshLogInitSub(ConfNode *conf, OutputCtx *parent_ctx)
{
    OutputJsonCtx *ojc = parent_ctx->data;

    if (OutputSshLoggerEnable() != 0) {
        SCLogError(SC_ERR_CONF_YAML_ERROR, "only one 'ssh' logger "
            "can be enabled");
        return NULL;
    }

    OutputSshCtx *ssh_ctx = SCMalloc(sizeof(OutputSshCtx));
    if (unlikely(ssh_ctx == NULL))
        return NULL;

    OutputCtx *output_ctx = SCCalloc(1, sizeof(OutputCtx));
    if (unlikely(output_ctx == NULL)) {
        SCFree(ssh_ctx);
        return NULL;
    }

    ssh_ctx->file_ctx = ojc->file_ctx;

    output_ctx->data = ssh_ctx;
    output_ctx->DeInit = OutputSshLogDeinitSub;

    return output_ctx;
}

/** \internal
 *  \brief Condition function for SSH logger
 *  \retval bool true or false -- log now?
 */
static int JsonSshCondition(ThreadVars *tv, const Packet *p)
{
    if (p->flow == NULL) {
        return FALSE;
    }

    if (!(PKT_IS_TCP(p))) {
        return FALSE;
    }

    FLOWLOCK_RDLOCK(p->flow);
    uint16_t proto = FlowGetAppProtocol(p->flow);
    if (proto != ALPROTO_SSH)
        goto dontlog;

    SshState *ssh_state = (SshState *)FlowGetAppState(p->flow);
    if (ssh_state == NULL) {
        SCLogDebug("no ssh state, so no logging");
        goto dontlog;
    }

    /* we only log the state once */
    if (ssh_state->cli_hdr.flags & SSH_FLAG_STATE_LOGGED)
        goto dontlog;

    if (ssh_state->cli_hdr.software_version == NULL ||
        ssh_state->srv_hdr.software_version == NULL)
        goto dontlog;

    /* todo: logic to log once */

    FLOWLOCK_UNLOCK(p->flow);
    return TRUE;
dontlog:
    FLOWLOCK_UNLOCK(p->flow);
    return FALSE;
}

void TmModuleJsonSshLogRegister (void)
{
    tmm_modules[TMM_JSONSSHLOG].name = "JsonSshLog";
    tmm_modules[TMM_JSONSSHLOG].ThreadInit = JsonSshLogThreadInit;
    tmm_modules[TMM_JSONSSHLOG].ThreadDeinit = JsonSshLogThreadDeinit;
    tmm_modules[TMM_JSONSSHLOG].RegisterTests = NULL;
    tmm_modules[TMM_JSONSSHLOG].cap_flags = 0;
    tmm_modules[TMM_JSONSSHLOG].flags = TM_FLAG_LOGAPI_TM;

    /* register as separate module */
    OutputRegisterPacketModule("JsonSshLog", "ssh-json-log", OutputSshLogInit,
            JsonSshLogger, JsonSshCondition);

    /* also register as child of eve-log */
    OutputRegisterPacketSubModule("eve-log", "JsonSshLog", "eve-log.ssh", OutputSshLogInitSub,
            JsonSshLogger, JsonSshCondition);
}

#else

static TmEcode OutputJsonThreadInit(ThreadVars *t, void *initdata, void **data)
{
    SCLogInfo("Can't init JSON output - JSON support was disabled during build.");
    return TM_ECODE_FAILED;
}

void TmModuleJsonSshLogRegister (void)
{
    tmm_modules[TMM_JSONSSHLOG].name = "JsonSshLog";
    tmm_modules[TMM_JSONSSHLOG].ThreadInit = OutputJsonThreadInit;
}

#endif
