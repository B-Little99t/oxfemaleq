/*
 * Copyright (c) 2013-2019 Joris Vink <joris@coders.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/socket.h>

#include <openssl/rand.h>

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>

#include "kore.h"

#if !defined(KORE_NO_HTTP)
#include "http.h"
#endif

#if defined(KORE_USE_PGSQL)
#include "pgsql.h"
#endif

#if defined(KORE_USE_TASKS)
#include "tasks.h"
#endif

#if defined(KORE_USE_PYTHON)
#include "python_api.h"
#endif

#if defined(KORE_USE_CURL)
#include "curl.h"
#endif

#if !defined(WAIT_ANY)
#define WAIT_ANY		(-1)
#endif

#define WORKER_SOLO_COUNT	2

#define WORKER(id)						\
	(struct kore_worker *)((u_int8_t *)kore_workers +	\
	    (sizeof(struct kore_worker) * id))

struct wlock {
	volatile int		lock;
	pid_t			current;
};

static int	worker_trylock(void);
static void	worker_unlock(void);

static inline int	worker_acceptlock_obtain(void);
static inline void	worker_acceptlock_release(void);
static void		worker_accept_avail(struct kore_msg *, const void *);

static void	worker_entropy_recv(struct kore_msg *, const void *);
static void	worker_keymgr_response(struct kore_msg *, const void *);
static int	worker_keymgr_response_verify(struct kore_msg *, const void *,
		    struct kore_domain **);

static int				accept_avail;
static struct kore_worker		*kore_workers;
static int				worker_no_lock;
static int				shm_accept_key;
static struct wlock			*accept_lock;

struct kore_worker		*worker = NULL;
u_int8_t			worker_set_affinity = 1;
u_int32_t			worker_accept_threshold = 16;
u_int32_t			worker_rlimit_nofiles = 768;
u_int32_t			worker_max_connections = 512;
u_int32_t			worker_active_connections = 0;
int				worker_policy = KORE_WORKER_POLICY_RESTART;

void
kore_worker_init(void)
{
	size_t			len;
	struct kore_worker	*kw;
	u_int16_t		i, cpu;

	worker_no_lock = 0;

	if (worker_count == 0)
		worker_count = cpu_count;

	/* Account for the keymgr even if we don't end up starting it. */
	worker_count += 1;

	len = sizeof(*accept_lock) +
	    (sizeof(struct kore_worker) * worker_count);

	shm_accept_key = shmget(IPC_PRIVATE, len, IPC_CREAT | IPC_EXCL | 0700);
	if (shm_accept_key == -1)
		fatal("kore_worker_init(): shmget() %s", errno_s);
	if ((accept_lock = shmat(shm_accept_key, NULL, 0)) == (void *)-1)
		fatal("kore_worker_init(): shmat() %s", errno_s);

	accept_lock->lock = 0;
	accept_lock->current = 0;

	kore_workers = (struct kore_worker *)((u_int8_t *)accept_lock +
	    sizeof(*accept_lock));
	memset(kore_workers, 0, sizeof(struct kore_worker) * worker_count);

	kore_debug("kore_worker_init(): system has %d cpu's", cpu_count);
	kore_debug("kore_worker_init(): starting %d workers", worker_count);

	if (worker_count > cpu_count) {
		kore_debug("kore_worker_init(): more workers than cpu's");
	}

	/* Setup log buffers. */
	for (i = 0; i < worker_count; i++) {
		kw = WORKER(i);
		kw->lb.offset = 0;
	}

	/* Start keymgr if required. */
	if (keymgr_active)
		kore_worker_spawn(0, 0);

	/* Now start all the workers. */
	cpu = 1;
	for (i = 1; i < worker_count; i++) {
		if (cpu >= cpu_count)
			cpu = 0;
		kore_worker_spawn(i, cpu++);
	}
}

void
kore_worker_spawn(u_int16_t id, u_int16_t cpu)
{
	struct kore_worker	*kw;

	kw = WORKER(id);
	kw->id = id;
	kw->cpu = cpu;
	kw->has_lock = 0;
	kw->active_hdlr = NULL;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, kw->pipe) == -1)
		fatal("socketpair(): %s", errno_s);

	if (!kore_connection_nonblock(kw->pipe[0], 0) ||
	    !kore_connection_nonblock(kw->pipe[1], 0))
		fatal("could not set pipe fds to nonblocking: %s", errno_s);

	kw->pid = fork();
	if (kw->pid == -1)
		fatal("could not spawn worker child: %s", errno_s);

	if (kw->pid == 0) {
		kw->pid = getpid();
		kore_worker_entry(kw);
		/* NOTREACHED */
	}
}

