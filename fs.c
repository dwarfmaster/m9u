/* © 2008 sqweek <sqweek@gmail.com>
 * See COPYING for details.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <ixp.h>

#include "m9u.h"

/* 
0600 /ctl
	stop <n>
	skip <n>
	play
0600 /list
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
	{"list", QROOT, P9_QTFILE, 0600, 0},
	{"queue", QROOT, P9_QTFILE, 0600|P9_OAPPEND, 0},
	{"event", QROOT, P9_QTFILE, 0400, 0}
};

IxpFid **evfids = NULL;
int nevfids = 0, maxevfids = 0;

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
	/* line is not NUL terminated */
	if(len >= 4 && strncmp(line, "stop", 4) == 0){
		/* TODO support stop-after-n-songs */
		stop();
	} else if(len >= 4 && strncmp(line, "skip", 4) == 0){
		int n;
		if(len >= 6) {
			n = atoi(line+5);
		} else {
			n = 1;
		}
		skip(n);
	} else if(len >= 4 && strncmp(line, "play", 4) == 0){
		play(NULL);
	} else {
		return "syntax error";
	}
	return NULL;
}

Fidaux*
newbufaux(int buflen)
{
	Fidaux *out;
	if(!(out = malloc(sizeof(Fidaux)))) {
		return NULL;
	}
	out->rdtype = BUF;
	out->rd.buf.data = NULL;
	out->rd.buf.size = buflen;
	out->rd.buf.max = buflen;
	if(buflen && !(out->rd.buf.data = malloc(buflen))) {
		free(out);
		return NULL;
	}
	out->pre = NULL;
	out->prelen = 0;
	out->appendoffset = 0;
	return out;
}

Fidaux*
newevaux()
{
	Fidaux *out;
	if(!(out = malloc(sizeof(Fidaux)))) {
		return NULL;
	}
	out->rdtype = EVENT;
	out->rd.ev.list = NULL;
	out->rd.ev.offset = 0;
	out->rd.ev.blocked = NULL;
	out->pre = NULL;
	out->prelen = 0;
	out->appendoffset = 0;
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

void
fs_attach(Ixp9Req *r)
{
	r->fid->qid.type = files[QROOT].type;
	r->fid->qid.path = QROOT;
	r->ofcall.rattach.qid = r->fid->qid;
	respond(r, NULL);
}

void
fs_walk(Ixp9Req *r)
{
	char buf[512];
	qpath cwd;
	int i, j;

	cwd = r->fid->qid.path;
	r->ofcall.rwalk.nwqid = 0;
	for(i = 0; i < r->ifcall.twalk.nwname; ++i){
		for(j = 0; j < QMAX; ++j){
			if(files[j].parent == cwd && strcmp(files[j].name, r->ifcall.twalk.wname[i]) == 0)
				break;
		}
		if(j >= QMAX){
			snprintf(buf, sizeof(buf), "%s: no such file or directory", r->ifcall.twalk.wname[i]);
			respond(r, buf);
			return;
		}
		r->ofcall.rwalk.wqid[r->ofcall.rwalk.nwqid].type = files[j].type;
		r->ofcall.rwalk.wqid[r->ofcall.rwalk.nwqid].path = j;
		r->ofcall.rwalk.wqid[r->ofcall.rwalk.nwqid].version = 0;
		++r->ofcall.rwalk.nwqid;
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

			if(!(fidaux = newbufaux(files[QLIST].size))) {
				respond(r, "out of memory");
				return;
			}
			fidaux->appendoffset = fidaux->rd.buf.size;
			cp = fidaux->rd.buf.data;
			for(i = 0; i < playlist.nsongs; ++i) {
				cp += sprintf(cp, "%s\n", playlist.songs[i]);
			}
			r->fid->aux = fidaux;
			break;
		}
		case QQUEUE: {
			Queue *qn;

			if(!(fidaux = newbufaux(files[QQUEUE].size))) {
				respond(r, "out of memory");
				return;
			}
			cp = fidaux->rd.buf.data;
			for(qn = queue; qn; qn=qn->next) {
				cp += sprintf(cp, "%s\n", qn->song);
			}
			r->fid->aux = fidaux;
			break;
		}
		case QEVENT: {
			if(!(fidaux = newevaux())) {
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
				if(playlist.nsongs == 0) {
					putevent(r->fid, "Stop");
				} else {
					putevent(r->fid, "Stop %s", playlist.songs[playlist.current]);
				}
			} else {
				putevent(r->fid, "Play %s", playing_song);
			}
			break;
		}
	}
	respond(r, NULL);
}

