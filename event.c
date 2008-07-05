#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <ixp.h>

#include "m9u.h"

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
newevent(char *fmt, va_list ap)
{
	Event *ev;
	int len;
	if((ev = malloc(sizeof(Event)))) {
		len = vsnprintf(NULL, 0, fmt, ap) + 1;
		if((ev->event = malloc(len+2))) {
			vsnprintf(ev->event, len+1, fmt, ap);
			ev->event[len+1-1] = '\n';
			ev->event[len+2-1] = '\0';
			ev->next = NULL;
			return ev;
		} else {
			free(ev);
		}
	}
	fprintf(stderr, "m9u: couldn't alloc %d+%d bytes for event!\n", sizeof(Event), len+2);
	return NULL;
}

/* fid == NULL  =>  broadcast to all reading fids */
void
putevent(IxpFid *fid, char *fmt, ...)
{
	Event *ev, **pev;
	Fidaux *fidaux;
	int i;
	va_list ap;
	if(!fid && nevfids == 0)
		return;

	va_start(ap, fmt);
	ev = newevent(fmt, ap);
	va_end(ap);
	if(!ev)
		return;

	for(i = 0; i < nevfids; ++i) {
		if(fid && evfids[i] != fid)
			continue;

		++ev->refcount;
		fidaux = (Fidaux*)evfids[i]->aux;
		for(pev = &fidaux->rd.ev.list; *pev; pev = &(*pev)->next);
		*pev = ev;
		if(fidaux->rd.ev.blocked) {
			evrespond(fidaux->rd.ev.blocked);
			fidaux->rd.ev.blocked = NULL;
		}
	}
}