struct kore_worker *
kore_worker_data(u_int8_t id)
{
	if (id >= worker_count)
		fatal("id %u too large for worker count", id);

	return (WORKER(id));
}

void
kore_worker_shutdown(void)
{
	struct kore_worker	*kw;
	pid_t			pid;
	int			status;
	u_int16_t		id, done;

	if (!kore_quiet) {
		kore_log(LOG_NOTICE,
		    "waiting for workers to drain and shutdown");
	}

	for (;;) {
		for (id = 0; id < worker_count; id++) {
			kw = WORKER(id);
			if (kw->pid != 0) {
				pid = waitpid(kw->pid, &status, 0);
				if (pid == -1)
					continue;
				kw->pid = 0;
			}
		}

		done = 0;
		for (id = 0; id < worker_count; id++) {
			kw = WORKER(id);
			if (kw->pid == 0)
				done++;
		}

		if (done == worker_count)
			break;
	}

	if (shmctl(shm_accept_key, IPC_RMID, NULL) == -1) {
		kore_log(LOG_NOTICE,
		    "failed to deleted shm segment: %s", errno_s);
	}
}

void
kore_worker_dispatch_signal(int sig)
{
	u_int16_t		id;
	struct kore_worker	*kw;

	for (id = 0; id < worker_count; id++) {
		kw = WORKER(id);
		if (kill(kw->pid, sig) == -1) {
			kore_debug("kill(%d, %d): %s", kw->pid, sig, errno_s);
		}
	}
}

void
kore_worker_privdrop(const char *runas, const char *root)
{
	rlim_t			fd;
	struct rlimit		rl;
	struct passwd		*pw = NULL;

	if (root == NULL)
		fatalx("no root directory for kore_worker_privdrop");

	/* Must happen before chroot. */
	if (skip_runas == 0) {
		if (runas == NULL)
			fatalx("no runas user given and -r not specified");
		pw = getpwnam(runas);
		if (pw == NULL) {
			fatalx("cannot getpwnam(\"%s\") for user: %s",
			    runas, errno_s);
		}
	}

	if (skip_chroot == 0) {
		if (chroot(root) == -1) {
			fatalx("cannot chroot(\"%s\"): %s",
			    root, errno_s);
		}

		if (chdir("/") == -1)
			fatalx("cannot chdir(\"/\"): %s", errno_s);
	} else {
		if (chdir(root) == -1)
			fatalx("cannot chdir(\"%s\"): %s", root, errno_s);
	}

	if (getrlimit(RLIMIT_NOFILE, &rl) == -1) {
		kore_log(LOG_WARNING, "getrlimit(RLIMIT_NOFILE): %s", errno_s);
	} else {
		for (fd = 0; fd < rl.rlim_cur; fd++) {
			if (fcntl(fd, F_GETFD, NULL) != -1) {
				worker_rlimit_nofiles++;
			}
		}
	}

	rl.rlim_cur = worker_rlimit_nofiles;
	rl.rlim_max = worker_rlimit_nofiles;
	if (setrlimit(RLIMIT_NOFILE, &rl) == -1) {
		kore_log(LOG_ERR, "setrlimit(RLIMIT_NOFILE, %u): %s",
		    worker_rlimit_nofiles, errno_s);
	}

	if (skip_runas == 0) {
		if (setgroups(1, &pw->pw_gid) ||
#if defined(__MACH__) || defined(NetBSD)
		    setgid(pw->pw_gid) || setegid(pw->pw_gid) ||
		    setuid(pw->pw_uid) || seteuid(pw->pw_uid))
#else
		    setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) ||
		    setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid))
#endif
			fatalx("cannot drop privileges");
	}

	kore_platform_sandbox();
}

