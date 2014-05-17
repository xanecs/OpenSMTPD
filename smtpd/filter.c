/*	$OpenBSD$	*/

/*
 * Copyright (c) 2011 Gilles Chehade <gilles@poolp.org>
 * Copyright (c) 2012 Eric Faurot <eric@openbsd.org>
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

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <netinet/in.h>

#include <ctype.h>
#include <errno.h>
#include <event.h>
#include <imsg.h>
#include <inttypes.h>
#include <resolv.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "smtpd.h"
#include "log.h"

enum {
	QT_QUERY,
	QT_EVENT,
};

enum {
	QUERY_READY,
	QUERY_WAITING,
	QUERY_RUNNING,
	QUERY_DONE
};


struct filter_proc {
	TAILQ_ENTRY(filter_proc)	 entry;
	struct mproc			 mproc;
	int				 hooks;
	int				 flags;
	int				 ready;
};

struct filter {
	TAILQ_ENTRY(filter)		 entry;
	struct filter_proc		*proc;
};
TAILQ_HEAD(filter_lst, filter);

TAILQ_HEAD(filter_query_lst, filter_query);
struct filter_session {
	uint64_t		 id;
	int			 terminate;
	struct filter_lst	*filters;
	struct filter		*fcurr;

	struct filter_query_lst	 queries;

	struct io		 io;
	struct iobuf		 iobuf;
	FILE			*ofile;
	size_t			 datain;
	size_t			 datalen;
	int			 error;
	struct filter_query	*eom;
};

struct filter_query {
	uint64_t			 qid;
	int				 type;
	int				 hook;
	struct filter_session		*session;
	TAILQ_ENTRY(filter_query)	 entry;

	int				 state;
	int				 hasrun;
	struct filter			*current;

	/* current data */
	union {
		struct {
			struct sockaddr_storage	 local;
			struct sockaddr_storage	 remote;
			char			 hostname[SMTPD_MAXHOSTNAMELEN];
		} connect;
		char			line[SMTPD_MAXLINESIZE];
		struct mailaddr		maddr;
		size_t			datalen;
	} u;

	/* current response */
	struct {
		int	 status;
		int	 code;
		char	*response;	
	} smtp;
};

static void filter_imsg(struct mproc *, struct imsg *);
static struct filter_query *filter_query(struct filter_session *, int, int);
static void filter_drain_query(struct filter_query *);
static void filter_run_query(struct filter *, struct filter_query *);
static void filter_end_query(struct filter_query *);
static void filter_set_fdout(struct filter_session *, int);
static int filter_tx(struct filter_session *, int);
static void filter_tx_io(struct io *, int);

static TAILQ_HEAD(, filter_proc)	procs;
struct dict				chains;

static const char * filter_session_to_text(struct filter_session *);
static const char * filter_query_to_text(struct filter_query *);
static const char * filter_to_text(struct filter *);
static const char * filter_proc_to_text(struct filter_proc *);
static const char * type_to_str(int);
static const char * query_to_str(int);
static const char * event_to_str(int);
static const char * status_to_str(int);
static const char * filterimsg_to_str(int);

struct tree	sessions;
struct tree	queries;

static void
filter_extend_chain(struct filter_lst *chain, const char *name)
{
	struct filter		*n;
	struct filter_lst	*fchain;
	struct filter_conf	*fconf;
	int			 i;

	fconf = dict_xget(&env->sc_filters, name);
	if (fconf->chain) {
		log_debug("filter:     extending with \"%s\"", name);
		for (i = 0; i < fconf->argc; i++)
			filter_extend_chain(chain, fconf->argv[i]);
	}
	else {
		log_debug("filter:     adding filter \"%s\"", name);
		n = xcalloc(1, sizeof(*n), "filter_extend_chain");
		fchain = dict_get(&chains, name);
		n->proc = TAILQ_FIRST(fchain)->proc;
		TAILQ_INSERT_TAIL(chain, n, entry);
	}
}

