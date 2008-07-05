#include <stdlib.h>
#include <string.h>

#include "m9u.h"

int
plinit(Playlist *pl)
{
	pl->nsongs = 0;
	pl->maxn = 32;
	pl->current = 0;
	if(!(pl->songs = malloc(sizeof(char*)*pl->maxn)))
		return -1;
	return 0;
}

int
pladd(Playlist *pl, char *song)
{
	if(pl->nsongs + 1 > pl->maxn){
		char ** new;
		if(!(new = realloc(pl->songs, sizeof(char*)*pl->maxn*2))){
			return -1;
		}
		pl->songs = new;
		pl->maxn *= 2;
	}
	pl->songs[pl->nsongs++] = song;
	return 0;
}