void
kore_worker_entry(struct kore_worker *kw)
{
	struct kore_runtime_call	*rcall;
	char				buf[16];
	u_int64_t			last_seed;
	int				quit, had_lock;
	u_int64_t			netwait, now, next_prune;

	worker = kw;

	(void)snprintf(buf, sizeof(buf), "[wrk %d]", kw->id);
	if (kw->id == KORE_WORKER_KEYMGR)
		(void)snprintf(buf, sizeof(buf), "[keymgr]");
	kore_platform_proctitle(buf);

	if (worker_set_affinity == 1)
		kore_platform_worker_setcpu(kw);

	kore_pid = kw->pid;

	kore_signal_setup();

	if (kw->id == KORE_WORKER_KEYMGR) {
		kore_keymgr_run();
		exit(0);
	}

	net_init();
	kore_connection_init();
	kore_platform_event_init();
	kore_msg_worker_init();

#if defined(KORE_USE_TASKS)
	kore_task_init();
#endif

	kore_worker_privdrop(kore_runas_user, kore_root_path);

#if !defined(KORE_NO_HTTP)
	http_init();
	kore_filemap_resolve_paths();
	kore_accesslog_worker_init();
#endif
	kore_timer_init();
	kore_fileref_init();
	kore_domain_keymgr_init();

	quit = 0;
	had_lock = 0;
	next_prune = 0;
	accept_avail = 1;
	worker_active_connections = 0;

	last_seed = 0;

	if (keymgr_active) {
		kore_msg_register(KORE_MSG_CRL, worker_keymgr_response);
		kore_msg_register(KORE_MSG_ENTROPY_RESP, worker_entropy_recv);
		kore_msg_register(KORE_MSG_CERTIFICATE, worker_keymgr_response);

		if (worker->restarted) {
			kore_msg_send(KORE_WORKER_KEYMGR,
			    KORE_MSG_CERTIFICATE_REQ, NULL, 0);
		}
	}

	kore_msg_register(KORE_MSG_ACCEPT_AVAILABLE, worker_accept_avail);

	if (nlisteners == 0)
		worker_no_lock = 1;

	if (!kore_quiet) {
		kore_log(LOG_NOTICE,
		    "worker %d started (cpu#%d, pid#%d)",
		    kw->id, kw->cpu, kw->pid);
	}

	rcall = kore_runtime_getcall("kore_worker_configure");
	if (rcall != NULL) {
		kore_runtime_execute(rcall);
		kore_free(rcall);
	}

	kore_module_onload();
	worker->restarted = 0;

	for (;;) {
		now = kore_time_ms();

		if (keymgr_active && (now - last_seed) > KORE_RESEED_TIME) {
			kore_msg_send(KORE_WORKER_KEYMGR,
			    KORE_MSG_ENTROPY_REQ, NULL, 0);
			last_seed = now;
		}

		if (!worker->has_lock && accept_avail) {
			accept_avail = 0;
			if (worker_acceptlock_obtain()) {
				if (had_lock == 0) {
					kore_platform_enable_accept();
					had_lock = 1;
				}
			}
		}

		netwait = kore_timer_next_run(now);

#if !defined(KORE_NO_HTTP)
		if (netwait == KORE_WAIT_INFINITE && http_request_count > 0)
			netwait = 100;
#endif

		kore_platform_event_wait(netwait);
		now = kore_time_ms();

		if (worker->has_lock)
			worker_acceptlock_release();

		if (!worker->has_lock) {
			if (had_lock == 1) {
				had_lock = 0;
				kore_platform_disable_accept();
			}
		}

		if (sig_recv != 0) {
			switch (sig_recv) {
			case SIGHUP:
				kore_module_reload(1);
				break;
			case SIGQUIT:
			case SIGINT:
			case SIGTERM:
				quit = 1;
				break;
			case SIGCHLD:
#if defined(KORE_USE_PYTHON)
				kore_python_proc_reap();
#endif
				break;
			default:
				break;
			}

			sig_recv = 0;
		}

		if (quit)
			break;

		kore_timer_run(now);
#if defined(KORE_USE_CURL)
		kore_curl_do_timeout();
#endif
#if !defined(KORE_NO_HTTP)
		http_process();
#endif
#if defined(KORE_USE_PYTHON)
		kore_python_coro_run();
#endif
#if defined(KORE_USE_CURL)
		kore_curl_do_timeout();
#endif
		if (next_prune <= now) {
			kore_connection_check_timeout(now);
			kore_connection_prune(KORE_CONNECTION_PRUNE_DISCONNECT);
			next_prune = now + 500;
		}
	}

	rcall = kore_runtime_getcall("kore_worker_teardown");
	if (rcall != NULL) {
		kore_runtime_execute(rcall);
		kore_free(rcall);
	}

	kore_server_cleanup();

	kore_platform_event_cleanup();
	kore_connection_cleanup();
	kore_domain_cleanup();
	kore_module_cleanup();
#if !defined(KORE_NO_HTTP)
	http_cleanup();
#endif
	net_cleanup();

#if defined(KORE_USE_PYTHON)
	kore_python_cleanup();
#endif

#if defined(KORE_USE_PGSQL)
	kore_pgsql_sys_cleanup();
#endif

	kore_debug("worker %d shutting down", kw->id);

	kore_mem_cleanup();
	exit(0);
}