void
filter_postfork(void)
{
	static int		 prepare = 0;
	struct filter_conf	*filter;
	void			*iter;
	struct filter_proc	*proc;
	struct filter_lst	*fchain;
	struct filter		*f;
	struct mproc		*p;
	int			 done, i;

	if (prepare)
		return;
	prepare = 1;

	TAILQ_INIT(&procs);
	dict_init(&chains);

	log_debug("filter: building simple chains...");

	/* create all filter proc and associated chains */
	iter = NULL;
	while (dict_iter(&env->sc_filters, &iter, NULL, (void **)&filter)) {
		if (filter->chain)
			continue;

		log_debug("filter: building simple chain \"%s\"", filter->name);

		proc = xcalloc(1, sizeof(*proc), "filter_postfork");
		p = &proc->mproc;
		p->handler = filter_imsg;
		p->proc = PROC_FILTER;
		p->name = xstrdup(filter->name, "filter_postfork");
		p->data = proc;
		if (mproc_fork(p, filter->path, filter->argv) < 0)
			fatalx("filter_postfork");

		log_debug("filter: registering proc \"%s\"", filter->name);

		f = xcalloc(1, sizeof(*f), "filter_postfork");
		f->proc = proc;

		TAILQ_INSERT_TAIL(&procs, proc, entry);
		fchain = xcalloc(1, sizeof(*fchain), "filter_postfork");
		TAILQ_INIT(fchain);
		TAILQ_INSERT_TAIL(fchain, f, entry);
		dict_xset(&chains, filter->name, fchain);
		filter->done = 1;
	}

	log_debug("filter: building complex chains...");

	/* resolve all chains */
	done = 0;
	while (!done) {
		done = 1;
		iter = NULL;
		while (dict_iter(&env->sc_filters, &iter, NULL, (void **)&filter)) {
			if (filter->done)
				continue;
			done = 0;
			filter->done = 1;
			for (i = 0; i < filter->argc; i++) {
				if (!dict_get(&chains, filter->argv[i])) {
					filter->done = 0;
					break;
				}
			}
			if (filter->done == 0)
				continue;
			fchain = xcalloc(1, sizeof(*fchain), "filter_postfork");
			TAILQ_INIT(fchain);
			log_debug("filter: building chain \"%s\"...", filter->name);
			for (i = 0; i < filter->argc; i++)
				filter_extend_chain(fchain, filter->argv[i]);
			log_debug("filter: done building chain \"%s\"", filter->name);
			dict_xset(&chains, filter->name, fchain);
		}
	}
	log_debug("filter: done building complex chains");

	if (dict_get(&chains, "default") == NULL) {
		log_debug("filter: done building default chain");
		fchain = xcalloc(1, sizeof(*fchain), "filter_postfork");
		TAILQ_INIT(fchain);
		dict_xset(&chains, "default", fchain);
	}
}

void
filter_configure(void)
{
	static int		 init = 0;
	struct filter_proc	*p;

	if (init)
		return;
	init = 1;

	tree_init(&sessions);
	tree_init(&queries);

	TAILQ_FOREACH(p, &procs, entry) {
		m_create(&p->mproc, IMSG_FILTER_REGISTER, 0, 0, -1);
		m_add_u32(&p->mproc, FILTER_API_VERSION);
		m_add_string(&p->mproc, p->mproc.name);
		m_close(&p->mproc);
		mproc_enable(&p->mproc);
	}

	if (TAILQ_FIRST(&procs) == NULL)
		smtp_configure();
}

void
filter_event(uint64_t id, int event)
{
	struct filter_session	*s;
	struct filter_query	*q;

	if (event == EVENT_CONNECT) {
		s = xcalloc(1, sizeof(*s), "filter_event");
		s->id = id;
		s->filters = dict_xget(&chains, "default");
		s->io.sock = -1;
		TAILQ_INIT(&s->queries);
		tree_xset(&sessions, s->id, s);
	}
	else if (event == EVENT_DISCONNECT)
		/* On disconnect, the session is virtualy dead */
		s = tree_xpop(&sessions, id);
	else
		s = tree_xget(&sessions, id);
	q = filter_query(s, QT_EVENT, event);

	filter_drain_query(q);
}

void
filter_connect(uint64_t id, const struct sockaddr *local,
	const struct sockaddr *remote, const char *host)
{
	struct filter_session	*s;
	struct filter_query	*q;

	s = tree_xget(&sessions, id);
	q = filter_query(s, QT_QUERY, QUERY_CONNECT);

	memmove(&q->u.connect.local, local, local->sa_len);
	memmove(&q->u.connect.remote, remote, remote->sa_len);
	strlcpy(q->u.connect.hostname, host, sizeof(q->u.connect.hostname));

	q->smtp.status = FILTER_OK;
	q->smtp.code = 0;
	q->smtp.response = NULL;

	filter_drain_query(q);
}

