#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <sys/types.h>
#include <signal.h>

#include "m9u.h"

char *player_cmd = "m9uplay";

char current_song[512] = {0};
int player_pid = -1;
Queue *queue = NULL;
Playlist playlist;

int
init()
{
	return plinit(&playlist);
}

void
stop()
{
	if(player_pid == -1)
		return;
	postevent("Stop");
	kill(player_pid, SIGTERM);
	player_pid = -1;
	current_song[0] = '\0';
}

char*
play(char *song)
{
	char buf[512];
	if(player_pid != -1){
		return "already playing";
	}
	if(song == NULL){
		player_pid = 0;
		songends();
		return NULL;
	}

	current_song[0] = '\0';
	switch(player_pid=fork()){
		case -1:
			return "fork failed";
		case 0:
			execlp(player_cmd, player_cmd, song, NULL);
			fprintf(stderr, "error execing %s: %s\n", player_cmd, strerror(errno));
			exit(1);
		default:
			snprintf(current_song, sizeof(current_song), "%s", song);
			snprintf(buf, sizeof(buf), "Play %s", song);
			postevent(buf);
	}
	return NULL;
}

void
skip()
{
	if(player_pid == -1)
		return;
	kill(player_pid, SIGTERM);
}

int
add(char *song)
{
	return pladd(&playlist, song);
}

char*
enqueue(char *song)
{
	Queue **node, *new;
	for(node=&queue; *node; node=&(*node)->next);

	if(!(new = malloc(sizeof(Queue))))
		return "out of memory";
	new->song = song;
	new->next = NULL;
	*node = new;
	return NULL;
}

void
songends()
{
	Queue *node;
	if(player_pid == -1){
		/* playback is stopped - don't restart it */
		return;
	}
	/* otherwise our player just died */
	player_pid = -1;
	if(queue){
		node = queue;
		queue = queue->next;
		play(node->song);
		free(node);
	}else if(playlist.nsongs > 0){
		/* TODO proper shuffled list */
		play(playlist.songs[rand()%playlist.nsongs]);
	}
}
