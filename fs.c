#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <ixp.h>

#include "m9u.h"

/* 
0600 /ctl
	add <file>
	queue <file>
	stop <n>
	skip <n>
	play
0400 /list
	list of filenames in playlist, one per line
0400 /queue
	list of songs queued up
0400 /event
	Play <file>
	Stop
	Select <file>
*/

extern char current_song[];
extern int player_pid;
extern Playlist playlist;
extern Queue *queue;

typedef enum {QNONE=-1, QROOT=0, QCTL, QLIST, QQUEUE, QEVENT, QMAX} qpath;

struct {char *name; qpath parent; int type; int mode;} files[QMAX] = {
	{"", QNONE, P9_QTDIR, 0500|P9_DMDIR},
	{"ctl", QROOT, P9_QTFILE, 0200},
	{"list", QROOT, P9_QTFILE, 0400},
	{"queue", QROOT, P9_QTFILE, 0400},
	{"event", QROOT, P9_QTFILE, 0400}
};

IxpFid **evfids = NULL;
int nevfids = 0, maxevfids = 0;

typedef struct Event {
	char *event;
	int refcount;
	struct Event *next;
} Event;

typedef struct Fidaux {
	char *contents;
	int size;
	Event *events;
	int evoffset;
	Ixp9Req *blocked;
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
	/* TODO drop add/queue from ctl, just append to /list or /queue instead */
	if(len > 4 && strncmp(line, "add ", 4) == 0){
		if(!(song = dupsong(line+4, len-4))){
			return "out of memory";
		}
		add(song);
	} else if(len > 6 && strncmp(line, "queue ", 6) == 0){
		if(!(song = dupsong(line+6, len-6))){
			return "out of memory";
		}
		enqueue(song);
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
	out->contents = NULL;
	out->size = 0;
	out->events = NULL;
	out->evoffset = 0;
	out->blocked = NULL;
	if(buflen && !(out->contents = malloc(buflen))) {
		free(out);
		return NULL;
	}
	return out;
}

int
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
	ev = fidaux->events;
	event = fidaux->events->event + fidaux->evoffset;
	len = strlen(event);
	if((n = dispatch(r, event, len)) >= len) {
		fidaux->events = ev->next;
		if(--ev->refcount == 0) {
			free(ev->event);
			free(ev);
		}
		fidaux->evoffset = 0;
	} else {
		fidaux->evoffset += n;
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
		for(pev = &fidaux->events; *pev; pev = &(*pev)->next);
		*pev = ev;
		if(fidaux->blocked) {
			evrespond(fidaux->blocked);
			fidaux->blocked = NULL;
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
		for(i=0; i<nevfids; ++i) {
			fidaux = (Fidaux*)evfids[i]->aux;
			for(pev = &fidaux->events; *pev; pev = &(*pev)->next);
			*pev = ev;
			if(fidaux->blocked) {
				evrespond(fidaux->blocked);
				fidaux->blocked = NULL;
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
	for(i=0; i<r->ifcall.nwname; ++i){
		for(j=0; j<QMAX; ++j){
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
	/* TODO spawn thread for /event? */
	Fidaux *fidaux = NULL;
	char *cp;

	switch(r->fid->qid.path) {
		case QLIST: {
			int i;

			if(!(fidaux = newfidaux(playlist.buflen))) {
				respond(r, "out of memory");
				return;
			}
			fidaux->size = playlist.buflen;
			cp = fidaux->contents;
			for(i=0; i<playlist.nsongs; ++i) {
				cp += sprintf(cp, "%s\n", playlist.songs[i]);
			}
			r->fid->aux = fidaux;
			break;
		}
		case QQUEUE: {
			Queue *qn;
			int buflen;
			
			buflen = 0;
			for(qn=queue; qn; qn=qn->next) {
				buflen += strlen(qn->song)+1;
			}
			if(!(fidaux = newfidaux(buflen))) {
				respond(r, "out of memory");
				return;
			}
			fidaux->size = buflen;
			cp = fidaux->contents;
			for(qn=queue; qn; qn=qn->next) {
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
			if(player_pid == -1) {
				sendevent("Stop", r->fid);
			} else {
				char buf[512];
				snprintf(buf, sizeof(buf), "Play %s", current_song);
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
	/* TODO kill /event thread */
	Fidaux *fidaux;
	int i;
	if((fidaux=(Fidaux*)r->fid->aux)) {
		if(fidaux->contents) {
			free(fidaux->contents);
			fidaux->size = 0;
		} else {
			for(i = 0; i < nevfids; ++i) {
				if(r->fid == evfids[i]) {
					if(i+1 < nevfids) {
						memcpy(evfids+i, evfids+i+1, (nevfids-1-i)*sizeof(IxpFid*));
					}
					--nevfids;
					break;
				}
			}
			if(fidaux->events) {
				Event *ev, *next;
				for(ev = fidaux->events; ev; ev = next) {
					next = ev->next;
					if(--ev->refcount == 0) {
						free(ev->event);
						free(ev);
					}
				}
			}
		}
		free(fidaux);
		r->fid->aux = NULL;
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
		/* hack! */
		if(r->ifcall.offset > 0) {
			respond(r, NULL);
			return;
		}
		for(i=0; i<QMAX; ++i){
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
		if(fidaux->contents && r->ifcall.offset < fidaux->size) {
			if(r->ifcall.offset + r->ifcall.count > fidaux->size) {
				r->ofcall.count = fidaux->size - r->ifcall.offset;
			} else {
				r->ofcall.count = r->ifcall.count;
			}
			if(!(r->ofcall.data = malloc(r->ofcall.count))) {
				r->ofcall.count = 0;
				respond(r, "out of memory");
				return;
			}
			memcpy(r->ofcall.data, fidaux->contents+r->ifcall.offset, r->ofcall.count);
			respond(r, NULL);
			return;
		} else if(!fidaux->contents) {
			if(fidaux->events) {
				evrespond(r);
			} else {
				/* there's no pending event, so we don't respond(), which leaves the client blocked */
				if(fidaux->blocked) {
					/* ... unless this fid is already blocked */
					respond(r, "fid already blocked on /event");
				} else {
					fidaux->blocked = r;
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
	switch(r->fid->qid.path){
		case QCTL:
			/* TODO allow multiple messages in a single write, one per line */
			/* TODO return proper error string on failure */
			ctlparse(r->ifcall.data, r->ifcall.count);
			r->ofcall.count = r->ifcall.count;
			break;
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
