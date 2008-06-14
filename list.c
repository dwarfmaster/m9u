#include <stdlib.h>
#include <string.h>

#include "m9u.h"

/* Return the index a url belongs in the playlist */ 
static int
plpos(Playlist *pl, char *song)
{
	int i0, i1, i, d;
	i0 = 0;
	i1 = pl->nsongs-1;
	if(i1 < 0)
		return 0;
	else if(strcmp(song, pl->songs[i0]) < 0)
		return 0;
	else if(strcmp(song, pl->songs[i1]) > 0)
		return i1+1;

	while (i0 <= i1) {
		i = (i0+i1)/2;
		if((d = strcmp(song, pl->songs[i])) < 0)
			i1 = i-1;
		else if(d > 0)
			i0 = i+1;
		else if(d == 0)
			return i;
	}
	return i0;
}

int
plinit(Playlist *pl)
{
	pl->nsongs = 0;
	pl->maxn = 32;
	if(!(pl->songs = malloc(sizeof(char*)*pl->maxn)))
		return -1;
	return 0;
}

int
plcontains(Playlist *pl, char *song)
{
	int i;
	i = plpos(pl, song);
	return i < pl->nsongs && strcmp(song, pl->songs[i]) == 0;
}

int
pladd(Playlist *pl, char *song)
{
	int i;
	i = plpos(pl, song);
	if(i < pl->nsongs && strcmp(song, pl->songs[i]) == 0)
		return 0; /* already in playlist */

	if(pl->nsongs + 1 > pl->maxn){
		char ** new;
		if(!(new = realloc(pl->songs, sizeof(char*)*pl->maxn*2))){
			return -1;
		}
		pl->songs = new;
		pl->maxn *= 2;
	}

	if(i < pl->nsongs)
		memmove(pl->songs+i+1, pl->songs+i, sizeof(char*)*(pl->nsongs-i));
	pl->songs[i] = song;
	++pl->nsongs;
	pl->buflen += strlen(song)+1;
	return 0;
}