void
fs_clunk(Ixp9Req *r)
{
	Fidaux *fidaux;
	fidaux = (Fidaux*)r->fid->aux;
	if (!fidaux) {
		respond(r, NULL);
		return;
	}
	switch(r->fid->qid.path) {
		case QQUEUE: {
			char *song;
			if(fidaux->pre) {
				if((song = dupsong(fidaux->pre, fidaux->prelen))) {
					enqueue(song);
				}
				free(fidaux->pre);
				fidaux->pre = NULL;
			}
			break;
		}
		case QLIST: {
			char *song;
			if(fidaux->pre) {
				if((song = dupsong(fidaux->pre, fidaux->prelen))) {
					add(song);
				}
				free(fidaux->pre);
				fidaux->pre = NULL;
			}
			if(fidaux->appendoffset == -1) {
				char *cp, *end, *eob;
				clear();
				eob = fidaux->rd.buf.data + fidaux->rd.buf.size;
				for(end=cp=fidaux->rd.buf.data; cp < eob; cp=end+1) {
					if(!(end = memchr(cp, '\n', eob-cp))) {
						end = eob;
					}
					if((song = dupsong(cp, end-cp))) {
						add(song);
					}
				}
			}
			break;
		}
	}
	freefidaux(r->fid);
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
	r->ofcall.rstat.nstat = ixp_sizeof_stat(&st);
	if(!(r->ofcall.rstat.stat = malloc(r->ofcall.rstat.nstat))) {
		r->ofcall.rstat.nstat = 0;
		respond(r, "out of memory");
		return;
	}
	memcpy(r->ofcall.rstat.stat, m.data, r->ofcall.rstat.nstat);
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

		r->ofcall.rread.count = 0;
		if(r->ifcall.tread.offset > 0) {
			/* hack! assuming the whole directory fits in a single Rread */
			respond(r, NULL);
			return;
		}
		for(i = 0; i < QMAX; ++i){
			if(files[i].parent == r->fid->qid.path){
				dostat(&st, i);
				ixp_pstat(&m, &st);
				r->ofcall.rread.count += ixp_sizeof_stat(&st);
			}
		}
		if(!(r->ofcall.rread.data = malloc(r->ofcall.rread.count))) {
			r->ofcall.rread.count = 0;
			respond(r, "out of memory");
			return;
		}
		memcpy(r->ofcall.rread.data, m.data, r->ofcall.rread.count);
		respond(r, NULL);
		return;
	}

	if((fidaux = (Fidaux*)r->fid->aux)) {
		if(fidaux->rdtype == BUF && r->ifcall.tread.offset < fidaux->rd.buf.size) {
			if(r->ifcall.tread.offset + r->ifcall.tread.count > fidaux->rd.buf.size) {
				r->ofcall.rread.count = fidaux->rd.buf.size - r->ifcall.tread.offset;
			} else {
				r->ofcall.rread.count = r->ifcall.tread.count;
			}
			if(!(r->ofcall.rread.data = malloc(r->ofcall.rread.count))) {
				r->ofcall.rread.count = 0;
				respond(r, "out of memory");
				return;
			}
			memcpy(r->ofcall.rread.data, fidaux->rd.buf.data+r->ifcall.tread.offset, r->ofcall.rread.count);
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

static char*
getln(char **line, Fidaux *fidaux, char **start, char *end)
{
	char *nl;
	int len;
	nl = memchr(*start, '\n', end-*start);
	if(!nl) {
		/* at the end of the buffer */
		char *new;
		*line = NULL;
		len = end-*start;
		if(len > 0) {
			if(!(new = realloc(fidaux->pre, len))) {
				free(fidaux->pre);
				fidaux->pre = NULL;
				fidaux->prelen = 0;
				return "out of memory";
			}
			memcpy(new, *start, len);
			fidaux->pre = new;
			fidaux->prelen = len;
		}
		*start = end+1;
	} else if(fidaux->pre) {
		/* at the start of the buffer */
		len = (nl-*start) + fidaux->prelen;
		if(!((*line) = malloc(len+1))) {
			return "out of memory";
		}
		memcpy((*line), fidaux->pre, fidaux->prelen);
		memcpy((*line)+fidaux->prelen, *start, nl-*start);
		(*line)[len] = '\0';
		free(fidaux->pre);
		fidaux->pre = NULL;
		fidaux->prelen = 0;
		*start += (nl-*start)+1;
	} else {
		len = nl-*start;
		if(!(*line = dupsong(*start, len))) {
			return "out of memory";
		}
		*start += len+1;
	}
	return NULL;
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
			ctlparse(r->ifcall.twrite.data, r->ifcall.twrite.count);
			r->ofcall.rwrite.count = r->ifcall.twrite.count;
			break;
		case QQUEUE: {
			char *start, *end, *song, *errstr;
			start = r->ifcall.twrite.data;
			end = start + r->ifcall.twrite.count;
			do {
				if((errstr = getln(&song, fidaux, &start, end))) {
					respond(r, errstr);
					return;
				}
				if(song)
					enqueue(song);
			} while (start < end);
			r->ofcall.rwrite.count = r->ifcall.twrite.count;
			break;
		}
		case QLIST: {
			if(r->ifcall.twrite.offset == fidaux->appendoffset) {
				char *start, *end, *song, *errstr;
				start = r->ifcall.twrite.data;
				end = start + r->ifcall.twrite.count;
				do {
					if((errstr = getln(&song, fidaux, &start, end))) {
						respond(r, errstr);
						return;
					}
					if(song)
						add(song);
				} while (start < end);
				fidaux->appendoffset += r->ifcall.twrite.count;
				r->ofcall.rwrite.count = r->ifcall.twrite.count;
			} else {
				fidaux->appendoffset = -1;
				/*if(!fidaux->rd.buf.data) {
					initlistbuf(fidaux, 1.2);
				}*/
				if(r->ifcall.twrite.offset + r->ifcall.twrite.count > fidaux->rd.buf.max) {
					int newmax;
					char *newbuf;

					newmax = fidaux->rd.buf.max * 2;
					if(!newmax) {
						newmax = 16384;
					}
					if((newbuf = realloc(fidaux->rd.buf.data, newmax))) {
						memset(newbuf+fidaux->rd.buf.max, '\0', newmax-fidaux->rd.buf.max);
						free(fidaux->rd.buf.data);
						fidaux->rd.buf.data = newbuf;
						fidaux->rd.buf.max = newmax;
					} else {
						respond(r, "out of memory");
						return;
					}
				}
				memcpy(fidaux->rd.buf.data + r->ifcall.twrite.offset, r->ifcall.twrite.data, r->ifcall.twrite.count);
				if(r->ifcall.twrite.offset + r->ifcall.twrite.count > fidaux->rd.buf.size) {
					fidaux->rd.buf.size = r->ifcall.twrite.offset + r->ifcall.twrite.count;
				}
				r->ofcall.rwrite.count = r->ifcall.twrite.count;
			}
			break;
		}
	}
	respond(r, NULL);
}

void
fs_wstat(Ixp9Req *r)
{
	respond(r, NULL); /* pretend it worked */
}

Ixp9Srv p9srv = {
	.open=fs_open,
	.clunk=fs_clunk,
	.walk=fs_walk,
	.read=fs_read,
	.stat=fs_stat,
	.write=fs_write,
	.wstat=fs_wstat,
	.attach=fs_attach
};