void
kore_worker_reap(void)
{
	u_int16_t		id;
	pid_t			pid;
	struct kore_worker	*kw;
	const char		*func;
	int			status;

	for (;;) {
		pid = waitpid(WAIT_ANY, &status, WNOHANG);

		if (pid == -1) {
			if (errno == ECHILD)
				return;
			if (errno == EINTR)
				continue;
			kore_log(LOG_ERR,
			    "failed to wait for children: %s", errno_s);
			return;
		}

		break;
	}

	if (pid == 0)
		return;

	for (id = 0; id < worker_count; id++) {
		kw = WORKER(id);
		if (kw->pid != pid)
			continue;

		if (!kore_quiet) {
			kore_log(LOG_NOTICE,
			    "worker %d (%d) exited with status %d",
			    kw->id, pid, status);
		}

		func = "none";
#if !defined(KORE_NO_HTTP)
		if (kw->active_hdlr != NULL)
			func = kw->active_hdlr->func;
#endif
		kore_log(LOG_NOTICE,
		    "worker %d (pid: %d) (hdlr: %s) gone",
		    kw->id, kw->pid, func);

#if defined(__linux__)
		if (WIFSIGNALED(status) && WTERMSIG(status) == SIGSYS) {
			kore_log(LOG_NOTICE,
			    "worker %d died from sandbox violation", kw->id);
		}
#endif

		if (id == KORE_WORKER_KEYMGR) {
			kore_log(LOG_CRIT, "keymgr gone, stopping");
			kw->pid = 0;
			if (raise(SIGTERM) != 0) {
				kore_log(LOG_WARNING,
				    "failed to raise SIGTERM signal");
			}
			break;
		}

		if (kw->pid == accept_lock->current &&
		    worker_no_lock == 0)
			worker_unlock();

#if !defined(KORE_NO_HTTP)
		if (kw->active_hdlr != NULL) {
			kw->active_hdlr->errors++;
			kore_log(LOG_NOTICE,
			    "hdlr %s has caused %d error(s)",
			    kw->active_hdlr->func,
			    kw->active_hdlr->errors);
		}
#endif

		if (worker_policy == KORE_WORKER_POLICY_TERMINATE) {
			kw->pid = 0;
			kore_log(LOG_NOTICE,
			    "worker policy is 'terminate', stopping");
			if (raise(SIGTERM) != 0) {
				kore_log(LOG_WARNING,
				    "failed to raise SIGTERM signal");
			}
			break;
		}

		kore_log(LOG_NOTICE, "restarting worker %d", kw->id);
		kw->restarted = 1;
		kore_msg_parent_remove(kw);
		kore_worker_spawn(kw->id, kw->cpu);
		kore_msg_parent_add(kw);

		break;
	}
}

void
kore_worker_make_busy(void)
{
	if (worker_count == WORKER_SOLO_COUNT || worker_no_lock == 1)
		return;

	if (worker->has_lock) {
		worker_unlock();
		worker->has_lock = 0;
		kore_msg_send(KORE_MSG_WORKER_ALL,
		    KORE_MSG_ACCEPT_AVAILABLE, NULL, 0);
	}
}

