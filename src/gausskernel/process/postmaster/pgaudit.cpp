/*
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 2004-2012, PostgreSQL Global Development Group
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * -------------------------------------------------------------------------
 *
 * pgaudit.cpp
 * 		auditor process
 *
 * 		The audit collector (auditor) catches all audit output from
 * 		the postmaster, backends, and other subprocesses by redirecting to a
 * 		pipe, and writes it to a set of auditfiles.
 * 		It's possible to have size and age limits for the auditfile configured
 * 		in postgresql.conf. If these limits are reached or passed, the
 * 		current auditfile is closed and a new one is created (rotated).
 * 		The auditfiles are stored in a subdirectory (configurable in
 * 		postgresql.conf), using a user-selectable naming scheme.
 *
 * IDENTIFICATION
 *	  src/gausskernel/process/postmaster/pgaudit.cpp
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include <fcntl.h>
#include <limits.h> /* for PIPE_BUF */
#include <sys/time.h>

#include "lib/stringinfo.h"
#include "libpq/libpq-be.h"
#include "libpq/pqsignal.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "pgtime.h"
#include "postmaster/fork_process.h"
#include "postmaster/postmaster.h"
#include "pgaudit.h"
#include "pgxc/pgxc.h"
#include "storage/ipc.h"
#include "storage/fd.h"
#include "storage/latch.h"
#include "storage/pg_shmem.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/timestamp.h"
#include "utils/builtins.h"
#include "utils/acl.h"

#include "gssignal/gs_signal.h"

/*
 * Primitive protocol structure for writing to sysauditor pipe(s).  The idea
 * here is to divide long messages into chunks that are not more than
 * PIPE_BUF bytes long, which according to POSIX spec must be written into
 * the pipe atomically.  The pipe reader then uses the protocol headers to
 * reassemble the parts of a message into a single string.	The reader can
 * also cope with non-protocol data coming down the pipe, though we cannot
 * guarantee long strings won't get split apart.
 *
 * We use non-nul bytes in is_last to make the protocol a tiny bit
 * more robust against finding a false double nul byte prologue. But
 * we still might find it in the len and/or pid bytes unless we're careful.
 */
#ifdef PIPE_BUF
/* Are there any systems with PIPE_BUF > 64K?  Unlikely, but ... */
#if PIPE_BUF > 65536
#define PIPE_CHUNK_SIZE 65536
#else
#define PIPE_CHUNK_SIZE ((int)PIPE_BUF)
#endif
#else /* not defined */
/* POSIX says the value of PIPE_BUF must be at least 512, so use that */
#define PIPE_CHUNK_SIZE 512
#endif

typedef struct {
    char nuls[2]; /* always \0\0 */
    uint16 len;   /* size of this chunk (counts data only) */
    ThreadId pid; /* writer's pid */
    char is_last; /* last chunk of message? 't' or 'f' ('T' or
                   * 'F' for CSV case) */
    char data[1]; /* data payload starts here */
} PipeProtoHeader;

typedef union {
    PipeProtoHeader proto;
    char filler[PIPE_CHUNK_SIZE];
} PipeProtoChunk;

#define PIPE_HEADER_SIZE offsetof(PipeProtoHeader, data)
#define PIPE_MAX_PAYLOAD ((int)(PIPE_CHUNK_SIZE - PIPE_HEADER_SIZE))

/* The indextbl->count should meet a relationship with curidx and begidx. */
#define AUDIT_COUNT(indextbl)                                                                   \
    (((indextbl)->curidx >= (indextbl)->begidx) ? ((indextbl)->curidx - (indextbl)->begidx + 1) \
                                                : ((indextbl)->curidx + (indextbl)->maxnum + 1 - (indextbl)->begidx))

/*
 * We really want line-buffered mode for auditfile output, but Windows does
 * not have it, and interprets _IOLBF as _IOFBF (bozos).  So use _IONBF
 * instead on Windows.
 */
#ifdef WIN32
#define LBF_MODE _IONBF
#else
#define LBF_MODE _IOLBF
#endif

/*
 * We read() into a temp buffer twice as big as a chunk, so that any fragment
 * left after processing can be moved down to the front and we'll still have
 * room to read a full chunk.
 */
#define READ_BUF_SIZE (2 * PIPE_CHUNK_SIZE)

/*
 * Brief		: bitnum in integer Audit_Session
 * Description	:
 */
typedef enum { SESSION_LOGIN_SUCCESS = 0, SESSION_LOGIN_FAILED, SESSION_LOGOUT } SessionType;

/*
 * Globally visible state (used by postmaster.c)
 */
static bool auditpipe_done = false; /* build audit pipe for auditor process? */

/*
 * Private state
 */
static const char* pgaudit_filename = "%s/%d_adt";
static int pgaudit_filemode = S_IRUSR | S_IWUSR;

/*
 * Buffers for saving partial messages from different backends.
 *
 * Keep NBUFFER_LISTS lists of these, with the entry for a given source pid
 * being in the list numbered (pid % NBUFFER_LISTS), so as to cut down on
 * the number of entries we have to examine for any one incoming message.
 * There must never be more than one entry for the same source pid.
 *
 * An inactive buffer is not removed from its list, just held for re-use.
 * An inactive buffer has pid == 0 and undefined contents of data.
 */
typedef struct {
    ThreadId pid;        /* PID of source process */
    StringInfoData data; /* accumulated data, as a StringInfo */
} save_buffer;

#define NBUFFER_LISTS 256

/* These must be exported for EXEC_BACKEND case ... annoying */
#ifndef WIN32
int sysauditPipe[2] = {-1, -1};
#else
HANDLE sysauditPipe[2] = {0, 0};
#endif

#ifdef WIN32
static HANDLE threadHandle = 0;
static CRITICAL_SECTION sysauditorSection;
#endif

/*
 * Flags set by interrupt handlers for later service in the main loop.
 */
#define SPACE_INTERVAL_SIZE (10 * 1024 * 1024)                          // 10MB
const static uint64 SPACE_MAXIMUM_SIZE = (1024 * 1024 * 1024 * 1024L);  // 1024GB
/* The static variable for print log when exceeding the space limit */

/*
 * Brief		: audit index item in index table
 * Description	:
 */
typedef struct AuditIndexItem {
    /*
     * file create time. used when scan all the audit data.
     * if system time changed when auditor write into this file,
     * then ctime would less than zero.
     */
    pg_time_t ctime;

    uint32 filenum;  /* file number */
    uint32 filesize; /* file size */
} AuditIndexItem;

/*
 * Brief		: audit index table
 * Description	:
 */
typedef struct AuditIndexTable {
    uint32 maxnum;             /* max count of the audit index item */
    uint32 begidx;             /* the position of the first audit index item */
    uint32 curidx;             /* the position of the current audit index item */
    uint32 count;              /* the count of the audit index item */
    pg_time_t last_audit_time; /* the audit time of the latest audit record */
    AuditIndexItem data[1];
} AuditIndexTable;

static const char audit_indextbl_file[] = "index_table";
static const int indextbl_header_size = offsetof(AuditIndexTable, data);

static const char* AuditTypeDescs[] = {"unknown",
    "login_success",
    "login_failed",
    "user_logout",
    "system_start",
    "system_stop",
    "system_recover",
    "system_switch",
    "lock_user",
    "unlock_user",
    "grant_role",
    "revoke_role",
    "user_violation",
    "ddl_database",
    "ddl_directory",
    "ddl_tablespace",
    "ddl_schema",
    "ddl_user",
    "ddl_table",
    "ddl_index",
    "ddl_view",
    "ddl_trigger",
    "ddl_function",
    "ddl_resourcepool",
    "ddl_workload",
    "ddl_serverforhadoop",
    "ddl_datasource",
    "ddl_nodegroup",
    "ddl_rowlevelsecurity",
    "ddl_synonym",
    "ddl_type",
    "ddl_textsearch",
    "dml_action",
    "dml_action_select",
    "internal_event",
    "function_exec",
    "copy_to",
    "copy_from",
    "set_parameter"};

static const int AuditTypeNum = sizeof(AuditTypeDescs) / sizeof(char*);

#define AuditTypeDesc(type) (((type) > 0 && (type) < AuditTypeNum) ? AuditTypeDescs[(type)] : AuditTypeDescs[0])

static const char* AuditResultDescs[] = {"unknown", "ok", "failed"};

static const int AuditResultNum = sizeof(AuditResultDescs) / sizeof(char*);

#define AuditResultDesc(type) (((type) > 0 && (type) < AuditResultNum) ? AuditResultDescs[(type)] : AuditResultDescs[0])

/*
 * Brief		: The audit message header
 * Description	: exactly 160 bits.
 */
typedef struct AuditMsgHdr {
    char signature[2]; /* always 'A''U' */
    uint16 version;    /* current is 0 */
    uint16 fields;     /* the field counts */
    uint16 flags;      /* flags mark the tuple is deleted */
    pg_time_t time;    /* audit time */
    uint32 size;       /* record length */
} AuditMsgHdr;

#define AUDIT_TUPLE_NORMAL 1 /* normal tuple */
#define AUDIT_TUPLE_DEAD 2   /* dead tuple */

typedef struct AuditEncodedData {
    AuditMsgHdr header;
    char data[1]; /* data payload starts here */
} AuditEncodedData;

/*
 * AuditData holds the data accumulated during any one audit_report() cycle.
 * Any non-NULL pointers must point to palloc'd data.
 * (The const pointers are an exception; we assume they point at non-freeable
 * constant strings.)
 */
typedef struct AuditData {
    AuditMsgHdr header;
    AuditType type;
    AuditResult result;
    char varstr[1]; /* variable length array - must be last */
} AuditData;

#define AUDIT_HEADER_SIZE offsetof(AuditData, varstr)

/*
 * Brief		: the string field number in audit record
 * Description	:
 */
typedef enum {
    AUDIT_USER_ID = 0,
    AUDIT_USER_NAME,
    AUDIT_DATABASE_NAME,
    AUDIT_CLIENT_CONNINFO,
    AUDIT_OBJECT_NAME,
    AUDIT_DETAIL_INFO,
    AUDIT_NODENAME_INFO,
    AUDIT_THREADID_INFO,
    AUDIT_LOCALPORT_INFO,
    AUDIT_REMOTEPORT_INFO
} AuditStringFieldNum;

#define PGAUDIT_RESTART_INTERVAL 60

#define PGAUDIT_QUERY_COLS 13

#define MAXNUMLEN 16
/* Local subroutines */
static void process_pipe_input(char* auditbuffer, int* bytes_in_auditbuffer);
static void flush_pipe_input(char* auditbuffer, int* bytes_in_auditbuffer);
static void pgaudit_write_file(char* buffer, int count);
static void auditfile_init(void);
static FILE* auditfile_open(pg_time_t timestamp, const char* mode, bool allow_errors);
static void auditfile_close(void);

#ifdef WIN32
static unsigned int __stdcall pipeThread(void* arg);
#endif
static void auditfile_rotate(bool time_based_rotation, bool size_based_rotation);
static void set_next_rotation_time(void);
static void pgaudit_cleanup(void);