void
filter_mailaddr(uint64_t id, int qhook, const struct mailaddr *maddr)
{
	struct filter_session	*s;
	struct filter_query	*q;

	s = tree_xget(&sessions, id);
	q = filter_query(s, QT_QUERY, qhook);

	strlcpy(q->u.maddr.user, maddr->user, sizeof(q->u.maddr.user));
	strlcpy(q->u.maddr.domain, maddr->domain, sizeof(q->u.maddr.domain));

	filter_drain_query(q);
}

void
filter_line(uint64_t id, int qhook, const char *line)
{
	struct filter_session	*s;
	struct filter_query	*q;

	s = tree_xget(&sessions, id);
	q = filter_query(s, QT_QUERY, qhook);

	if (line)
		strlcpy(q->u.line, line, sizeof(q->u.line));

	filter_drain_query(q);
}

void
filter_eom(uint64_t id, int qhook, size_t datalen)
{
	struct filter_session	*s;
	struct filter_query	*q;

	s = tree_xget(&sessions, id);
	q = filter_query(s, QT_QUERY, qhook);
	q->u.datalen = datalen;

	filter_drain_query(q);
}

static void
filter_set_fdout(struct filter_session *s, int fdout)
{
	struct mproc	*p;
	int		 fd;

	while(s->fcurr) {
		if (s->fcurr->proc->hooks & HOOK_DATALINE) {
			log_trace(TRACE_MFA, "filter: sending fd %d to %s", fdout, filter_to_text(s->fcurr));
			p = &s->fcurr->proc->mproc;
			m_create(p, IMSG_FILTER_PIPE_SETUP, 0, 0, fdout);
			m_add_id(p, s->id);
			m_close(p);
			return;
		}
		s->fcurr = TAILQ_PREV(s->fcurr, filter_lst, entry);
	}

	log_trace(TRACE_MFA, "filter: chain input is %d", fdout);

	fd = filter_tx(s, fdout);
	smtp_filter_fd(s->id, fd);
}

void
filter_build_fd_chain(uint64_t id, int fdout)
{
	struct filter_session	*s;

	s = tree_xget(&sessions, id);
	s->fcurr = TAILQ_LAST(s->filters, filter_lst);

	filter_set_fdout(s, fdout);
}

static struct filter_query *
filter_query(struct filter_session *s, int type, int qhook)
{
	struct filter_query	*q;

	q = xcalloc(1, sizeof *q, "filter_query");
	q->qid = generate_uid();
	q->session = s;
	q->type = type;
	q->hook = qhook;
	TAILQ_INSERT_TAIL(&s->queries, q, entry);

	q->state = QUERY_READY;
	q->current = TAILQ_FIRST(s->filters);
	q->hasrun = 0;

	if (type == QT_QUERY)
		log_trace(TRACE_MFA, "filter: new query %s %s", type_to_str(type),
		    query_to_str(qhook));
	else
		log_trace(TRACE_MFA, "filter: new query %s %s", type_to_str(type),
		    event_to_str(qhook));

	return (q);
}

static void
filter_drain_query(struct filter_query *q)
{
	struct filter_query	*prev;

	log_trace(TRACE_MFA, "filter: filter_drain_query %s", filter_query_to_text(q));

	/*
	 * The query must be passed through all filters that registered
	 * a hook, until one rejects it.  
	 */
	while (q->state != QUERY_DONE) {

		/* Walk over all filters */
		while (q->current) {

			/* Trigger the current filter if not done yet. */
			if (!q->hasrun) {
				filter_run_query(q->current, q);
				q->hasrun = 1;
			}
			if (q->state == QUERY_RUNNING) {
				log_trace(TRACE_MFA,
				    "filter: waiting for running query %s",
				    filter_query_to_text(q));
				return;
			}

			/*
			 * Do not move forward if the query ahead of us is
			 * waiting on this filter.
			 */
			prev = TAILQ_PREV(q, filter_query_lst, entry);
			if (prev && prev->current == q->current) {
				q->state = QUERY_WAITING;
				log_trace(TRACE_MFA,
				    "filter: query blocked by previous query %s",
				    filter_query_to_text(prev));
				return;
			}

			q->current = TAILQ_NEXT(q->current, entry);
			q->hasrun = 0;
		}
		q->state = QUERY_DONE;
	}

	/* Defer the response if the file is not closed yet. */
	if (q->type == QT_QUERY && q->hook == QUERY_EOM && q->session->ofile) {
		log_debug("filter: deferring eom query...");
		q->session->eom = q;
		return;
	}

	filter_end_query(q);
}

