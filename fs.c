#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <ixp.h>

#include "m9u.h"

/* 
0600 /ctl
	add <file>
	stop <n>
	skip <n>
	play
0400 /list
	list of filenames in playlist, one per line
0600 /queue
	list of songs queued up
0400 /event
	Play <file>
	Stop
	Select <file>
*/

/* name, parent, type, mode, size */
Fileinfo files[QMAX] = {
	{"", QNONE, P9_QTDIR, 0500|P9_DMDIR, 0},
	{"ctl", QROOT, P9_QTFILE, 0200, 0},
	{"list", QROOT, P9_QTFILE, 0400, 0},
	{"queue", QROOT, P9_QTFILE, 0600|P9_OAPPEND, 0},
	{"event", QROOT, P9_QTFILE, 0400, 0}
};

IxpFid **evfids = NULL;
int nevfids = 0, maxevfids = 0;

typedef struct Event {
	char *event;
	int refcount;
	struct Event *next;
} Event;

typedef struct Fidaux {
	enum {BUF, EVENT} rdtype;
	union {
		struct {
			char *data;
			int size;
		} buf;

		struct {
			Event *list;
			int offset;
			Ixp9Req *blocked;
		} ev;
	} rd;

	char *pre;
	int prelen;
} Fidaux;

static char*
dupsong(char *buf, int len)
{
	/* buf is not NUL terminated */
	char *song, *cp;
	if(!(song = malloc(len+1))){
		return NULL;
	}
	memcpy(song, buf, len);
	cp = song+len;
	*cp = '\0';
	while(*--cp == '\n'){
		*cp = '\0';
	}
	return song;
}

char*
ctlparse(char *line, int len)
{
	char *song;
	/* line is not NUL terminated */
	/* TODO drop add from ctl, just append to /list instead */
	if(len > 4 && strncmp(line, "add ", 4) == 0){
		if(!(song = dupsong(line+4, len-4))){
			return "out of memory";
		}
		add(song);
	} else if(len >= 4 && strncmp(line, "stop", 4) == 0){
		/* TODO support stop-after-n-songs */
		stop();
	} else if(len >= 4 && strncmp(line, "skip", 4) == 0){
		/* TODO support skip-n-songs */
		/* NOTE: stop() then play(NULL) doesn't work because we don't handle the child death event until after this Twrite is finished with */
		skip();
	} else if(len >= 4 && strncmp(line, "play", 4) == 0){
		play(NULL);
	} else {
		return "syntax error";
	}
	return NULL;
}

Fidaux*
newfidaux(int buflen)
{
	Fidaux *out;
	if(!(out = malloc(sizeof(Fidaux)))) {
		return NULL;
	}
	if(buflen >= 0) {
		out->rdtype = BUF;
		out->rd.buf.data = NULL;
		out->rd.buf.size = buflen;
		if(buflen && !(out->rd.buf.data = malloc(buflen))) {
			free(out);
			return NULL;
		}
	} else {
		out->rdtype = EVENT;
		out->rd.ev.list = NULL;
		out->rd.ev.offset = 0;
		out->rd.ev.blocked = NULL;
	}
	out->pre = NULL;
	return out;
}

void
freefidaux(IxpFid *fid)
{
	int i;
	Fidaux *fidaux;
	fidaux = (Fidaux*)fid->aux;
	if(fidaux->rdtype == BUF) {
		if(fidaux->rd.buf.data) {
			free(fidaux->rd.buf.data);
		}
	} else {
		for(i = 0; i < nevfids; ++i) {
			if(fid == evfids[i]) {
				if(i+1 < nevfids) {
					memcpy(evfids+i, evfids+i+1, (nevfids-1-i)*sizeof(IxpFid*));
				}
				--nevfids;
				break;
			}
		}
		if(fidaux->rd.ev.list) {
			Event *ev, *next;
			for(ev = fidaux->rd.ev.list; ev; ev = next) {
				next = ev->next;
				if(--ev->refcount == 0) {
					free(ev->event);
					free(ev);
				}
			}
		}
	}
	if(fidaux->pre) {
		free(fidaux->pre);
	}
	free(fidaux);
	fid->aux = NULL;
}