static inline void
worker_acceptlock_release(void)
{
	if (worker_count == WORKER_SOLO_COUNT || worker_no_lock == 1)
		return;

	if (worker->has_lock != 1)
		return;

	if (worker_active_connections < worker_max_connections) {
#if !defined(KORE_NO_HTTP)
		if (http_request_count < http_request_limit)
			return;
#else
		return;
#endif
	}

#if defined(WORKER_DEBUG)
	kore_log(LOG_DEBUG, "worker busy, releasing lock");
#endif

	worker_unlock();
	worker->has_lock = 0;

	kore_msg_send(KORE_MSG_WORKER_ALL, KORE_MSG_ACCEPT_AVAILABLE, NULL, 0);
}

static inline int
worker_acceptlock_obtain(void)
{
	int		r;

	if (worker->has_lock == 1)
		return (1);

	if (worker_count == WORKER_SOLO_COUNT || worker_no_lock == 1) {
		worker->has_lock = 1;
		return (1);
	}

	if (worker_active_connections >= worker_max_connections)
		return (0);

#if !defined(KORE_NO_HTTP)
	if (http_request_count >= http_request_limit)
		return (0);
#endif

	r = 0;
	if (worker_trylock()) {
		r = 1;
		worker->has_lock = 1;
#if defined(WORKER_DEBUG)
		kore_log(LOG_DEBUG, "got lock");
#endif
	}

	return (r);
}

static int
worker_trylock(void)
{
	if (!__sync_bool_compare_and_swap(&(accept_lock->lock), 0, 1))
		return (0);

	accept_lock->current = worker->pid;

	return (1);
}

static void
worker_unlock(void)
{
	accept_lock->current = 0;
	if (!__sync_bool_compare_and_swap(&(accept_lock->lock), 1, 0))
		kore_log(LOG_NOTICE, "worker_unlock(): wasnt locked");
}

static void
worker_accept_avail(struct kore_msg *msg, const void *data)
{
	accept_avail = 1;
}

static void
worker_entropy_recv(struct kore_msg *msg, const void *data)
{
	if (msg->length != 1024) {
		kore_log(LOG_WARNING,
		    "invalid entropy response (got:%zu - wanted:1024)",
		    msg->length);
	}

	RAND_poll();
	RAND_seed(data, msg->length);
}

static void
worker_keymgr_response(struct kore_msg *msg, const void *data)
{
	struct kore_domain		*dom;
	const struct kore_x509_msg	*req;

	if (!worker_keymgr_response_verify(msg, data, &dom))
		return;

	req = (const struct kore_x509_msg *)data;

	switch (msg->id) {
	case KORE_MSG_CERTIFICATE:
		kore_domain_tlsinit(dom, req->data, req->data_len);
		break;
	case KORE_MSG_CRL:
		kore_domain_crl_add(dom, req->data, req->data_len);
		break;
	default:
		kore_log(LOG_WARNING, "unknown keymgr request %u", msg->id);
		break;
	}
}

static int
worker_keymgr_response_verify(struct kore_msg *msg, const void *data,
    struct kore_domain **out)
{
	struct kore_server		*srv;
	struct kore_domain		*dom;
	const struct kore_x509_msg	*req;

	if (msg->length < sizeof(*req)) {
		kore_log(LOG_WARNING,
		    "short keymgr message (%zu)", msg->length);
		return (KORE_RESULT_ERROR);
	}

	req = (const struct kore_x509_msg *)data;
	if (msg->length != (sizeof(*req) + req->data_len)) {
		kore_log(LOG_WARNING,
		    "invalid keymgr payload (%zu)", msg->length);
		return (KORE_RESULT_ERROR);
	}

	if (req->domain_len > KORE_DOMAINNAME_LEN) {
		kore_log(LOG_WARNING,
		    "invalid keymgr domain (%u)",
		    req->domain_len);
		return (KORE_RESULT_ERROR);
	}

	LIST_FOREACH(srv, &kore_servers, list) {
		dom = NULL;

		if (srv->tls == 0)
			continue;

		TAILQ_FOREACH(dom, &srv->domains, list) {
			if (!strncmp(dom->domain, req->domain, req->domain_len))
				break;
		}

		if (dom != NULL)
			break;
	}

	if (dom == NULL) {
		kore_log(LOG_WARNING,
		    "got keymgr response for domain that does not exist");
		return (KORE_RESULT_ERROR);
	}

	*out = dom;

	return (KORE_RESULT_OK);
}