static void
filter_run_query(struct filter *f, struct filter_query *q)
{
	if (q->type == QT_QUERY) {

		log_trace(TRACE_MFA, "filter: running filter %s for query %s",
		    filter_to_text(f), filter_query_to_text(q));

		m_create(&f->proc->mproc, IMSG_FILTER_QUERY, 0, 0, -1);
		m_add_id(&f->proc->mproc, q->session->id);
		m_add_id(&f->proc->mproc, q->qid);
		m_add_int(&f->proc->mproc, q->hook);

		switch (q->hook) {
		case QUERY_CONNECT:
			m_add_sockaddr(&f->proc->mproc,
			    (struct sockaddr *)&q->u.connect.local);
			m_add_sockaddr(&f->proc->mproc,
			    (struct sockaddr *)&q->u.connect.remote);
			m_add_string(&f->proc->mproc, q->u.connect.hostname);
			break;
		case QUERY_HELO:
			m_add_string(&f->proc->mproc, q->u.line);
			break;
		case QUERY_MAIL:
		case QUERY_RCPT:
			m_add_mailaddr(&f->proc->mproc, &q->u.maddr);
			break;
		case QUERY_EOM:
			m_add_u32(&f->proc->mproc, q->u.datalen);
			break;
		default:
			break;
		}
		m_close(&f->proc->mproc);

		tree_xset(&queries, q->qid, q);
		q->state = QUERY_RUNNING;
	}
	else {
		log_trace(TRACE_MFA, "filter: running filter %s for query %s",
		    filter_to_text(f), filter_query_to_text(q));

		m_create(&f->proc->mproc, IMSG_FILTER_EVENT, 0, 0, -1);
		m_add_id(&f->proc->mproc, q->session->id);
		m_add_int(&f->proc->mproc, q->hook);
		m_close(&f->proc->mproc);
 	}
}

static void
filter_end_query(struct filter_query *q)
{
	struct filter_session *s = q->session;

	log_trace(TRACE_MFA, "filter: filter_end_query %s", filter_query_to_text(q));

	if (q->type == QT_EVENT)
		goto done;

	if (q->hook == QUERY_EOM) {
		if (s->error) {
			smtp_filter_response(s->id, QUERY_EOM, FILTER_FAIL, 0, NULL);
			free(q->smtp.response);
			goto done;
		}
		else if (q->u.datalen != s->datain) {
			log_warnx("filter: datalen mismatch on session %" PRIx64
			    ": %zu/%zu", s->id, s->datain, q->u.datalen);
			smtp_filter_response(s->id, QUERY_EOM, FILTER_FAIL, 0, NULL);
			free(q->smtp.response);
			goto done;
		}
	}

	log_trace(TRACE_MFA,
	    "filter: query %016"PRIx64" done: "
	    "status=%s code=%d response=\"%s\"",
	    q->qid,
	    status_to_str(q->smtp.status),
	    q->smtp.code,
	    q->smtp.response);
	smtp_filter_response(s->id, q->hook, q->smtp.status, q->smtp.code,
	    q->smtp.response);
	free(q->smtp.response);

    done:
	TAILQ_REMOVE(&s->queries, q, entry);
	free(q);
}

