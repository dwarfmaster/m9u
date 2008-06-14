typedef struct Playlist_ Playlist;
typedef struct Queue_ Queue;

struct Playlist_
{
	char **songs;
	int nsongs;
	int maxn;
	int buflen;
};

struct Queue_
{
	char *song;
	Queue *next;
};

extern int plinit(Playlist*);
extern int plcontains(Playlist*, char*);
extern int pladd(Playlist*, char*);

extern int init();
extern void skip();
extern void stop();
extern int add(char*);
extern char* enqueue(char*);
extern char* play(char*);
extern void songends();

extern void postevent(char*);