static void pgaudit_exit(SIGNAL_ARGS);
static void sigHupHandler(SIGNAL_ARGS);
static void sigUsr1Handler(SIGNAL_ARGS);

static void write_pipe_chunks(char* data, int len);
static void appendStringField(StringInfo str, const char* s);
static void pgaudit_close_file(FILE* fp, const char* file);
static void pgaudit_read_indexfile(const char* audit_directory);
static void pgaudit_update_indexfile(const char* mode, bool allow_errors);
static void pgaudit_indextbl_init(void);
static const char* pgaudit_string_field(AuditData* adata, int num);
static void pgaudit_query_file(Tuplestorestate* state, TupleDesc tdesc, uint32 fnum, TimestampTz begtime,
    TimestampTz endtime, const char* audit_directory);
static bool check_audit_login(AuditType audittype);

/*
 * Main entry point for auditor process
 * argc/argv parameters are valid only in EXEC_BACKEND case.
 */
NON_EXEC_STATIC void PgAuditorMain()
{
#ifndef WIN32
    char auditbuffer[READ_BUF_SIZE + 1] = {0};
    int bytes_in_auditbuffer = 0;
#endif
    int currentAuditRotationAge;
    int currentAuditRemainThreshold;
    pg_time_t now;

    IsUnderPostmaster = true; /* we are a postmaster subprocess now */

    t_thrd.proc_cxt.MyProcPid = gs_thread_self(); /* reset t_thrd.proc_cxt.MyProcPid */

    t_thrd.proc_cxt.MyStartTime = time(NULL); /* set our start time in case we call elog */
    now = t_thrd.proc_cxt.MyStartTime;

    t_thrd.role = AUDIT;

    init_ps_display("auditor process", "", "", "");

    /*
     * Also close our copy of the write end of the pipe.  This is needed to
     * ensure we can detect pipe EOF correctly.  (But note that in the restart
     * case, the postmaster already did this.)
     */
    InitializeLatchSupport(); /* needed for latch waits */
    /* Initialize private latch for use by signal handlers */
    InitLatch(&t_thrd.audit.sysAuditorLatch);

    /*
     * Properly accept or ignore signals the postmaster might send us
     *
     * Note: we ignore all termination signals, and instead exit only when all
     * upstream processes are gone, to ensure we don't miss any dying gasps of
     * broken backends...
     */
    (void)gspqsignal(SIGHUP, sigHupHandler); /* set flag to read config file */
    (void)gspqsignal(SIGINT, SIG_IGN);
    (void)gspqsignal(SIGTERM, SIG_IGN);
    (void)gspqsignal(SIGQUIT, pgaudit_exit);
    (void)gspqsignal(SIGALRM, SIG_IGN);
    (void)gspqsignal(SIGPIPE, SIG_IGN);
    (void)gspqsignal(SIGUSR1, sigUsr1Handler); /* request audit rotation */
    (void)gspqsignal(SIGUSR2, SIG_IGN);

    /* Reset some signals that are accepted by postmaster but not here */
    (void)gspqsignal(SIGCHLD, SIG_DFL);
    (void)gspqsignal(SIGTTIN, SIG_DFL);
    (void)gspqsignal(SIGTTOU, SIG_DFL);
    (void)gspqsignal(SIGCONT, SIG_DFL);
    (void)gspqsignal(SIGWINCH, SIG_DFL);

    gs_signal_setmask(&t_thrd.libpq_cxt.UnBlockSig, NULL);
    (void)gs_signal_unblock_sigusr2();

    if (t_thrd.mem_cxt.pgAuditLocalContext == NULL)
        t_thrd.mem_cxt.pgAuditLocalContext = AllocSetContextCreate(t_thrd.top_mem_cxt,
            "audit memory context",
            ALLOCSET_DEFAULT_MINSIZE,
            ALLOCSET_DEFAULT_INITSIZE * 3,
            ALLOCSET_DEFAULT_MAXSIZE * 3);

    pgaudit_indextbl_init();

#ifdef WIN32
    /* Fire up separate data transfer thread */
    InitializeCriticalSection(&sysauditorSection);
    EnterCriticalSection(&sysauditorSection);

    threadHandle = (HANDLE)_beginthreadex(NULL, 0, pipeThread, NULL, 0, NULL);
    if (threadHandle == 0)
        ereport(FATAL, (errmsg("could not create sysauditor data transfer thread: %m")));
#endif /* WIN32 */

    /* remember active auditfile parameters */
    currentAuditRotationAge = u_sess->attr.attr_security.Audit_RotationAge;
    currentAuditRemainThreshold = u_sess->attr.attr_security.Audit_RemainThreshold;
    /* set next planned rotation time */
    set_next_rotation_time();

    /* main worker loop */
    for (;;) {
        bool time_based_rotation = false;
        bool size_based_rotation = false;
        long cur_timeout;
        unsigned int cur_flags;

#ifndef WIN32
        unsigned int rc = 0;
#endif

        /* Clear any already-pending wakeups */
        ResetLatch(&t_thrd.audit.sysAuditorLatch);

        /*
         * Quit if we get SIGQUIT from the postmaster.
         */
        if (t_thrd.audit.need_exit)
            break;

        /*
         * Process any requests or signals received recently.
         */
        if (t_thrd.audit.got_SIGHUP) {
            t_thrd.audit.got_SIGHUP = false;
            ProcessConfigFile(PGC_SIGHUP);

            /*
             * If rotation time parameter changed, reset next rotation time,
             * but don't immediately force a rotation.
             */
            if (currentAuditRotationAge != u_sess->attr.attr_security.Audit_RotationAge) {
                currentAuditRotationAge = u_sess->attr.attr_security.Audit_RotationAge;
                set_next_rotation_time();
            }

            /* If file remain threshold parameter changed, reset audit index table */
            if (currentAuditRemainThreshold != u_sess->attr.attr_security.Audit_RemainThreshold) {
                currentAuditRemainThreshold = u_sess->attr.attr_security.Audit_RemainThreshold;

                /* the audit index table may be dirty, so update index table first */
                pgaudit_update_indexfile(PG_BINARY_W, true);

                /* reset the audit index table */
                pgaudit_indextbl_init();
            }

            /*
             * If we had a rotation-disabling failure, re-enable rotation
             * attempts after SIGHUP, and force one immediately.
             */
            if (t_thrd.audit.rotation_disabled) {
                t_thrd.audit.rotation_disabled = false;
                t_thrd.audit.rotation_requested = true;
            }
        }

        if (u_sess->attr.attr_security.Audit_RotationAge > 0 && !t_thrd.audit.rotation_disabled) {
            /* Do a auditfile rotation if it's time */
            now = (pg_time_t)time(NULL);
            if (now >= t_thrd.audit.next_rotation_time)
                t_thrd.audit.rotation_requested = time_based_rotation = true;
        }

        if (!t_thrd.audit.rotation_requested && u_sess->attr.attr_security.Audit_RotationSize > 0 &&
            !t_thrd.audit.rotation_disabled) {
            int64 filesize = ftell(t_thrd.audit.sysauditFile);
            /* Do a rotation if file is too big */
            if (filesize >= u_sess->attr.attr_security.Audit_RotationSize * 1024L ||
                filesize >= u_sess->attr.attr_security.Audit_SpaceLimit * 1024L) {
                t_thrd.audit.rotation_requested = size_based_rotation = true;
            }
        }

        if (t_thrd.audit.rotation_requested) {
            /*
             * Force rotation when both values are zero. It means the request
             * was sent by pg_rotate_auditfile.
             */
            if (!time_based_rotation && !size_based_rotation)
                size_based_rotation = true;
            auditfile_rotate(time_based_rotation, size_based_rotation);
        }

        pgaudit_cleanup();

        /*
         * Calculate time till next time-based rotation, so that we don't
         * sleep longer than that.  We assume the value of "now" obtained
         * above is still close enough.  Note we can't make this calculation
         * until after calling auditfile_rotate(), since it will advance
         * t_thrd.audit.next_rotation_time.
         */
        if (u_sess->attr.attr_security.Audit_RotationAge > 0 && !t_thrd.audit.rotation_disabled) {
            if (now < t_thrd.audit.next_rotation_time)
                cur_timeout = (t_thrd.audit.next_rotation_time - now) * 1000L; /* msec */
            else
                cur_timeout = 0;

            cur_flags = WL_TIMEOUT;
        } else {
            cur_timeout = -1L;
            cur_flags = 0;
        }

        /*
         * Sleep until there's something to do
         */
#ifndef WIN32
        rc = WaitLatchOrSocket(
            &t_thrd.audit.sysAuditorLatch, WL_LATCH_SET | WL_SOCKET_READABLE | cur_flags, sysauditPipe[0], cur_timeout);

        if (rc & WL_SOCKET_READABLE) {
            int bytesRead;

            bytesRead = read(
                sysauditPipe[0], auditbuffer + bytes_in_auditbuffer, sizeof(auditbuffer) - bytes_in_auditbuffer - 1);
            if (bytesRead < 0) {
                if (errno != EINTR)
                    ereport(LOG, (errcode_for_socket_access(), errmsg("could not read from auditor pipe: %m")));
            } else if (bytesRead > 0) {
                bytes_in_auditbuffer += bytesRead;
                process_pipe_input(auditbuffer, &bytes_in_auditbuffer);
                continue;
            } else {
                /*
                 * Zero bytes read when select() is saying read-ready means
                 * EOF on the pipe: that is, there are no longer any processes
                 * with the pipe write end open.  Therefore, the postmaster
                 * and all backends are shut down, and we are done.
                 */
                t_thrd.audit.pipe_eof_seen = true;

                /* if there's any data left then force it out now */
                flush_pipe_input(auditbuffer, &bytes_in_auditbuffer);
            }
        }
#else  /* WIN32 */

        /*
         * On Windows we leave it to a separate thread to transfer data and
         * detect pipe EOF.  The main thread just wakes up to handle SIGHUP
         * and rotation conditions.
         *
         * Server code isn't generally thread-safe, so we ensure that only one
         * of the threads is active at a time by entering the critical section
         * whenever we're not sleeping.
         */
        LeaveCriticalSection(&sysauditorSection);

        (void)WaitLatch(&t_thrd.audit.sysAuditorLatch, WL_LATCH_SET | cur_flags, cur_timeout);

        EnterCriticalSection(&sysauditorSection);
#endif /* WIN32 */

        if (t_thrd.audit.pipe_eof_seen) {
            break;
        }
    }

    /*
     * seeing this message on the real stderr is annoying - so we make
     * it DEBUG1 to suppress in normal use.
     */
    ereport(DEBUG1, (errmsg("auditor shutting down")));

    pgaudit_cleanup();
    pgaudit_update_indexfile(PG_BINARY_W, true);
    if (t_thrd.audit.sysauditFile) {
        fclose(t_thrd.audit.sysauditFile);
        t_thrd.audit.sysauditFile = NULL;
    }

    /* Release memory, if any was allocated */
    if (t_thrd.mem_cxt.pgAuditLocalContext != NULL) {
        MemoryContextDelete(t_thrd.mem_cxt.pgAuditLocalContext);
        t_thrd.mem_cxt.pgAuditLocalContext = NULL;
    }

    if (sysauditPipe[0] > 0) {
        close(sysauditPipe[0]);
        sysauditPipe[0] = -1;
    }

    proc_exit(0);
}