static void
filter_imsg(struct mproc *p, struct imsg *imsg)
{
	struct filter_proc	*proc = p->data;
	struct filter_session	*s;
	struct filter_query	*q, *next;
	struct msg		 m;
	const char		*line;
	uint64_t		 qid;
	uint32_t		 datalen;
	int			 qhook, status, code;

	if (imsg == NULL) {
		log_warnx("warn: filter \"%s\" closed unexpectedly", p->name);
		fatalx("exiting");
	}

	log_trace(TRACE_MFA, "filter: imsg %s from procfilter %s",
	    filterimsg_to_str(imsg->hdr.type),
	    filter_proc_to_text(proc));

	switch (imsg->hdr.type) {

	case IMSG_FILTER_REGISTER:
		if (proc->ready) {
			log_warnx("warn: filter \"%s\" already registered",
			    proc->mproc.name);
			exit(1);
		}
		
		m_msg(&m, imsg);
		m_get_int(&m, &proc->hooks);
		m_get_int(&m, &proc->flags);
		m_end(&m);
		proc->ready = 1;

		log_debug("debug: filter \"%s\": hooks 0x%08x flags 0x%04x",
		    proc->mproc.name, proc->hooks, proc->flags);

		TAILQ_FOREACH(proc, &procs, entry)
			if (!proc->ready)
				return;

		smtp_configure();
		break;

	case IMSG_FILTER_RESPONSE:
		m_msg(&m, imsg);
		m_get_id(&m, &qid);
		m_get_int(&m, &qhook);
		if (qhook == QUERY_EOM)
			m_get_u32(&m, &datalen);
		m_get_int(&m, &status);
		m_get_int(&m, &code);
		if (m_is_eom(&m))
			line = NULL;
		else
			m_get_string(&m, &line);
		m_end(&m);

		q = tree_xpop(&queries, qid);
		if (q->hook != qhook) {
			log_warnx("warn: filter: hook mismatch %d != %d", q->hook, qhook);
			fatalx("exiting");
		}
		q->smtp.status = status;
		if (code)
			q->smtp.code = code;
		if (line) {
			free(q->smtp.response);
			q->smtp.response = xstrdup(line, "filter_imsg");
		}
		q->state = (status == FILTER_OK) ? QUERY_READY : QUERY_DONE;
		if (qhook == QUERY_EOM)
			q->u.datalen = datalen;

		next = TAILQ_NEXT(q, entry);
		filter_drain_query(q);

		/*
		 * If there is another query after this one which is waiting,
		 * make it move forward.
		 */
		if (next && next->state == QUERY_WAITING)
			filter_drain_query(next);
		break;

	case IMSG_FILTER_PIPE_SETUP:
		m_msg(&m, imsg);
		m_get_id(&m, &qid);
		m_end(&m);

		s = tree_xget(&sessions, qid);
		s->fcurr = TAILQ_PREV(s->fcurr, filter_lst, entry);
		filter_set_fdout(s, imsg->fd);
		break;

	default:
		log_warnx("warn: bad imsg from filter %s", p->name);
		exit(1);
	}
}

static int
filter_tx(struct filter_session *s, int fdout)
{
	int			 sp[2];

	/* reset */
	s->datain = 0;
	s->datalen = 0;
	s->eom = NULL;
	s->error = 0;

	if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, sp) == -1) {
		log_warn("warn: filter: socketpair");
		return (-1);
	}

	if ((s->ofile = fdopen(fdout, "w")) == NULL) {
		log_warn("warn: filter: fdopen");
		close(sp[0]);
		close(sp[1]);
		return (-1);
	}

	iobuf_init(&s->iobuf, 0, 0);
	io_init(&s->io, sp[0], s, filter_tx_io, &s->iobuf);
	io_set_read(&s->io);

	return (sp[1]);
}

static void
filter_tx_io(struct io *io, int evt)
{
	struct filter_session	*s = io->arg;
	size_t			 len, n;
	char			*data;

	switch (evt) {
	case IO_DATAIN:
		data = iobuf_data(&s->iobuf);
		len = iobuf_len(&s->iobuf);
		log_debug("debug: filter: tx data (%zu) for req %016"PRIx64,
		    len, s->id);
		n = fwrite(data, 1, len, s->ofile);
		if (n != len) {
			log_warnx("warn: filter_tx_io: fwrite %zu/%zu", n, len);
			s->error = 1;
			break;
		}
		s->datain += n;
		iobuf_drop(&s->iobuf, n);
		iobuf_normalize(&s->iobuf);
		return;

	case IO_DISCONNECTED:
		log_debug("debug: filter: tx done for req %016"PRIx64, s->id);
		break;

	default:
		log_warn("warn: filter_tx_io: bad evt (%i) for req %016"PRIx64, evt, s->id);
		s->error = 1;
		break;
	}

	io_clear(&s->io);
	iobuf_clear(&s->iobuf);
	fclose(s->ofile);
	s->ofile = NULL;

	/* deferred eom request */
	if (s->eom) {
		log_debug("filter: running eom query...");
		filter_end_query(s->eom);
	} else {
		log_debug("filter: eom not received yet");
	}
}

