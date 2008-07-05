typedef struct Playlist_ Playlist;
typedef struct Queue_ Queue;

struct Playlist_
{
	char **songs;
	int nsongs;
	int maxn;
	int current; /* current position in playlist */
};

struct Queue_
{
	char *song;
	Queue *next;
};

extern char playing_song[];
extern Playlist playlist;
extern Queue *queue;

typedef enum {QNONE=-1, QROOT=0, QCTL, QLIST, QQUEUE, QEVENT, QMAX} qpath;
typedef struct Fileinfo_ Fileinfo;

struct Fileinfo_
{
	char *name;
	qpath parent;
	int type;
	int mode;
	unsigned int size;
};

extern Fileinfo files[];

extern int plinit(Playlist*);
extern int pladd(Playlist*, char*);

extern int init();
extern void skip();
extern void stop();
extern void add(char*);
extern void enqueue(char*);
extern char* play(char*);
extern void songends();

extern void postevent(char*);