static int
dispatch(Ixp9Req *r, char *event, int len)
{
	if(len > r->ifcall.count) {
		r->ofcall.count = r->ifcall.count;
	} else {
		r->ofcall.count = len;
	}
	if(!(r->ofcall.data = malloc(r->ofcall.count))) {
		r->ofcall.count = 0;
		respond(r, "out of memory");
		return 0;
	}
	memcpy(r->ofcall.data, event, r->ofcall.count);
	respond(r, NULL);
	return r->ofcall.count;
}

void
evrespond(Ixp9Req *r)
{
	Fidaux *fidaux;
	Event *ev;
	char *event;
	int len, n;

	fidaux = (Fidaux*)r->fid->aux;
	ev = fidaux->rd.ev.list;
	event = fidaux->rd.ev.list->event + fidaux->rd.ev.offset;
	len = strlen(event);
	if((n = dispatch(r, event, len)) >= len) {
		fidaux->rd.ev.list = ev->next;
		if(--ev->refcount == 0) {
			free(ev->event);
			free(ev);
		}
		fidaux->rd.ev.offset = 0;
	} else {
		fidaux->rd.ev.offset += n;
	}
}

Event*
newevent(char *event, IxpFid *fid)
{
	Event *ev;
	int len;
	if((ev = malloc(sizeof(Event)))) {
		len = strlen(event);
		if((ev->event = malloc(len+2))) {
			snprintf(ev->event, len+2, "%s\n", event);
			ev->refcount = (fid) ? 1:nevfids;
			ev->next = NULL;
			return ev;
		} else {
			free(ev);
		}
	}
	fprintf(stderr, "m9u: couldn't alloc %d+%d bytes for event!\n", sizeof(Event), len+2);
	return NULL;
}

/* TODO make send/postevent varadic */
void
sendevent(char *event, IxpFid *fid)
{
	Event *ev, **pev;
	Fidaux *fidaux;
	if((ev = newevent(event, fid))) {
		fidaux = (Fidaux*)fid->aux;
		for(pev = &fidaux->rd.ev.list; *pev; pev = &(*pev)->next);
		*pev = ev;
		if(fidaux->rd.ev.blocked) {
			evrespond(fidaux->rd.ev.blocked);
			fidaux->rd.ev.blocked = NULL;
		}
	}
}

void
postevent(char *event)
{
	Event *ev, **pev;
	Fidaux *fidaux;
	int i;
	if(nevfids == 0) {
		return; /* no one is listening... */
	}
	if((ev = newevent(event, NULL))) {	
		for(i = 0; i < nevfids; ++i) {
			fidaux = (Fidaux*)evfids[i]->aux;
			for(pev = &fidaux->rd.ev.list; *pev; pev = &(*pev)->next);
			*pev = ev;
			if(fidaux->rd.ev.blocked) {
				evrespond(fidaux->rd.ev.blocked);
				fidaux->rd.ev.blocked = NULL;
			}
		}
	}
}

void
fs_attach(Ixp9Req *r)
{
	r->fid->qid.type = files[QROOT].type;
	r->fid->qid.path = QROOT;
	r->ofcall.qid = r->fid->qid;
	respond(r, NULL);
}

void
fs_walk(Ixp9Req *r)
{
	char buf[512];
	qpath cwd;
	int i, j;

	cwd = r->fid->qid.path;
	r->ofcall.nwqid = 0;
	for(i = 0; i < r->ifcall.nwname; ++i){
		for(j = 0; j < QMAX; ++j){
			if(files[j].parent == cwd && strcmp(files[j].name, r->ifcall.wname[i]) == 0)
				break;
		}
		if(j >= QMAX){
			snprintf(buf, sizeof(buf), "%s: no such file or directory", r->ifcall.wname[i]);
			respond(r, buf);
			return;
		}
		r->ofcall.wqid[r->ofcall.nwqid].type = files[j].type;
		r->ofcall.wqid[r->ofcall.nwqid].path = j;
		r->ofcall.wqid[r->ofcall.nwqid].version = 0;
		++r->ofcall.nwqid;
	}
	respond(r, NULL);
}