/*
 * Postmaster subroutine to start a sysauditor subprocess.
 */
ThreadId pgaudit_start(void)
{
    pg_time_t curtime;
    ThreadId sysauditorPid;

    if (!u_sess->attr.attr_security.Audit_enabled)
        return 0;

    /*
     * Do nothing if too soon since last collector start.  This is a safety
     * valve to protect against continuous respawn attempts if the collector
     * is dying immediately at launch.	Note that since we will be re-called
     * from the postmaster main loop, we will get another chance later.
     */
    curtime = time(NULL);
    if ((unsigned int)(curtime - t_thrd.audit.last_pgaudit_start_time) < (unsigned int)PGAUDIT_RESTART_INTERVAL)
        return 0;
    t_thrd.audit.last_pgaudit_start_time = curtime;

    /*
     * If first time through, create the pipe which will receive audit
     * output.
     *
     * If the sysauditor crashes and needs to be restarted, we continue to use
     * the same pipe (indeed must do so, since extant backends will be writing
     * into that pipe).
     *
     * This means the postmaster must continue to hold the read end of the
     * pipe open, so we can pass it down to the reincarnated sysauditor. This
     * is a bit klugy but we have little choice.
     */
#ifndef WIN32
    if (sysauditPipe[0] < 0) {
        if (pipe(sysauditPipe) < 0)
            ereport(FATAL, (errcode_for_socket_access(), (errmsg("could not create pipe for sysaudit: %m"))));
    }
#else
    if (!sysauditPipe[0]) {
        SECURITY_ATTRIBUTES sa;
        errno_t errorno = EOK;

        errorno = memset_s(&sa, sizeof(SECURITY_ATTRIBUTES), 0, sizeof(SECURITY_ATTRIBUTES));
        securec_check(errorno, "\0", "\0");
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.bInheritHandle = TRUE;

        if (!CreatePipe(&sysauditPipe[0], &sysauditPipe[1], &sa, 32768))
            ereport(FATAL, (errcode_for_file_access(), (errmsg("could not create pipe for sysaudit: %m"))));
    }
#endif

    /*
     * Create audit directory if not present; ignore errors
     */
    (void)pg_mkdir_p(g_instance.attr.attr_security.Audit_directory, S_IRWXU);

    /*
     * The initial auditfile is created right in the postmaster, to verify that
     * the Audit_directory is writable.
     */
    pgaudit_update_indexfile(PG_BINARY_A, false);

    sysauditorPid = initialize_util_thread(AUDIT);
    if (sysauditorPid != 0) {
        /* success, in postmaster */
        if (!auditpipe_done) {
#ifdef WIN32
            int fd;
            /*
             * open the pipe in binary mode and make sure write pipe is binary,
             * to avoid disturbing the pipe chunking protocol.
             */
            fd = _open_osfhandle((intptr_t)sysauditPipe[1], _O_APPEND | _O_BINARY);

            _setmode(fd, _O_BINARY);
            /* close() must not be called because the close() would close the underlying handle. */
#endif
            auditpipe_done = true;
        }
        return (ThreadId)sysauditorPid;
    }

    /* we should never reach here */
    return 0;
}

void allow_immediate_pgaudit_restart(void)
{
    t_thrd.audit.last_pgaudit_start_time = 0;
}

/* --------------------------------
 *		pipe protocol handling
 * --------------------------------
 */
/*
 * Process data received through the sysauditor pipe.
 *
 * This routine interprets the audit pipe protocol which sends audit messages as
 * (hopefully atomic) chunks - such chunks are detected and reassembled here.
 *
 * The protocol has a header that starts with two nul bytes, then has a 16 bit
 * length, the pid of the sending process, and a flag to indicate if it is
 * the last chunk in a message. Incomplete chunks are saved until we read some
 * more, and non-final chunks are accumulated until we get the final chunk.
 *
 * All of this is to avoid 2 problems:
 * . partial messages being written to auditfiles (messes rotation), and
 * . messages from different backends being interleaved (messages garbled).
 *
 * Any non-protocol messages are written out directly. These should only come
 * from non-PostgreSQL sources, however (e.g. third party libraries writing to
 * stderr).
 *
 * auditbuffer is the data input buffer, and *bytes_in_auditbuffer is the number
 * of bytes present.  On exit, any not-yet-eaten data is left-justified in
 * auditbuffer, and *bytes_in_auditbuffer is updated.
 */
static void process_pipe_input(char* auditbuffer, int* bytes_in_auditbuffer)
{
    char* cursor = auditbuffer;
    int count = *bytes_in_auditbuffer;

    /* While we have enough for a header, process data... */
    while (count >= (int)sizeof(PipeProtoHeader)) {
        PipeProtoHeader p;
        int chunklen;
        errno_t errorno = EOK;

        /* Do we have a valid header? */
        errorno = memcpy_s(&p, sizeof(PipeProtoHeader), cursor, sizeof(PipeProtoHeader));
        securec_check(errorno, "\0", "\0");
        if (p.nuls[0] == '\0' && p.nuls[1] == '\0' && p.len > 0 && p.len <= PIPE_MAX_PAYLOAD && p.pid != 0 &&
            (p.is_last == 't' || p.is_last == 'f')) {
            List* buffer_list = NULL;
            ListCell* cell = NULL;
            save_buffer* existing_slot = NULL;
            save_buffer* free_slot = NULL;
            StringInfo str;

            chunklen = PIPE_HEADER_SIZE + p.len;

            /* Fall out of loop if we don't have the whole chunk yet */
            if (count < chunklen) {
                break;
            }

            /* Locate any existing buffer for this source pid */
            buffer_list = t_thrd.audit.buffer_lists[p.pid % NBUFFER_LISTS];
            foreach (cell, buffer_list) {
                save_buffer* buf = (save_buffer*)lfirst(cell);

                if (buf->pid == p.pid) {
                    existing_slot = buf;
                    break;
                }

                if (buf->pid == 0 && free_slot == NULL) {
                    free_slot = buf;
                }
            }

            if (p.is_last == 'f') {
                /*
                 * Save a complete non-final chunk in a per-pid buffer
                 */
                if (existing_slot != NULL) {
                    /* Add chunk to data from preceding chunks */
                    str = &(existing_slot->data);
                    appendBinaryStringInfo(str, cursor + PIPE_HEADER_SIZE, p.len);
                } else {
                    /* First chunk of message, save in a new buffer */
                    if (free_slot == NULL) {
                        /*
                         * Need a free slot, but there isn't one in the list,
                         * so create a new one and extend the list with it.
                         */
                        free_slot = (save_buffer*)palloc(sizeof(save_buffer));
                        buffer_list = lappend(buffer_list, free_slot);
                        t_thrd.audit.buffer_lists[p.pid % NBUFFER_LISTS] = buffer_list;
                    }
                    free_slot->pid = p.pid;
                    str = &(free_slot->data);
                    initStringInfo(str);
                    appendBinaryStringInfo(str, cursor + PIPE_HEADER_SIZE, p.len);
                }
            } else {
                /*
                 * Final chunk --- add it to anything saved for that pid, and
                 * either way write the whole thing out.
                 */
                if (existing_slot != NULL) {
                    str = &(existing_slot->data);
                    appendBinaryStringInfo(str, cursor + PIPE_HEADER_SIZE, p.len);
                    pgaudit_write_file(str->data, str->len);
                    /* Mark the buffer unused, and reclaim string storage */
                    existing_slot->pid = 0;
                    pfree(str->data);
                } else {
                    /* The whole message was one chunk, evidently. */
                    pgaudit_write_file(cursor + PIPE_HEADER_SIZE, p.len);
                }
            }

            /* Finished processing this chunk */
            cursor += chunklen;
            count -= chunklen;
        } else {
            /* Process non-protocol data */

            /*
             * Look for the start of a protocol header.  If found, dump data
             * up to there and repeat the loop.  Otherwise, dump it all and
             * fall out of the loop.  (Note: we want to dump it all if at all
             * possible, so as to avoid dividing non-protocol messages across
             * auditfiles.  We expect that in many scenarios, a non-protocol
             * message will arrive all in one read(), and we want to respect
             * the read() boundary if possible.)
             */
            for (chunklen = 1; chunklen < count; chunklen++) {
                if (cursor[chunklen] == '\0') {
                    break;
                }
            }
            /* fall back on the stderr audit as the destination */
            pgaudit_write_file(cursor, chunklen);
            cursor += chunklen;
            count -= chunklen;
        }
    }

    /* We don't have a full chunk, so left-align what remains in the buffer */
    if (count > 0 && cursor != auditbuffer) {
        errno_t errorno = EOK;
        errorno = memmove_s(auditbuffer, READ_BUF_SIZE, cursor, count);
        securec_check(errorno, "\0", "\0");
    }
    *bytes_in_auditbuffer = count;
}

/*
 * Force out any buffered data
 *
 * This is currently used only at sysauditor shutdown, but could perhaps be
 * useful at other times, so it is careful to leave things in a clean state.
 */
static void flush_pipe_input(char* auditbuffer, int* bytes_in_auditbuffer)
{
    int i;

    /* Dump any incomplete protocol messages */
    for (i = 0; i < NBUFFER_LISTS; i++) {
        List* list = t_thrd.audit.buffer_lists[i];
        ListCell* cell = NULL;

        foreach (cell, list) {
            save_buffer* buf = (save_buffer*)lfirst(cell);

            if (buf->pid != 0) {
                StringInfo str = &(buf->data);

                pgaudit_write_file(str->data, str->len);
                /* Mark the buffer unused, and reclaim string storage */
                buf->pid = 0;
                pfree(str->data);
            }
        }
    }

    /*
     * Force out any remaining pipe data as-is; we don't bother trying to
     * remove any protocol headers that may exist in it.
     */
    if (*bytes_in_auditbuffer > 0) {
        pgaudit_write_file(auditbuffer, *bytes_in_auditbuffer);
    }
    *bytes_in_auditbuffer = 0;
}

/* --------------------------------
 *		auditfile routines
 * --------------------------------
 */
/*
 * Write data to the currently open auditfile
 *
 * This is exported so that elog.c can call it when t_thrd.audit.am_sysauditor is true.
 * This allows the sysauditor process to record elog messages of its own,
 * even though its stderr does not point at the sysaudit pipe.
 */
