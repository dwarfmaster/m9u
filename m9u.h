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
typedef struct Event_ Event;
typedef struct Fidaux_ Fidaux;

struct Fileinfo_
{
	char *name;
	qpath parent;
	int type;
	int mode;
	unsigned int size;
};

struct Event_
{
	char *event;
	int refcount;
	Event *next;
};

struct Fidaux_
{
	enum {BUF, EVENT} rdtype;
	union {
		struct {
			char *data;
			int size;
			int max;
		} buf;

		struct {
			Event *list;
			int offset;
			Ixp9Req *blocked;
		} ev;
	} rd;

	char *pre;
	int prelen;

	int appendoffset;
};

extern Fileinfo files[];
extern IxpFid **evfids;
extern int nevfids;

extern int plinit(Playlist*);
extern int pladd(Playlist*, char*);
extern void plclear(Playlist*);

extern int init();
extern void skip(int n);
extern void stop();
extern void add(char*);
extern void clear();
extern void enqueue(char*);
extern char* play(char*);
extern void songends();
extern int current();

extern void putevent(IxpFid*, char*, ...);
extern void evrespond(Ixp9Req *r);
