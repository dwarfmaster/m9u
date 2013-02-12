/* © 2008 sqweek <sqweek@gmail.com>
 * See COPYING for details.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <sys/types.h>
#include <signal.h>

#include <ixp.h>

#include "m9u.h"

char *player_cmd = "m9play";

char playing_song[512] = {0};
int player_pid = -1;
Queue *queue = NULL;
Playlist playlist;

int
init(char *player)
{
	if(player) {
		player_cmd = player;
	}
	return plinit(&playlist);
}

void
stop()
{
	if(player_pid == -1)
		return;
	putevent(NULL, "Stop %s", playlist.songs[playlist.current]);
	kill(player_pid, SIGTERM);
	player_pid = -1;
	playing_song[0] = '\0';
}

char*
play(char *song)
{
	if(player_pid != -1){
		return "already playing";
	}
	if(song == NULL){
		song = playlist.songs[playlist.current];
	}

	playing_song[0] = '\0';
	switch(player_pid=fork()){
		case -1:
			return "fork failed";
		case 0:
			execlp(player_cmd, player_cmd, song, NULL);
			fprintf(stderr, "error execing %s: %s\n", player_cmd, strerror(errno));
			exit(1);
		default:
			snprintf(playing_song, sizeof(playing_song), "%s", song);
			putevent(NULL, "Play %s", playing_song);
	}
	return NULL;
}

void
skip(int n)
{
	/* XXX n is ignored if queue is not empty... so:
	 * a) there's no way to clear the queue in one message
	 * b) there's no way to move the current song while the queue is populated */
	if(!queue) {
		playlist.current += n;
	}
	if(player_pid != -1 && !queue) {
		/* songends() will advance playlist.current if the queue is empty, avoid that */
		--playlist.current;
	}
	if(playlist.current < 0) {
		playlist.current = playlist.nsongs + playlist.current;
	} else if(playlist.nsongs > 0) {
		playlist.current %= playlist.nsongs;
	}
	if(player_pid == -1) {
		putevent(NULL, "Stop %s", playlist.songs[playlist.current]);
		return;
	}
	kill(player_pid, SIGTERM);
}

void
add(char *song)
{
	if(pladd(&playlist, song) == 0) {
		files[QLIST].size += strlen(song) + 1;
	} else {
		fprintf(stderr, "m9u: %s: couldn't alloc memory to add song!\n", song);
	}
}

void
clear()
{
	plclear(&playlist);
	files[QLIST].size = 0;
}

void
enqueue(char *song)
{
	Queue **node, *new;
	for(node=&queue; *node; node=&(*node)->next);

	if(!(new = malloc(sizeof(Queue)))) {
		fprintf(stderr, "m9u: %s couldn't alloc memory to queue song!\n", song);
	}
	new->song = song;
	new->next = NULL;
	*node = new;
	files[QQUEUE].size += strlen(song) + 1;
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
		++playlist.current;
		playlist.current %= playlist.nsongs;
		play(playlist.songs[playlist.current]);
	}
}