static void pgaudit_write_file(char* buffer, int count)
{
    int rc;
    pg_time_t curtime;
    errno_t errorno = EOK;

    if (buffer == NULL || t_thrd.audit.sysauditFile == NULL)
        return;

    curtime = time(NULL);
    errorno = memcpy_s(
        buffer + offsetof(AuditMsgHdr, time), READ_BUF_SIZE - offsetof(AuditMsgHdr, time), &curtime, sizeof(pg_time_t));
    securec_check(errorno, "\0", "\0");
    errorno = memcpy_s(
        buffer + offsetof(AuditMsgHdr, size), READ_BUF_SIZE - offsetof(AuditMsgHdr, size), &count, sizeof(uint32));
    securec_check(errorno, "\0", "\0");

    if (t_thrd.audit.audit_indextbl) {
        /* check to see whether system time changed. */
        if (t_thrd.audit.audit_indextbl->last_audit_time > curtime) {
            AuditIndexItem* item = NULL;
            item = t_thrd.audit.audit_indextbl->data + t_thrd.audit.audit_indextbl->curidx;
            if (item->ctime > 0)
                item->ctime *= -1;
            t_thrd.audit.audit_indextbl->last_audit_time = curtime;
            pgaudit_update_indexfile(PG_BINARY_W, true);

            audit_report(AUDIT_INTERNAL_EVENT, AUDIT_OK, "time", "system time changed.");
        }
        t_thrd.audit.audit_indextbl->last_audit_time = curtime;
    }

    errno = 0;
retry1:
    rc = fwrite(buffer, 1, count, t_thrd.audit.sysauditFile);

    if (rc != count) {
        /*
         * If no disk space, we will retry, and we can not report a log as
         * there is not space to write.
         */
        if (errno == ENOSPC) {
            pg_usleep(1000000);
            goto retry1;
        }
        ereport(ERROR, (errcode_for_file_access(), errmsg("could not write to audit file: %m")));
    }

    /*
     * The contents of the audit logfile haven't newline which is difference from syslog, so
     * LBF_MODE set by setvbuf can't make sure the write buffer be fflushed into the logfile
     * immediately. We need call the fflush function here by ourself to make sure this.
     * NOTICE : in some version of glibc, ftell have the flush feature built-in but it's not
     * standard practice to rely ftell to flush, so fflush here is the most assured.
     */
    (void)fflush(t_thrd.audit.sysauditFile);
}

#ifdef WIN32

/*
 * Worker thread to transfer data from the pipe to the current auditfile.
 *
 * We need this because on Windows, WaitforMultipleObjects does not work on
 * unnamed pipes: it always reports "signaled", so the blocking ReadFile won't
 * allow for SIGHUP; and select is for sockets only.
 */
static unsigned int __stdcall pipeThread(void* arg)
{
    char auditbuffer[READ_BUF_SIZE];
    int bytes_in_auditbuffer = 0;

    for (;;) {
        DWORD bytesRead;
        BOOL result = false;

        result = ReadFile(sysauditPipe[0],
            auditbuffer + bytes_in_auditbuffer,
            sizeof(auditbuffer) - bytes_in_auditbuffer,
            &bytesRead,
            0);

        /*
         * Enter critical section before doing anything that might touch
         * global state shared by the main thread. Anything that uses
         * palloc()/pfree() in particular are not safe outside the critical
         * section.
         */
        EnterCriticalSection(&sysauditorSection);
        if (!result) {
            DWORD error = GetLastError();

            if (error == ERROR_HANDLE_EOF || error == ERROR_BROKEN_PIPE)
                break;
            _dosmaperr(error);
            ereport(LOG, (errcode_for_file_access(), errmsg("could not read from auditor pipe: %m")));
        } else if (bytesRead > 0) {
            bytes_in_auditbuffer += bytesRead;
            process_pipe_input(auditbuffer, &bytes_in_auditbuffer);
        }
        LeaveCriticalSection(&sysauditorSection);
    }

    /* We exit the above loop only upon detecting pipe EOF */
    t_thrd.audit.pipe_eof_seen = true;

    /* if there's any data left then force it out now */
    flush_pipe_input(auditbuffer, &bytes_in_auditbuffer);

    /* set the latch to waken the main thread, which will quit */
    SetLatch(&t_thrd.audit.sysAuditorLatch);

    LeaveCriticalSection(&sysauditorSection);
    _endthread();
    return 0;
}
#endif /* WIN32 */

/*
 * Brief		: initialize the audit file.
 * Description	:
 */
static void auditfile_init(void)
{
    if (t_thrd.audit.sysauditFile)
        return;

    /*
     * The initial auditfile is created right in the postmaster, to verify that
     * the Audit_directory is writable.
     */
    t_thrd.audit.sysauditFile = auditfile_open(time(NULL), "a", false);
    if (ftell(t_thrd.audit.sysauditFile) == 0)
        audit_report(AUDIT_INTERNAL_EVENT, AUDIT_OK, "file", "create a new audit file");
}

/*
 * Brief		: open a new audit file.
 * Description	:
 * 		Open a new auditfile with proper permissions and buffering options.
 *
 * 		If allow_errors is true, we just audit any open failure and return NULL
 * 		(with errno still correct for the fopen failure).
 * 		Otherwise, errors are treated as fatal.
 */
static FILE* auditfile_open(pg_time_t timestamp, const char* mode, bool allow_errors)
{
    FILE* fh = NULL;
    char* filename = NULL;
    uint32 fnum = 0;
    AuditIndexItem* item = NULL;
    struct stat st;
    bool exist = false;
    if (t_thrd.audit.audit_indextbl) {
        item = t_thrd.audit.audit_indextbl->data + t_thrd.audit.audit_indextbl->curidx;
        fnum = item->filenum;
    }
    filename = (char*)palloc(MAXPGPATH);
    int rc = snprintf_s(
        filename, MAXPGPATH, MAXPGPATH - 1, pgaudit_filename, g_instance.attr.attr_security.Audit_directory, fnum);
    securec_check_intval(rc, , NULL);

    /*
     * Note we do not let pgaudit_filemode disable IWUSR, since we certainly want
     * to be able to write the files ourselves.
     */
    if (stat(filename, &st) == 0)
        exist = true;
    fh = fopen(filename, mode);

    if (fh != NULL) {
        setvbuf(fh, NULL, LBF_MODE, 0);

#ifdef WIN32
        /* use CRLF line endings on Windows */
        _setmode(_fileno(fh), _O_BINARY);
#endif
        if (t_thrd.audit.audit_indextbl) {
            if (!exist) {
                item = t_thrd.audit.audit_indextbl->data + t_thrd.audit.audit_indextbl->curidx;
                item->ctime = timestamp;
            }
            t_thrd.audit.audit_indextbl->count = AUDIT_COUNT(t_thrd.audit.audit_indextbl);
            pgaudit_update_indexfile(PG_BINARY_W, true);
        }
    } else {
        int save_errno = errno;

        ereport(allow_errors ? LOG : FATAL,
            (errcode_for_file_access(), errmsg("could not open audit file \"%s\": %m", filename)));
        errno = save_errno;
    }

    if (!exist) {
        if (chmod(filename, S_IWUSR | (mode_t)pgaudit_filemode) < 0) {
            int save_errno = errno;

            ereport(allow_errors ? LOG : FATAL,
                (errcode_for_file_access(), errmsg("could not chmod audit file \"%s\": %m", filename)));
            errno = save_errno;
        }
    }

    pfree(filename);
    return fh;
}

/*
 * Brief		: close the audit file.
 * Description	:
 */
static void auditfile_close(void)
{
    AuditIndexItem* item = NULL;
    uint32 fnum = 0;

    if (t_thrd.audit.sysauditFile == NULL)
        return;

    if (t_thrd.audit.audit_indextbl != NULL) {
        item = t_thrd.audit.audit_indextbl->data + t_thrd.audit.audit_indextbl->curidx;
        item->filesize = ftell(t_thrd.audit.sysauditFile);
        fnum = item->filenum + 1;

        t_thrd.audit.pgaudit_totalspace += item->filesize;

        /* switch to next audit file */
        t_thrd.audit.audit_indextbl->curidx =
            (t_thrd.audit.audit_indextbl->curidx + 1) % t_thrd.audit.audit_indextbl->maxnum;
        item = t_thrd.audit.audit_indextbl->data + t_thrd.audit.audit_indextbl->curidx;
        item->filenum = fnum;
    }
    fclose(t_thrd.audit.sysauditFile);
    t_thrd.audit.sysauditFile = NULL;
}

/*
 * Brief		: perform audit file rotation
 * Description	:
 */
static void auditfile_rotate(bool time_based_rotation, bool size_based_rotation)
{
    pg_time_t fntime;
    FILE* fh = NULL;

    t_thrd.audit.rotation_requested = false;

    /*
     * When doing a time-based rotation, invent the new auditfile name based on
     * the planned rotation time, not current time, to avoid "slippage" in the
     * file name when we don't do the rotation immediately.
     */
    if (time_based_rotation)
        fntime = t_thrd.audit.next_rotation_time;
    else
        fntime = time(NULL);

    if (time_based_rotation || size_based_rotation) {
        auditfile_close();

        fh = auditfile_open(fntime, "a", true);
        if (fh == NULL) {
            /*
             * ENFILE/EMFILE are not too surprising on a busy system; just
             * keep using the old file till we manage to get a new one.
             * Otherwise, assume something's wrong with Audit_directory and stop
             * trying to create files.
             */
            if (errno != ENFILE && errno != EMFILE) {
                ereport(LOG, (errmsg("disabling automatic rotation (use SIGHUP to re-enable)")));
                t_thrd.audit.rotation_disabled = true;
            }

            return;
        }

        t_thrd.audit.sysauditFile = fh;
        audit_report(AUDIT_INTERNAL_EVENT, AUDIT_OK, "file", "create a new audit file");
    }

    set_next_rotation_time();
}

/*
 * Determine the next planned rotation time, and store in t_thrd.audit.next_rotation_time.
 */
static void set_next_rotation_time(void)
{
    pg_time_t now;
    struct pg_tm* tm = NULL;
    int rotinterval;

    /* nothing to do if time-based rotation is disabled */
    if (u_sess->attr.attr_security.Audit_RotationAge <= 0)
        return;

    /*
     * The requirements here are to choose the next time > now that is a
     * "multiple" of the audit rotation interval.  "Multiple" can be interpreted
     * fairly loosely.	In this version we align to audit_timezone rather than
     * GMT.
     */
    rotinterval = u_sess->attr.attr_security.Audit_RotationAge * SECS_PER_MINUTE; /* convert to seconds */
    now = (pg_time_t)time(NULL);
    tm = pg_localtime(&now, log_timezone);
    now += tm->tm_gmtoff;
    now -= now % rotinterval;
    now += rotinterval;
    now -= tm->tm_gmtoff;
    t_thrd.audit.next_rotation_time = now;
}

/*
 * pgaudit_cleanup
 *
 * Check audit data cleanup condition and delete old audit file then returns
 */