static const char *
filter_query_to_text(struct filter_query *q)
{
	static char buf[1024];
	char tmp[1024];

	tmp[0] = '\0';

	if (q->type == QT_QUERY) {
		switch(q->hook) {
		case QUERY_CONNECT:
			strlcat(tmp, "=", sizeof tmp);
			strlcat(tmp, ss_to_text(&q->u.connect.local), sizeof tmp);
			strlcat(tmp, " <-> ", sizeof tmp);
			strlcat(tmp, ss_to_text(&q->u.connect.remote), sizeof tmp);
			strlcat(tmp, "(", sizeof tmp);
			strlcat(tmp, q->u.connect.hostname, sizeof tmp);
			strlcat(tmp, ")", sizeof tmp);
			break;
		case QUERY_MAIL:
		case QUERY_RCPT:
			snprintf(tmp, sizeof tmp, "=%s@%s",
			    q->u.maddr.user, q->u.maddr.domain);
			break;
		case QUERY_HELO:
			snprintf(tmp, sizeof tmp, "=%s", q->u.line);
			break;
		default:
			break;
		}
		snprintf(buf, sizeof buf, "%016"PRIx64"[%s,%s%s,%s]",
		    q->qid, type_to_str(q->type), query_to_str(q->hook), tmp,
		    filter_session_to_text(q->session));
	}
	else {
		snprintf(buf, sizeof buf, "%016"PRIx64"[%s,%s%s,%s]",
		    q->qid, type_to_str(q->type), event_to_str(q->hook), tmp,
		    filter_session_to_text(q->session));
	}

	return (buf);
}

static const char *
filter_session_to_text(struct filter_session *s)
{
	static char buf[1024];

	if (s == NULL)
		return "filter_session@NULL";

	snprintf(buf, sizeof(buf), "filter_session@%p[datalen=%zu,eom=%p,ofile=%p]",
	    s, s->datalen, s->eom, s->ofile);

	return buf;
}

static const char *
filter_to_text(struct filter *f)
{
	static char buf[1024];

	snprintf(buf, sizeof buf, "filter:%s", filter_proc_to_text(f->proc));

	return (buf);
}

static const char *
filter_proc_to_text(struct filter_proc *proc)
{
	static char buf[1024];

	snprintf(buf, sizeof buf, "%s[hooks=0x%08x,flags=0x%04x]",
	    proc->mproc.name, proc->hooks, proc->flags);

	return (buf);
}

#define CASE(x) case x : return #x

static const char *
filterimsg_to_str(int imsg)
{
	switch (imsg) {
	CASE(IMSG_FILTER_REGISTER);
	CASE(IMSG_FILTER_EVENT);
	CASE(IMSG_FILTER_QUERY);
	CASE(IMSG_FILTER_PIPE_SETUP);
	CASE(IMSG_FILTER_PIPE_ABORT);
	CASE(IMSG_FILTER_NOTIFY);
	CASE(IMSG_FILTER_RESPONSE);
	default:
		return "IMSG_FILTER_???";
	}
}

static const char *
query_to_str(int query)
{
	switch (query) {
	CASE(QUERY_CONNECT);
	CASE(QUERY_HELO);
	CASE(QUERY_MAIL);
	CASE(QUERY_RCPT);
	CASE(QUERY_DATA);
	CASE(QUERY_EOM);
	CASE(QUERY_DATALINE);
	default:
		return "QUERY_???";
	}
}

static const char *
event_to_str(int event)
{
	switch (event) {
	CASE(EVENT_CONNECT);
	CASE(EVENT_RESET);
	CASE(EVENT_DISCONNECT);
	CASE(EVENT_COMMIT);
	CASE(EVENT_ROLLBACK);
	default:
		return "EVENT_???";
	}
}

static const char *
type_to_str(int type)
{
	switch (type) {
	CASE(QT_QUERY);
	CASE(QT_EVENT);
	default:
		return "QT_???";
	}
}

static const char *
status_to_str(int status)
{
	switch (status) {
	CASE(FILTER_OK);
	CASE(FILTER_FAIL);
	CASE(FILTER_CLOSE);
	default:
		return "FILTER_???";
	}
}