void
fs_open(Ixp9Req *r)
{
	/* TODO permissions check */
	Fidaux *fidaux = NULL;
	char *cp;

	/* TODO most of this stuff only has to be done when opening for read */
	switch(r->fid->qid.path) {
		case QLIST: {
			int i;

			if(!(fidaux = newfidaux(files[QLIST].size))) {
				respond(r, "out of memory");
				return;
			}
			fidaux->rd.buf.size = files[QLIST].size;
			cp = fidaux->rd.buf.data;
			for(i = 0; i < playlist.nsongs; ++i) {
				cp += sprintf(cp, "%s\n", playlist.songs[i]);
			}
			r->fid->aux = fidaux;
			break;
		}
		case QQUEUE: {
			Queue *qn;

			if(!(fidaux = newfidaux(files[QQUEUE].size))) {
				respond(r, "out of memory");
				return;
			}
			fidaux->rd.buf.size = files[QQUEUE].size;
			cp = fidaux->rd.buf.data;
			for(qn = queue; qn; qn=qn->next) {
				cp += sprintf(cp, "%s\n", qn->song);
			}
			r->fid->aux = fidaux;
			break;
		}
		case QEVENT: {
			if(!(fidaux = newfidaux(0))) {
				respond(r, "out of memory");
				return;
			}
			r->fid->aux = fidaux;
			if(nevfids+1 > maxevfids) {
				IxpFid **new;
				int n;
				n = maxevfids ? maxevfids*2 : 8;
				if(!(new = realloc(evfids, n*sizeof(IxpFid*)))) {
					respond(r, "out of memory");
					return;
				}
				maxevfids = n;
				evfids = new;
			}
			evfids[nevfids++] = r->fid;
			if(*playing_song == '\0') {
				sendevent("Stop", r->fid);
			} else {
				char buf[512];
				snprintf(buf, sizeof(buf), "Play %s", playing_song);
				sendevent(buf, r->fid);
			}
			break;
		}
	}
	respond(r, NULL);
}

void
fs_clunk(Ixp9Req *r)
{
	switch(r->fid->qid.path) {
		case QQUEUE: {
			char *song;
			Fidaux *fidaux = (Fidaux*)r->fid->aux;
			if(fidaux->pre) {
				if((song = dupsong(fidaux->pre, fidaux->prelen))) {
					enqueue(song);
				}
				free(fidaux->pre);
				fidaux->pre = NULL;
			}
		}
	}
	if((r->fid->aux)) {
		freefidaux(r->fid);
	}
	respond(r, NULL);
}

void
dostat(IxpStat *st, int path)
{
	st->type = files[path].type;
	st->qid.type = files[path].type;
	st->qid.path = path;
	st->mode = files[path].mode;
	st->name = files[path].name;
	st->length = files[path].size;
	st->uid = st->gid = st->muid = "";
}

void
fs_stat(Ixp9Req *r)
{
	IxpStat st = {0};
	IxpMsg m;
	char buf[512];

	dostat(&st, r->fid->qid.path);
	m = ixp_message(buf, sizeof(buf), MsgPack);
	ixp_pstat(&m, &st);
	r->ofcall.nstat = ixp_sizeof_stat(&st);
	if(!(r->ofcall.stat = malloc(r->ofcall.nstat))) {
		r->ofcall.nstat = 0;
		respond(r, "out of memory");
		return;
	}
	memcpy(r->ofcall.stat, m.data, r->ofcall.nstat);
	respond(r, NULL);
}