static void pgaudit_cleanup(void)
{
    uint32 index = 0;
    AuditIndexItem* item = NULL;
    uint64 filesize = 0;
    pg_time_t remain_time = (int64)u_sess->attr.attr_security.Audit_RemainAge * SECS_PER_DAY;  // how many seconds

    if (t_thrd.audit.audit_indextbl == NULL)
        return;

    if (t_thrd.audit.sysauditFile != NULL)
        filesize = ftell(t_thrd.audit.sysauditFile);

    index = t_thrd.audit.audit_indextbl->begidx;
    while (
        t_thrd.audit.pgaudit_totalspace + filesize >= (uint64)(u_sess->attr.attr_security.Audit_SpaceLimit * 1024L) ||
        t_thrd.audit.audit_indextbl->count > (uint32)u_sess->attr.attr_security.Audit_RemainThreshold) {
        errno_t errorno = EOK;
        struct stat statbuf;

        item = t_thrd.audit.audit_indextbl->data + index;

        /* to check how long the audit file is remained:
         * a. it must be time-based policy and the specified value is valid;
         * b. the remained time of oldest audit file is beyond the specified value;
         * c. the total size is not beyond the maximum space size.
         */
        if (t_thrd.audit.audit_indextbl->count <= (uint32)u_sess->attr.attr_security.Audit_RemainThreshold &&
            u_sess->attr.attr_security.Audit_CleanupPolicy == 0 && remain_time &&
            (t_thrd.audit.pgaudit_totalspace + filesize <= SPACE_MAXIMUM_SIZE)) {
            if ((uint64)(t_thrd.audit.pgaudit_totalspace + filesize -
                         u_sess->attr.attr_security.Audit_SpaceLimit * 1024L) >= t_thrd.audit.space_beyond_size) {
                ereport(WARNING,
                    (errmsg("audit file total space(%lld B) exceed guc parameter(audit_space_limit: %d KB) about %d MB",
                        (long long int)(t_thrd.audit.pgaudit_totalspace + filesize),
                        u_sess->attr.attr_security.Audit_SpaceLimit,
                        (int)(t_thrd.audit.space_beyond_size / (1024 * 1024)))));

                t_thrd.audit.space_beyond_size += SPACE_INTERVAL_SIZE;
            }

            /* get the next item */
            AuditIndexItem* next =
                t_thrd.audit.audit_indextbl->data + (index + 1) % t_thrd.audit.audit_indextbl->maxnum;

            if (remain_time >= (t_thrd.audit.audit_indextbl->last_audit_time - item->ctime) ||
                (next && (remain_time > (t_thrd.audit.audit_indextbl->last_audit_time - next->ctime))))
                break;
        }

        int rc = snprintf_s(t_thrd.audit.pgaudit_filepath,
            MAXPGPATH,
            MAXPGPATH - 1,
            pgaudit_filename,
            g_instance.attr.attr_security.Audit_directory,
            item->filenum);
        securec_check_intval(rc, , );

        if (stat(t_thrd.audit.pgaudit_filepath, &statbuf) == 0 && unlink(t_thrd.audit.pgaudit_filepath) < 0) {
            ereport(WARNING, (errmsg("could not remove audit file: %m")));
            break;
        }

        rc = snprintf_truncated_s(
            t_thrd.audit.pgaudit_filepath, MAXPGPATH, "remove an audit file(number: %u)", item->filenum);
        securec_check_ss(rc, "\0", "\0");

        if ((u_sess->attr.attr_security.Audit_CleanupPolicy || remain_time == 0) &&
            (t_thrd.audit.pgaudit_totalspace + filesize >= (uint64)u_sess->attr.attr_security.Audit_SpaceLimit * 1024L))
#ifdef HAVE_LONG_LONG_INT
            ereport(WARNING,
                (errmsg("audit file total space(%lld B) exceed guc parameter(audit_space_limit: %d KB)",
                    (long long int)(t_thrd.audit.pgaudit_totalspace + filesize),
                    u_sess->attr.attr_security.Audit_SpaceLimit)));
#else
            ereport(WARNING,
                (errmsg("audit file total space(%ld B) exceed guc parameter(audit_space_limit: %d KB)",
                    (t_thrd.audit.pgaudit_totalspace + filesize),
                    u_sess->attr.attr_security.Audit_SpaceLimit)));
#endif
        else if (u_sess->attr.attr_security.Audit_CleanupPolicy == 0 && remain_time &&
                 (t_thrd.audit.pgaudit_totalspace + filesize >=
                     (uint64)u_sess->attr.attr_security.Audit_SpaceLimit * 1024L))
#ifdef HAVE_LONG_LONG_INT
            ereport(WARNING,
                (errmsg("Based on time-priority policy, the oldest audit file is beyond %d days or "
                        "audit file total space(%lld B) exceed guc parameter(audit_space_limit: %d KB)",
                    u_sess->attr.attr_security.Audit_RemainAge,
                    (long long int)(t_thrd.audit.pgaudit_totalspace + filesize),
                    u_sess->attr.attr_security.Audit_SpaceLimit)));
#else
            ereport(WARNING,
                (errmsg("Based on time-priority policy, the oldest audit file is beyond %d days or "
                        "audit file total space(%ld B) exceed guc parameter(audit_space_limit: %d KB)",
                    u_sess->attr.attr_security.Audit_RemainAge,
                    (t_thrd.audit.pgaudit_totalspace + filesize),
                    u_sess->attr.attr_security.Audit_SpaceLimit)));
#endif

        if (t_thrd.audit.audit_indextbl->count > (uint32)u_sess->attr.attr_security.Audit_RemainThreshold)
            ereport(WARNING,
                (errmsg("audit file total count(%u) exceed guc parameter(audit_file_remain_threshold: %d)",
                    t_thrd.audit.audit_indextbl->count,
                    u_sess->attr.attr_security.Audit_RemainThreshold)));
        ereport(WARNING, (errmsg("%s", t_thrd.audit.pgaudit_filepath)));

        t_thrd.audit.pgaudit_totalspace -= item->filesize;
        if (t_thrd.audit.audit_indextbl->count > 0)
            t_thrd.audit.audit_indextbl->count--;
        t_thrd.audit.audit_indextbl->begidx = (index + 1) % t_thrd.audit.audit_indextbl->maxnum;
        errorno = memset_s(item, sizeof(AuditIndexItem), 0, sizeof(AuditIndexItem));
        securec_check(errorno, "\0", "\0");

        pgaudit_update_indexfile(PG_BINARY_W, true);

        audit_report(AUDIT_INTERNAL_EVENT, AUDIT_OK, "file", t_thrd.audit.pgaudit_filepath);

        if (index == t_thrd.audit.audit_indextbl->curidx)
            break;

        index = t_thrd.audit.audit_indextbl->begidx;
    }
}

/* --------------------------------
 *		signal handler routines
 * --------------------------------
 */
/* SIGQUIT signal handler for auditor process */
static void pgaudit_exit(SIGNAL_ARGS)
{
    int save_errno = errno;

    t_thrd.audit.need_exit = true;
    SetLatch(&t_thrd.audit.sysAuditorLatch);

    errno = save_errno;
}

/* SIGHUP: set flag to reload config file */
static void sigHupHandler(SIGNAL_ARGS)
{
    int save_errno = errno;

    t_thrd.audit.got_SIGHUP = true;
    SetLatch(&t_thrd.audit.sysAuditorLatch);

    errno = save_errno;
}

/* SIGUSR1: set flag to rotate auditfile */
static void sigUsr1Handler(SIGNAL_ARGS)
{
    int save_errno = errno;

    t_thrd.audit.rotation_requested = true;
    SetLatch(&t_thrd.audit.sysAuditorLatch);

    errno = save_errno;
}

/*
 * Send data to the syslogger using the chunked protocol
 *
 * Note: when there are multiple backends writing into the syslogger pipe,
 * it's critical that each write go into the pipe indivisibly, and not
 * get interleaved with data from other processes.  Fortunately, the POSIX
 * spec requires that writes to pipes be atomic so long as they are not
 * more than PIPE_BUF bytes long.  So we divide long messages into chunks
 * that are no more than that length, and send one chunk per write() call.
 * The collector process knows how to reassemble the chunks.
 *
 * Because of the atomic write requirement, there are only two possible
 * results from write() here: -1 for failure, or the requested number of
 * bytes.  There is not really anything we can do about a failure; retry would
 * probably be an infinite loop, and we can't even report the error usefully.
 * (There is noplace else we could send it!)  So we might as well just ignore
 * the result from write().  However, on some platforms you get a compiler
 * warning from ignoring write()'s result, so do a little dance with casting
 * rc to void to shut up the compiler.
 */
static void write_pipe_chunks(char* data, int len)
{
    PipeProtoChunk p;
    errno_t errorno = EOK;
#ifndef WIN32
    int rc;
#else
    DWORD bytesWritten;
    BOOL result = false;
#endif

    Assert(len > 0);

    p.proto.nuls[0] = p.proto.nuls[1] = '\0';
    p.proto.pid = t_thrd.proc_cxt.MyProcPid;

    /* write all but the last chunk */
    while (len > PIPE_MAX_PAYLOAD) {
        p.proto.is_last = 'f';
        p.proto.len = PIPE_MAX_PAYLOAD;
        errorno = memcpy_s(p.proto.data, PIPE_MAX_PAYLOAD, data, PIPE_MAX_PAYLOAD);
        securec_check(errorno, "\0", "\0");
#ifndef WIN32
        rc = write(sysauditPipe[1], &p, PIPE_HEADER_SIZE + PIPE_MAX_PAYLOAD);
        (void)rc;
#else
        result = WriteFile(sysauditPipe[1], &p, PIPE_HEADER_SIZE + PIPE_MAX_PAYLOAD, &bytesWritten, 0);
#endif
        data += PIPE_MAX_PAYLOAD;
        len -= PIPE_MAX_PAYLOAD;
    }

    /* write the last chunk */
    p.proto.is_last = 't';
    p.proto.len = len;
    errorno = memcpy_s(p.proto.data, PIPE_MAX_PAYLOAD, data, len);
    securec_check(errorno, "\0", "\0");
#ifndef WIN32
    rc = write(sysauditPipe[1], &p, PIPE_HEADER_SIZE + len);
    (void)rc;
#else
    result = WriteFile(sysauditPipe[1], &p, PIPE_HEADER_SIZE + len, &bytesWritten, 0);
#endif
}

/*
 * Brief		: append a string field to a streamed data
 * Description	:
 */
static void appendStringField(StringInfo str, const char* s)
{
    int size = 0;

    if (str == NULL)
        return;

    if (s == NULL)
        appendBinaryStringInfo(str, (char*)&size, sizeof(int));
    else {
        size = strlen(s) + 1;
        appendBinaryStringInfo(str, (char*)&size, sizeof(int));
        appendBinaryStringInfo(str, s, size);
    }
}

/*
 * Brief		: report audit info to the system auditor
 * Description	: called by all backends
 */
void audit_report(AuditType type, AuditResult result, const char* object_name, const char* detail_info)
{
    StringInfoData buf;
    AuditData adata;
    int size = 0;
    char threadid[MAXNUMLEN * 4] = {0};
    char userid[MAXNUMLEN] = {0};
    char localport[MAXNUMLEN] = {0};
    char remoteport[MAXNUMLEN] = {0};


#ifndef ENABLE_MULTIPLE_NODES
    /* After the standby read function is added, the standby node needs to be audited. */
    if (!u_sess->attr.attr_security.Audit_enabled ||
        (PGSharedMemoryAttached() && t_thrd.postmaster_cxt.HaShmData &&
            (t_thrd.postmaster_cxt.HaShmData->current_mode == PENDING_MODE)))
        return;
#else
    /*
     * check whether POSTMASTER is running in standby mode.
     * If in standby mode, then quit the audit_report function.
     */
    if (!u_sess->attr.attr_security.Audit_enabled ||
        (PGSharedMemoryAttached() && t_thrd.postmaster_cxt.HaShmData &&
            (STANDBY_MODE == t_thrd.postmaster_cxt.HaShmData->current_mode ||
                PENDING_MODE == t_thrd.postmaster_cxt.HaShmData->current_mode)))
        return;
#endif

    /* check the audit type to decide whether to report it */
    switch (type) {
        case AUDIT_LOGIN_SUCCESS:
            if (check_audit_login(type))
                break;
            return;
        case AUDIT_LOGIN_FAILED:
            if (check_audit_login(type))
                break;
            return;
        case AUDIT_USER_LOGOUT:
            if ((unsigned int)u_sess->attr.attr_security.Audit_Session & (1 << SESSION_LOGOUT))
                break;
            else
                return;

        case AUDIT_SYSTEM_START:
        case AUDIT_SYSTEM_STOP:
        case AUDIT_SYSTEM_RECOVER:
        case AUDIT_SYSTEM_SWITCH:
            if (u_sess->attr.attr_security.Audit_ServerAction)
                break;
            else
                return;

        case AUDIT_LOCK_USER:
        case AUDIT_UNLOCK_USER:
            if (u_sess->attr.attr_security.Audit_LockUser)
                break;
            else
                return;

        case AUDIT_GRANT_ROLE:
        case AUDIT_REVOKE_ROLE:
            if (u_sess->attr.attr_security.Audit_PrivilegeAdmin)
                break;
            else
                return;

        case AUDIT_USER_VIOLATION:
            if (u_sess->attr.attr_security.Audit_UserViolation)
                break;
            else
                return;

        case AUDIT_DDL_DATABASE:
            if (CHECK_AUDIT_DDL(DDL_DATABASE))
                break;
            else
                return;
        case AUDIT_DDL_DIRECTORY:
            if (CHECK_AUDIT_DDL(DDL_DIRECTORY))
                break;
            else
                return;
        case AUDIT_DDL_TABLESPACE:
            if (CHECK_AUDIT_DDL(DDL_TABLESPACE))
                break;
            else
                return;
        case AUDIT_DDL_SCHEMA:
            if (CHECK_AUDIT_DDL(DDL_SCHEMA))
                break;
            else
                return;
        case AUDIT_DDL_USER:
            if (CHECK_AUDIT_DDL(DDL_USER))
                break;
            else
                return;
        case AUDIT_DDL_TABLE:
            if (CHECK_AUDIT_DDL(DDL_TABLE))
                break;
            else
                return;
        case AUDIT_DDL_INDEX:
            if (CHECK_AUDIT_DDL(DDL_INDEX))
                break;
            else
                return;
        case AUDIT_DDL_VIEW:
            if (CHECK_AUDIT_DDL(DDL_VIEW))
                break;
            else
                return;
        case AUDIT_DDL_TRIGGER:
            if (CHECK_AUDIT_DDL(DDL_TRIGGER))
                break;
            else
                return;
        case AUDIT_DDL_FUNCTION:
            if (CHECK_AUDIT_DDL(DDL_FUNCTION))
                break;
            else
                return;
        case AUDIT_DDL_RESOURCEPOOL:
            if (CHECK_AUDIT_DDL(DDL_RESOURCEPOOL))
                break;
            else
                return;
        case AUDIT_DDL_WORKLOAD:
            if (CHECK_AUDIT_DDL(DDL_WORKLOAD))
                break;
            else
                return;
        case AUDIT_DDL_SERVERFORHADOOP:
            if (CHECK_AUDIT_DDL(DDL_SERVERFORHADOOP))
                break;
            else
                return;
        case AUDIT_DDL_DATASOURCE:
            if (CHECK_AUDIT_DDL(DDL_DATASOURCE))
                break;
            else
                return;
        case AUDIT_DDL_NODEGROUP:
            if (CHECK_AUDIT_DDL(DDL_NODEGROUP))
                break;
            else
                return;
        case AUDIT_DDL_ROWLEVELSECURITY:
            if (CHECK_AUDIT_DDL(DDL_ROWLEVELSECURITY))
                break;
            else
                return;
        case AUDIT_DDL_SYNONYM:
            if (CHECK_AUDIT_DDL(DDL_SYNONYM))
                break;
            else
                return;
        case AUDIT_DDL_TYPE:
            if (CHECK_AUDIT_DDL(DDL_TYPE))
                break;
            else
                return;
        case AUDIT_DDL_TEXTSEARCH:
            if (CHECK_AUDIT_DDL(DDL_TEXTSEARCH))
                break;
            else
                return;
        case AUDIT_DML_ACTION:
            if (u_sess->attr.attr_security.Audit_DML)
                break;
            else
                return;
        case AUDIT_DML_ACTION_SELECT:
            if (u_sess->attr.attr_security.Audit_DML_SELECT)
                break;
            else
                return;

        case AUDIT_FUNCTION_EXEC:
            if (u_sess->attr.attr_security.Audit_Exec)
                break;
            else
                return;

        case AUDIT_INTERNAL_EVENT:
            break;
        case AUDIT_COPY_TO:
            if (u_sess->attr.attr_security.Audit_Copy)
                break;
            else
                return;
        case AUDIT_COPY_FROM:
            if (u_sess->attr.attr_security.Audit_Copy)
                break;
            else
                return;
        case AUDIT_SET_PARAMETER:
            if (u_sess->attr.attr_security.Audit_Set)
                break;
            else
                return;
        case AUDIT_UNKNOWN_TYPE:
        default:
            ereport(WARNING, (errmsg("unknown audit type, discard it.")));
            return;
    }

    adata.header.signature[0] = 'A';
    adata.header.signature[1] = 'U';
    adata.header.version = 0;
    adata.header.fields = PGAUDIT_QUERY_COLS;
    adata.header.flags = AUDIT_TUPLE_NORMAL;
    adata.header.time = 0;
    adata.header.size = 0;
    adata.type = type;
    adata.result = result;

    initStringInfo(&buf);

    appendBinaryStringInfo(&buf, (char*)&adata, AUDIT_HEADER_SIZE);

    if (u_sess->proc_cxt.MyProcPort != NULL) {
        errno_t errorno = EOK;

        /* append user name information */
        const char* username = NULL;
        if (u_sess->misc_cxt.CurrentUserName != NULL) {
            username = u_sess->misc_cxt.CurrentUserName;
        } else {
            username = u_sess->proc_cxt.MyProcPort->user_name;
        }
        
        /* append user id information */
        Oid useroid = GetCurrentUserId();
        if (username != NULL && useroid == 0) {
            useroid = get_role_oid(username, true);
        }
        errorno = snprintf_s(userid, MAXNUMLEN, MAXNUMLEN - 1, "%d", useroid);
        securec_check_ss(errorno, "", "");
        appendStringField(&buf, userid);

        if (username == NULL || *username == '\0') {
            username = _("[unknown]");
        }
        appendStringField(&buf, username);

        /* append dbname, appname and ip information */
        const char* dbname = u_sess->proc_cxt.MyProcPort->database_name;
        const char* appname = u_sess->attr.attr_common.application_name;
        const char* remotehost = u_sess->proc_cxt.MyProcPort->remote_host;
        errorno = snprintf_s(threadid,
            MAXNUMLEN * 4,
            MAXNUMLEN * 4 - 1,
            "%lu@%ld",
            t_thrd.proc_cxt.MyProcPid,
            t_thrd.audit.user_login_time);
        securec_check_ss(errorno, "\0", "\0");

        int portNum;
        if (IsHAPort(u_sess->proc_cxt.MyProcPort)) {
            portNum = g_instance.attr.attr_network.PoolerPort;
        } else {
            portNum = g_instance.attr.attr_network.PostPortNumber;
        }

        errorno = snprintf_s(localport, MAXNUMLEN, MAXNUMLEN - 1, "%d", portNum);
        securec_check_ss(errorno, "\0", "\0");

        errorno = snprintf_s(remoteport, MAXNUMLEN, MAXNUMLEN - 1, "%s", u_sess->proc_cxt.MyProcPort->remote_port);
        securec_check_ss(errorno, "\0", "\0");

        /* append database name */
        if (dbname == NULL || *dbname == '\0')
            dbname = _("[unknown]");
        appendStringField(&buf, dbname);

        /* append client_info */
        if (appname == NULL || *appname == '\0')
            appname = _("[unknown]");
        if (remotehost == NULL || *remotehost == '\0')
            remotehost = _("[unknown]");
        size = strlen(appname) + strlen(remotehost) + 2;
        appendBinaryStringInfo(&buf, (char*)&size, sizeof(int));
        appendStringInfo(&buf, "%s@%s", appname, remotehost);
        appendStringInfoChar(&buf, 0);
    } else {
        int i = 0;
        size = 0;
        /* set userid, username, dbname, client_conninfo to null */
        for (i = 0; i < 4; i++)
            appendBinaryStringInfo(&buf, (char*)&size, sizeof(int));
    }

    appendStringField(&buf, object_name);
    appendStringField(&buf, detail_info);
    appendStringField(&buf, g_instance.attr.attr_common.PGXCNodeName);
    if (threadid[0] != '\0')
        appendStringField(&buf, threadid);
    else
        appendStringField(&buf, NULL);

    if (localport[0] != '\0')
        appendStringField(&buf, localport);
    else
        appendStringField(&buf, NULL);

    if (remoteport[0] != '\0')
        appendStringField(&buf, remoteport);
    else
        appendStringField(&buf, NULL);

    /*
     * Use the chunking protocol if we know the syslogger should be
     * catching stderr output, and we are not ourselves the syslogger.
     * Otherwise, just do a vanilla write to stderr.
     */
    if (auditpipe_done && t_thrd.role != AUDIT)
        write_pipe_chunks(buf.data, buf.len);
    /* If in the syslogger process, try to write messages direct to file */
    else if (t_thrd.role == AUDIT)
        pgaudit_write_file(buf.data, buf.len);
    else {
        /* report audit data to syslogger. */
        if (detail_info != NULL)
            ereport(LOG, (errmsg("discard audit data: %s", detail_info)));
    }

    pfree(buf.data);
}

static void pgaudit_close_file(FILE* fp, const char* file)
{
    if (NULL == file || NULL == fp) {
        return;
    }

    if (ferror(fp)) {
        ereport(LOG, (errcode_for_file_access(), errmsg("could not write audit file \"%s\": %m", file)));
        if (FreeFile(fp) < 0) {
            ereport(LOG, (errcode_for_file_access(), errmsg("could not close audit file \"%s\": %m", file)));
        }
    } else if (FreeFile(fp) < 0) {
        ereport(LOG, (errcode_for_file_access(), errmsg("could not close audit file \"%s\": %m", file)));
    }
}

/*
 * Brief		: read the index table into memory from file.
 * Description	:
 */