void
fs_read(Ixp9Req *r)
{
	Fidaux *fidaux;
	if(files[r->fid->qid.path].type & P9_QTDIR){
		IxpStat st = {0};
		IxpMsg m;
		char buf[512];
		int i;

		m = ixp_message(buf, sizeof(buf), MsgPack);

		r->ofcall.count = 0;
		if(r->ifcall.offset > 0) {
			/* hack! assuming the whole directory fits in a single Rread */
			respond(r, NULL);
			return;
		}
		for(i = 0; i < QMAX; ++i){
			if(files[i].parent == r->fid->qid.path){
				dostat(&st, i);
				ixp_pstat(&m, &st);
				r->ofcall.count += ixp_sizeof_stat(&st);
			}
		}
		if(!(r->ofcall.data = malloc(r->ofcall.count))) {
			r->ofcall.count = 0;
			respond(r, "out of memory");
			return;
		}
		memcpy(r->ofcall.data, m.data, r->ofcall.count);
		respond(r, NULL);
		return;
	}

	if((fidaux = (Fidaux*)r->fid->aux)) {
		if(fidaux->rdtype == BUF && r->ifcall.offset < fidaux->rd.buf.size) {
			if(r->ifcall.offset + r->ifcall.count > fidaux->rd.buf.size) {
				r->ofcall.count = fidaux->rd.buf.size - r->ifcall.offset;
			} else {
				r->ofcall.count = r->ifcall.count;
			}
			if(!(r->ofcall.data = malloc(r->ofcall.count))) {
				r->ofcall.count = 0;
				respond(r, "out of memory");
				return;
			}
			memcpy(r->ofcall.data, fidaux->rd.buf.data+r->ifcall.offset, r->ofcall.count);
			respond(r, NULL);
			return;
		} else if(fidaux->rdtype == EVENT) {
			if(fidaux->rd.ev.list) {
				evrespond(r);
			} else {
				/* there's no pending event, so we don't respond(), which leaves the client blocked */
				if(fidaux->rd.ev.blocked) {
					/* ... unless this fid is already blocked */
					respond(r, "fid already blocked on /event");
				} else {
					fidaux->rd.ev.blocked = r;
				}	
			}
			return;
		}	
	}

	respond(r, NULL);
}

void
fs_write(Ixp9Req *r)
{
	Fidaux *fidaux;
	fidaux = (Fidaux*)r->fid->aux;
	switch(r->fid->qid.path){
		case QCTL:
			/* TODO allow multiple messages in a single write, one per line */
			/* TODO return proper error string on failure */
			ctlparse(r->ifcall.data, r->ifcall.count);
			r->ofcall.count = r->ifcall.count;
			break;
		case QQUEUE: {
			char *start, *end, *song;
			int len;
			start = r->ifcall.data;
			end = memchr(r->ifcall.data, '\n', r->ifcall.count);
			if(end == NULL) {
				end = start+r->ifcall.count;
			}
			if(fidaux->pre) {
				len = (end-start) + fidaux->prelen;
				if(!(song = malloc(len+1))) {
					respond(r, "out of memory");
					return;
				}
				memcpy(song, fidaux->pre, fidaux->prelen);
				memcpy(song+fidaux->prelen, start, end-start);
				song[len] = '\0';
				enqueue(song);
				free(fidaux->pre);
				fidaux->pre = NULL;
				fidaux->prelen = 0;
				start = end+1;
				end = memchr(start, '\n', r->ifcall.count - (start-r->ifcall.data));
			}
			while(end) {
				if(!(song = dupsong(start, end-start))) {
					respond(r, "out of memory");
					return;
				}
				enqueue(song);
				start = end+1;
				end = memchr(start, '\n', r->ifcall.count - (start-r->ifcall.data));
			}
			fidaux->prelen = r->ifcall.count - (start-r->ifcall.data);
			if(fidaux->prelen > 0) {
				if(!(fidaux->pre = malloc(fidaux->prelen))) {
					respond(r, "out of memory");
					return;
				}
				memcpy(fidaux->pre, start, fidaux->prelen);
			}
			r->ofcall.count = r->ifcall.count;
		}
	}
	respond(r, NULL);
}

Ixp9Srv p9srv = {
	.open=fs_open,
	.clunk=fs_clunk,
	.walk=fs_walk,
	.read=fs_read,
	.stat=fs_stat,
	.write=fs_write,
	.attach=fs_attach
};