static void pgaudit_read_indexfile(const char* audit_directory)
{
    FILE* fp = NULL;
    struct stat statbuf;
    char tblfile_path[MAXPGPATH] = {0};
    size_t nread = 0;
    AuditIndexTable indextbl;

    if (t_thrd.audit.audit_indextbl != NULL) {
        pfree(t_thrd.audit.audit_indextbl);
        t_thrd.audit.audit_indextbl = NULL;
    }

    int rc = snprintf_s(tblfile_path, MAXPGPATH, MAXPGPATH - 1, "%s/%s", audit_directory, audit_indextbl_file);
    securec_check_intval(rc, , );

    /* Check whether the map file is exist. */
    if (stat(tblfile_path, &statbuf) == 0) {
        /* Open the audit index table file to write out the current values. */
        fp = AllocateFile(tblfile_path, PG_BINARY_R);
        if (NULL == fp) {
            ereport(LOG,
                (errcode_for_file_access(), errmsg("could not open audit index table file \"%s\": %m", tblfile_path)));
            return;
        }

        /* read the audit index table header first */
        nread = fread(&indextbl, indextbl_header_size, 1, fp);

        if (1 == nread) {
            errno_t errorno = EOK;

            /* read the whole audit index table */
            t_thrd.audit.audit_indextbl =
                (AuditIndexTable*)palloc0(indextbl.maxnum * sizeof(AuditIndexItem) + indextbl_header_size);
            errorno = memcpy_s(t_thrd.audit.audit_indextbl,
                indextbl.maxnum * sizeof(AuditIndexItem) + indextbl_header_size,
                &indextbl,
                indextbl_header_size);
            securec_check(errorno, "\0", "\0");

            nread = fread(t_thrd.audit.audit_indextbl->data, sizeof(AuditIndexItem), indextbl.maxnum, fp);
            if (nread != indextbl.maxnum) {
                ereport(WARNING,
                    (errcode_for_file_access(), errmsg("could not read audit index file \"%s\": %m", tblfile_path)));
            }
        }

        pgaudit_close_file(fp, tblfile_path);
    }
}

/*
 * Brief		: write the index table into file from memory.
 * Description	:
 */
static void pgaudit_update_indexfile(const char* mode, bool allow_errors)
{
    FILE* fp = NULL;
    char tblfile_path[MAXPGPATH] = {0};
    size_t nwritten = 0;
    size_t count = 0;

    int rc = snprintf_s(tblfile_path,
        MAXPGPATH,
        MAXPGPATH - 1,
        "%s/%s",
        g_instance.attr.attr_security.Audit_directory,
        audit_indextbl_file);
    securec_check_intval(rc, , );

    /* Open the audit index table file to write out the current values. */
    fp = AllocateFile(tblfile_path, mode);
    if (NULL == fp) {
        ereport(allow_errors ? LOG : FATAL,
            (errcode_for_file_access(), errmsg("could not open audit index table file \"%s\": %m", tblfile_path)));
        return;
    }

    if (t_thrd.audit.audit_indextbl != NULL) {
        count = t_thrd.audit.audit_indextbl->maxnum * sizeof(AuditIndexItem) + indextbl_header_size;
        nwritten = fwrite(t_thrd.audit.audit_indextbl, 1, count, fp);
        if (nwritten != count)
            ereport(allow_errors ? LOG : FATAL,
                (errcode_for_file_access(), errmsg("could not write to audit index file: %m")));
    }

    pgaudit_close_file(fp, tblfile_path);
}

/* ----------
 * pgaudit_indextbl_init() -
 *
 *	Initialize audit index table.
 * ----------
 */
static void pgaudit_indextbl_init(void)
{
    uint32 index = 0;
    uint32 old_maxnum = 0;
    AuditIndexItem* item = NULL;

    pgaudit_read_indexfile(g_instance.attr.attr_security.Audit_directory);

    if (t_thrd.audit.audit_indextbl == NULL) {
        t_thrd.audit.audit_indextbl = (AuditIndexTable*)palloc0(
            (u_sess->attr.attr_security.Audit_RemainThreshold + 1) * sizeof(AuditIndexItem) + indextbl_header_size);
        t_thrd.audit.audit_indextbl->maxnum = u_sess->attr.attr_security.Audit_RemainThreshold + 1;
        auditfile_init();
        return;
    }

    auditfile_init();

    /* caculate the total space of the audit data */
    t_thrd.audit.pgaudit_totalspace = 0;

    if (t_thrd.audit.audit_indextbl != NULL) {
        index = t_thrd.audit.audit_indextbl->begidx;
        do {
            item = t_thrd.audit.audit_indextbl->data + index;

            t_thrd.audit.pgaudit_totalspace += item->filesize;

            if (index == t_thrd.audit.audit_indextbl->curidx)
                break;

            index = (index + 1) % t_thrd.audit.audit_indextbl->maxnum;
        } while (true);
    }

    t_thrd.audit.space_beyond_size =
        (t_thrd.audit.pgaudit_totalspace / SPACE_INTERVAL_SIZE) * SPACE_INTERVAL_SIZE + SPACE_INTERVAL_SIZE;

    old_maxnum = t_thrd.audit.audit_indextbl->maxnum;
    /* If file remain threshold parameter changed more little, than need to cleanup the audit data first */
    if (old_maxnum > (uint32)u_sess->attr.attr_security.Audit_RemainThreshold + 1) {
        int rc = snprintf_s(t_thrd.audit.pgaudit_filepath,
            MAXPGPATH,
            MAXPGPATH - 1,
            "%s/%s",
            g_instance.attr.attr_security.Audit_directory,
            audit_indextbl_file);
        securec_check_intval(rc, , );

        if (unlink(t_thrd.audit.pgaudit_filepath) < 0)
            ereport(WARNING, (errmsg("could not remove audit index table file: %m")));

        pgaudit_cleanup();
    }

    /* If file remain threshold parameter changed, than copy the old audit index table to the new table */
    if (old_maxnum != (uint32)u_sess->attr.attr_security.Audit_RemainThreshold + 1) {
        AuditIndexTable* new_indextbl = NULL;
        new_indextbl = (AuditIndexTable*)palloc0(
            (u_sess->attr.attr_security.Audit_RemainThreshold + 1) * sizeof(AuditIndexItem) + indextbl_header_size);
        new_indextbl->maxnum = u_sess->attr.attr_security.Audit_RemainThreshold + 1;

        if (t_thrd.audit.audit_indextbl != NULL && t_thrd.audit.audit_indextbl->count > 0) {
            uint32 pos = 0;
            errno_t errorno = EOK;
            index = t_thrd.audit.audit_indextbl->begidx;
            pos = new_indextbl->begidx;
            do {
                item = t_thrd.audit.audit_indextbl->data + index;
                errorno = memcpy_s(new_indextbl->data + pos,
                    (u_sess->attr.attr_security.Audit_RemainThreshold + 1) * sizeof(AuditIndexItem) - pos,
                    item,
                    sizeof(AuditIndexItem));
                securec_check(errorno, "\0", "\0");
                new_indextbl->count++;

                if (index == t_thrd.audit.audit_indextbl->curidx)
                    break;

                pos++;
                index = (index + 1) % t_thrd.audit.audit_indextbl->maxnum;
                new_indextbl->curidx = (new_indextbl->curidx + 1) % new_indextbl->maxnum;
            } while (true);
        }
        pfree(t_thrd.audit.audit_indextbl);
        t_thrd.audit.audit_indextbl = new_indextbl;

        pgaudit_update_indexfile(PG_BINARY_W, true);
    }
}

/*
 * Brief		: get the specified string field.
 * Description	:
 */
static const char* pgaudit_string_field(AuditData* adata, int num)
{
    int index = 0;
    uint32 size = 0;
    uint32 datalen = 0;
    const char* field = NULL;

    if (adata == NULL)
        return NULL;

    datalen = adata->header.size - AUDIT_HEADER_SIZE;
    field = adata->varstr;
    do {
        errno_t errorno = EOK;

        errorno = memcpy_s(&size, sizeof(uint32), field, sizeof(uint32));
        securec_check(errorno, "\0", "\0");

        datalen -= sizeof(uint32);
        if (size > datalen) { /* invalid data */
            return NULL;
        }

        field = field + sizeof(uint32);
        if (index == num) {
            break;
        }
        field = field + size;
        datalen -= size;
        index++;
    } while (index <= num);

    if (size == 0) {
        return NULL;
    }
    return field;
}

/*
 * Brief		: scan the specified audit file.
 * Description	:
 */
static void pgaudit_query_file(Tuplestorestate* state, TupleDesc tdesc, uint32 fnum, TimestampTz begtime,
    TimestampTz endtime, const char* audit_directory)
{
    FILE* fp = NULL;
    size_t nread = 0;
    TimestampTz datetime;
    AuditMsgHdr header;
    AuditData* adata = NULL;
    const char* field = NULL;

    if (state == NULL || tdesc == NULL)
        return;

    int rcs =
        snprintf_s(t_thrd.audit.pgaudit_filepath, MAXPGPATH, MAXPGPATH - 1, pgaudit_filename, audit_directory, fnum);
    securec_check_intval(rcs, , );
    /* Open the audit file to scan the audit record. */
    fp = AllocateFile(t_thrd.audit.pgaudit_filepath, PG_BINARY_R);
    if (NULL == fp) {
        ereport(LOG,
            (errcode_for_file_access(), errmsg("could not open audit file \"%s\": %m", t_thrd.audit.pgaudit_filepath)));
        return;
    }

    do {
        Datum values[PGAUDIT_QUERY_COLS] = {0};
        bool nulls[PGAUDIT_QUERY_COLS] = {0};
        int i = 0;
        errno_t errorno = EOK;

        /* read the audit message header first */
        nread = fread(&header, sizeof(AuditMsgHdr), 1, fp);
        if (0 == nread)
            break;

        if (header.signature[0] != 'A' || header.signature[1] != 'U' || header.version != 0 ||
            header.fields != PGAUDIT_QUERY_COLS) {
            ereport(LOG, (errmsg("invalid data in audit file \"%s\"", t_thrd.audit.pgaudit_filepath)));
            break;
        }

        /* read the whole audit record */
        adata = (AuditData*)palloc(header.size);
        errorno = memcpy_s(adata, header.size, &header, sizeof(AuditMsgHdr));
        securec_check(errorno, "\0", "\0");
        nread = fread((char*)adata + sizeof(AuditMsgHdr), header.size - sizeof(AuditMsgHdr), 1, fp);

        if (nread != 1) {
            ereport(WARNING,
                (errcode_for_file_access(),
                    errmsg("could not read audit file \"%s\": %m", t_thrd.audit.pgaudit_filepath)));
            pfree(adata);
            break;
        }

        datetime = time_t_to_timestamptz(adata->header.time);
        if (datetime >= begtime && datetime < endtime && header.flags == AUDIT_TUPLE_NORMAL) {
            values[i++] = TimestampTzGetDatum(datetime);
            values[i++] = CStringGetTextDatum(AuditTypeDesc(adata->type));
            values[i++] = CStringGetTextDatum(AuditResultDesc(adata->result));
            field = pgaudit_string_field(adata, AUDIT_USER_ID);
            values[i++] = CStringGetTextDatum(field ? field : _("null"));
            field = pgaudit_string_field(adata, AUDIT_USER_NAME);
            values[i++] = CStringGetTextDatum(field ? field : _("null"));
            field = pgaudit_string_field(adata, AUDIT_DATABASE_NAME);
            values[i++] = CStringGetTextDatum(field ? field : _("null"));
            field = pgaudit_string_field(adata, AUDIT_CLIENT_CONNINFO);
            values[i++] = CStringGetTextDatum(field ? field : _("null"));
            field = pgaudit_string_field(adata, AUDIT_OBJECT_NAME);
            values[i++] = CStringGetTextDatum(field ? field : _("null"));
            field = pgaudit_string_field(adata, AUDIT_DETAIL_INFO);
            values[i++] = CStringGetTextDatum(field ? field : _("null"));
            field = pgaudit_string_field(adata, AUDIT_NODENAME_INFO);
            values[i++] = CStringGetTextDatum(field ? field : _("null"));
            field = pgaudit_string_field(adata, AUDIT_THREADID_INFO);
            values[i++] = CStringGetTextDatum(field ? field : _("null"));
            field = pgaudit_string_field(adata, AUDIT_LOCALPORT_INFO);
            values[i++] = CStringGetTextDatum(field ? field : _("null"));
            field = pgaudit_string_field(adata, AUDIT_REMOTEPORT_INFO);
            values[i++] = CStringGetTextDatum(field ? field : _("null"));

            Assert(i == PGAUDIT_QUERY_COLS);

            tuplestore_putvalues(state, tdesc, values, nulls);
        }

        pfree(adata);
    } while (true);

    pgaudit_close_file(fp, t_thrd.audit.pgaudit_filepath);
}

/*
 * Brief		: scan the specified audit file to delete audit.
 * Description	:
 */
static void pgaudit_delete_file(uint32 fnum, TimestampTz begtime, TimestampTz endtime)
{
    int fd = -1;
    ssize_t nread = 0;
    TimestampTz datetime;
    AuditMsgHdr header;

    int rc = snprintf_s(t_thrd.audit.pgaudit_filepath,
        MAXPGPATH,
        MAXPGPATH - 1,
        pgaudit_filename,
        g_instance.attr.attr_security.Audit_directory,
        fnum);
    securec_check_intval(rc, , );

    /* Open the audit file to scan the audit record. */
    fd = open(t_thrd.audit.pgaudit_filepath, O_RDWR, pgaudit_filemode);
    if (fd < 0) {
        ereport(LOG,
            (errcode_for_file_access(), errmsg("could not open audit file \"%s\": %m", t_thrd.audit.pgaudit_filepath)));
        return;
    }

    do {
        /* read the audit message header first */
        nread = read(fd, &header, sizeof(AuditMsgHdr));
        if (nread <= 0)
            break;

        if (header.signature[0] != 'A' || header.signature[1] != 'U' || header.version != 0 ||
            header.fields != PGAUDIT_QUERY_COLS) {
            ereport(LOG, (errmsg("invalid data in audit file \"%s\"", t_thrd.audit.pgaudit_filepath)));
            break;
        }

        datetime = time_t_to_timestamptz(header.time);
        if (datetime >= begtime && datetime < endtime && header.flags == AUDIT_TUPLE_NORMAL) {
            long offset = sizeof(AuditMsgHdr);
            header.flags = AUDIT_TUPLE_DEAD;
            if (lseek(fd, -offset, SEEK_CUR) < 0) {
                ereport(WARNING, (errcode_for_file_access(), errmsg("could not seek in audit file: %m")));
                break;
            }
            if (write(fd, &header, sizeof(AuditMsgHdr)) != sizeof(AuditMsgHdr)) {
                ereport(WARNING, (errcode_for_file_access(), errmsg("could not write to audit file: %m")));
                break;
            }
        }
        if (lseek(fd, header.size - sizeof(AuditMsgHdr), SEEK_CUR) < 0) {
            ereport(WARNING, (errcode_for_file_access(), errmsg("could not seek in audit file: %m")));
            break;
        }
    } while (true);

    close(fd);
}

/* check whether system changed when auditor write audit data to current file */
static bool pgaudit_check_system(TimestampTz begtime, TimestampTz endtime, uint32 index)
{
    bool satisfied = false;
    TimestampTz curr_filetime = 0;
    TimestampTz next_filetime = 0;
    AuditIndexItem* item = t_thrd.audit.audit_indextbl->data + index;

    if (item->ctime > 0) {
        curr_filetime = time_t_to_timestamptz(item->ctime);
        /* check whether the item is the last item */
        if (index == t_thrd.audit.audit_indextbl->curidx) {
            if (curr_filetime <= begtime || curr_filetime <= endtime) {
                satisfied = true;
            }
        } else {
            item = t_thrd.audit.audit_indextbl->data + (index + 1) % t_thrd.audit.audit_indextbl->maxnum;
            if (item->ctime > 0) {
                next_filetime = time_t_to_timestamptz(item->ctime);
                /*
                 * check whether the time quantum between begtime and endtime
                 * intersect with the time quantum between curr_filetime and next_filetime
                 */
                curr_filetime = curr_filetime > begtime ? curr_filetime : begtime;
                next_filetime = next_filetime < endtime ? next_filetime : endtime;
                if (curr_filetime <= next_filetime) {
                    satisfied = true;
                }
            } else if (curr_filetime <= begtime || curr_filetime <= endtime) {
                satisfied = true;
            }
        }
    } else {
        satisfied = true;
    }

    return satisfied;
}

/*
 * Brief        : query audit information between begin time and end time.
 * Description  :
 */
Datum pg_query_audit(PG_FUNCTION_ARGS)
{
    ReturnSetInfo* rsinfo = (ReturnSetInfo*)fcinfo->resultinfo;
    TupleDesc tupdesc = NULL;
    Tuplestorestate* tupstore = NULL;
    MemoryContext per_query_ctx = NULL;
    MemoryContext oldcontext = NULL;
    TimestampTz begtime = PG_GETARG_TIMESTAMPTZ(0);
    TimestampTz endtime = PG_GETARG_TIMESTAMPTZ(1);
    Oid roleid = InvalidOid;
    char* audit_dir = NULL;

    /* Check some permissions first */
    roleid = GetUserId();
    if (!has_auditadmin_privilege(roleid)) {
        ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("permission denied to query audit")));
    }

    if (PG_NARGS() == PG_QUERY_AUDIT_ARGS_MAX) {
        audit_dir = text_to_cstring(PG_GETARG_TEXT_PP(PG_QUERY_AUDIT_ARGS_MAX - 1));
    }

    /* check to see if caller supports us returning a tuplestore */
    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo)) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("set-valued function called in context that cannot accept a set")));
    }
    if (!(rsinfo->allowedModes & SFRM_Materialize)) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("materialize mode required, but it is not allowed in this context")));
    }

    /* Build a tuple descriptor for our result type */
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE) {
        ereport(ERROR, (errcode(ERRCODE_SYSTEM_ERROR), errmsg("return type must be a row type")));
    }

    if (tupdesc->natts != PGAUDIT_QUERY_COLS) {
        ereport(ERROR, (errcode(ERRCODE_SYSTEM_ERROR), errmsg("attribute count of the return row type not matched")));
    }

    /*
     * When t_thrd.audit.audit_indextbl is not NULL,
     * but its origin memory context is NULL, free it will generate core
     */
    t_thrd.audit.audit_indextbl = NULL;
    audit_dir = (audit_dir == NULL) ? g_instance.attr.attr_security.Audit_directory : audit_dir;
    pgaudit_read_indexfile(audit_dir);

    per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
    oldcontext = MemoryContextSwitchTo(per_query_ctx);

    tupstore = tuplestore_begin_heap(true, false, u_sess->attr.attr_memory.work_mem);
    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult = tupstore;
    rsinfo->setDesc = tupdesc;

    MemoryContextSwitchTo(oldcontext);

    if (begtime < endtime && t_thrd.audit.audit_indextbl != NULL && t_thrd.audit.audit_indextbl->count > 0) {
        bool satisfied = false;
        uint32 index = 0;
        uint32 fnum = 0;
        AuditIndexItem* item = NULL;

        index = t_thrd.audit.audit_indextbl->begidx;
        do {
            item = t_thrd.audit.audit_indextbl->data + index;
            fnum = item->filenum;

            /* check whether system changed when auditor write audit data to current file */
            satisfied = pgaudit_check_system(begtime, endtime, index);
            if (satisfied) {
                pgaudit_query_file(tupstore, tupdesc, fnum, begtime, endtime, audit_dir);
                satisfied = false;
            }

            if (index == t_thrd.audit.audit_indextbl->curidx) {
                break;
            }

            index = (index + 1) % t_thrd.audit.audit_indextbl->maxnum;
        } while (true);
    }

    if (t_thrd.audit.audit_indextbl != NULL) {
        pfree(t_thrd.audit.audit_indextbl);
        t_thrd.audit.audit_indextbl = NULL;
    }
    /* clean up and return the tuplestore */
    tuplestore_donestoring(tupstore);

    return (Datum)0;
}

/*
 * Brief        : delete audit information between begin time and end time.
 * Description  :
 */
Datum pg_delete_audit(PG_FUNCTION_ARGS)
{
    TimestampTz begtime = PG_GETARG_TIMESTAMPTZ(0);
    TimestampTz endtime = PG_GETARG_TIMESTAMPTZ(1);

    t_thrd.audit.Audit_delete = true;

    /* Check some permissions first */
    Oid roleid = GetUserId();
    if (!has_auditadmin_privilege(roleid)) {
        ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("permission denied to delete audit")));
    }

    /*
     * When t_thrd.audit.audit_indextbl is not NULL,
     * but its origin memory context is NULL, free it will generate core
     */
    t_thrd.audit.audit_indextbl = NULL;
    pgaudit_read_indexfile(g_instance.attr.attr_security.Audit_directory);

    if (begtime < endtime && (t_thrd.audit.audit_indextbl != NULL) && t_thrd.audit.audit_indextbl->count > 0) {
        bool satisfied = false;
        uint32 index = 0;
        uint32 fnum = 0;
        AuditIndexItem* item = NULL;

        index = t_thrd.audit.audit_indextbl->begidx;
        do {
            item = t_thrd.audit.audit_indextbl->data + index;
            fnum = item->filenum;

            /* check whether system changed when auditor write audit data to current file */
            satisfied = pgaudit_check_system(begtime, endtime, index);
            if (satisfied) {
                pgaudit_delete_file(fnum, begtime, endtime);
                satisfied = false;
            }

            if (index == t_thrd.audit.audit_indextbl->curidx) {
                break;
            }

            index = (index + 1) % t_thrd.audit.audit_indextbl->maxnum;
        } while (true);
    }

    if (t_thrd.audit.audit_indextbl) {
        pfree(t_thrd.audit.audit_indextbl);
        t_thrd.audit.audit_indextbl = NULL;
    }

    PG_RETURN_VOID();
}

/*
 * @Description: check whether audit the login operator.
 * @in audittype : the audit type which need check.
 * @return : return true if need audit, otherwise return false.
 */
static bool check_audit_login(AuditType audittype)
{
    /* Obtain the login time for later use. */
    t_thrd.audit.user_login_time = GetCurrentTimestamp();

    if (audittype == AUDIT_LOGIN_SUCCESS) {
        if ((unsigned int)u_sess->attr.attr_security.Audit_Session & (1 << SESSION_LOGIN_SUCCESS))
            return true;
    } else {
        if ((unsigned int)u_sess->attr.attr_security.Audit_Session & (1 << SESSION_LOGIN_FAILED))
            return true;
    }
    return false;
}
