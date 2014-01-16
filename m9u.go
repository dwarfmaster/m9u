package main

import (
	"code.google.com/p/go9p/p"
	"code.google.com/p/go9p/p/srv"
	"strings"
	"errors"
	"bytes"
	"time"
	"flag"
	"fmt"
	"net"
	"os"
	"os/exec"
	"os/signal"
	"syscall"
	"strconv"
	"unicode/utf8"
)

type M9Player struct {
	playlist []string
	position int
	queue []string

	player *M9Play
	song *string

	spawn chan string
}

type M9Play struct {
	Song string
	proc *os.Process
	killed bool
	reaped bool
}

func (player *M9Play) Kill() {
	if player.killed {
		return
	}
	player.killed = true
	go func() {
		/* we want to make sure the process dies, but give it a chance
		 * to go to its death gracefully before pulling out SIGKILL */
		for i := 0; i < 5; i++ {
			player.proc.Signal(os.Interrupt)
			time.Sleep(25 * time.Millisecond)
			if player.reaped {
				return
			}
		}
		player.proc.Kill()
	}()
}

func (player *M9Play) Reap(m9 *M9Player) {
	player.proc.Wait()
	player.reaped = true
	if player.killed {
		return
	}
	/* player terminated normally, kickoff next song */
	if len(m9.queue) == 0 && len(m9.playlist) > 0 {
		m9.position = (m9.position + 1) % len(m9.playlist)
	}
	m9.Play("")
}

var m9 *M9Player

func NewM9Player() *M9Player {
	m9 := M9Player{spawn: make(chan string, 15)}
	go m9.spawner()
	return &m9
}

func (m9 *M9Player) _spawn(song string) *M9Play {
	path, err := exec.LookPath("m9play")
	if err != nil {
		fmt.Printf("couldn't find m9play: %s\n", err)
		return nil
	}
	proc, err := os.StartProcess(path, []string{"m9play", song}, new(os.ProcAttr))
	if err != nil {
		fmt.Printf("couldn't spawn player: %s\n", err)
		return nil
	}
	return &M9Play{song, proc, false, false}
}

func (m9 *M9Player) spawner() {
	for {
		song := <-m9.spawn
		if m9.player != nil {
			/* already playing; stop the current player first */
			m9.player.Kill()
		}
		player := m9._spawn(song)
		if player == nil {
			m9.player = nil
			events <- m9.state()
			continue
		}
		go player.Reap(m9)
		m9.player = player
		events <- "Play " + song
	}
}

/* instantaneous state */
func (m9 *M9Player) state() string {
	player := m9.player
	if player != nil {
		return "Play " + player.Song
	} else {
		if len(m9.queue) > 0 {
			return "Stop " + m9.queue[0]
		} else if len(m9.playlist) > 0 {
			return "Stop " + m9.playlist[m9.position]
		}
		return "Stop"
	}
}

func (m9 *M9Player) Add(song string) {
	m9.playlist = append(m9.playlist, song)
	if m9.player == nil && len(m9.queue) == 0 && len(m9.playlist) == 1 {
		/* We are stopped, the queue is empty and this was the first song added to the list.
		 * Update /event to reflect the correct next-song */
		events <- m9.state()
	}
}

func (m9 *M9Player) Clear() {
	m9.playlist = make([]string, 0)
}

func (m9 *M9Player) Enqueue(song string) {
	m9.queue = append(m9.queue, song)
	if m9.player != nil && len(m9.queue) == 1 {
		/* We are stopped, and this was the first item added to the queue.
		 * Update /event to show the correct next-song */
		events <- m9.state()
	}
}

func (m9 *M9Player) Play(song string) {
	if song != "" {
		m9.spawn <- song
	} else {
		if len(m9.queue) > 0 {
			m9.spawn <- m9.queue[0]
			m9.queue = m9.queue[1:]
		} else if len(m9.playlist) > 0 {
			m9.spawn <- m9.playlist[m9.position]
		}
	}
}

func (m9 *M9Player) Skip(n int) {
	if len(m9.playlist) == 0 {
		return
	}
	m9.position += n
	m9.position %= len(m9.playlist)
	if m9.position < 0 {
		m9.position += len(m9.playlist)
	}
	if m9.player != nil {
		m9.Play("")
	} else {
		events <- m9.state()
	}
}

func (m9 *M9Player) Stop() {
	player := m9.player
	m9.player = nil
	if player != nil {
		player.Kill()
	}
	events <- m9.state()
}

func skip(amount string) error {
	if amount == "" {
		m9.Skip(1)
		return nil
	}
	i, err := strconv.Atoi(amount)
	if err != nil {
		return err
	}
	m9.Skip(i)
	return nil
}


var events chan string
var register chan chan string

func eventer() {
	listeners := make([]chan string, 0)
	for {
		select {
		case ev := <- events:
			for i := range(listeners) {
				listeners[i] <- ev
			}
			listeners = make([]chan string, 0)

		case l := <- register:
			listeners = append(listeners, l)
		}
	}
}

func waitForEvent() string {
	c := make(chan string)
	register <- c
	ev := <- c
	return ev
}

var ntype = flag.String("net", "unix", "network type")
var addr = flag.String("addr", "/tmp/ns.sqweek.:0/m9u", "network address")

type CtlFile struct {
	srv.File
}
type SongListFile struct {
	srv.File
	rd map[*srv.Fid] []byte
	wr map[*srv.Fid] *PartialLine
	SongAdded func(string)
}
type ListFile struct {
	SongListFile
}
type QueueFile struct {
	SongListFile
}
type EventFile struct {
	srv.File
}

func (*CtlFile) Write(fid *srv.FFid, b []byte, offset uint64) (n int, err error) {
	cmd := string(b)
	if strings.HasPrefix(cmd, "play") {
		m9.Play(strings.Trim(cmd[4:], " \n"))
	} else if strings.HasPrefix(cmd, "skip") {
		err = skip(strings.Trim(cmd[4:], " \n"))
	} else if strings.HasPrefix(cmd, "stop") {
		m9.Stop()
	} else {
		err = errors.New("ill-formed control message")
	}
	if err == nil {
		n = len(b)
	}
	return n, err
}

func (*CtlFile) Wstat(fid *srv.FFid, d *p.Dir) error {
	return nil
}

func mkbuf(lst []string) []byte {
	buflen := 0
	for i := range(lst) {
		buflen += 1 + len([]byte(lst[i]))
	}
	buf := make([]byte, buflen)
	j := 0
	for i := range(lst) {
		j += copy(buf[j:], lst[i])
		buf[j] = '\n'
		j++
	}
	return buf
}

type PartialLine struct {
	leftover []byte
}

func (part *PartialLine) append(bytes []byte) string {
	if part.leftover == nil {
		return string(bytes)
	}
	left := part.leftover
	part.leftover = nil
	return string(left) + string(bytes)
}

func (lstfile *ListFile) Open(fid *srv.FFid, mode uint8) error {
	if mode & 3 == p.OWRITE || mode & 3 == p.ORDWR {
		lstfile.wr[fid.Fid] = new(PartialLine)
		if mode & p.OTRUNC != 0 {
			m9.Clear()
		}
	}
	if mode & 3 == p.OREAD || mode & 3 == p.ORDWR {
		lstfile.rd[fid.Fid] = mkbuf(m9.playlist)
	}
	return nil
}

func (slf *SongListFile) Write(fid *srv.FFid, b []byte, offset uint64) (int, error) {
	prefix, ok := slf.wr[fid.Fid]
	if !ok {
		return 0, errors.New("internal state corrupted")
	}
	i := 0
	for {
		j := bytes.IndexByte(b[i:], '\n')
		if j == -1 {
			break
		}
		song := prefix.append(b[i:i+j])
		slf.SongAdded(song)
		//m9.Add(song)
		i += j+1
	}
	if i < len(b) {
		prefix.leftover = b[i:]
	}
	return len(b), nil
}

func (slf *SongListFile) Wstat(fid *srv.FFid, d *p.Dir) error {
	return nil
}



func min(a uint64, b uint64) uint64 {
	if a < b {
		return a
	}
	return b
}

func (slf *SongListFile) Read(fid *srv.FFid, b []byte, offset uint64) (int, error) {
	buf, ok := slf.rd[fid.Fid]
	if !ok {
		return 0, errors.New("internal state corrupted")
	}
	remaining := uint64(len(buf)) - offset
	n := min(remaining, uint64(len(b)))
	copy(b, buf[offset:offset + n])
	return int(n), nil
}

func (slf *SongListFile) Clunk(fid *srv.FFid) error {
	delete(slf.rd, fid.Fid)
	delete(slf.wr, fid.Fid)
	return nil
}


func (qf *QueueFile) Open(fid *srv.FFid, mode uint8) error {
	if mode & 3 == p.OWRITE || mode & 3 == p.ORDWR {
		qf.wr[fid.Fid] = new(PartialLine)
	}
	if mode & 3 == p.OREAD || mode & 3 == p.ORDWR {
		qf.rd[fid.Fid] = mkbuf(m9.queue)
	}
	return nil
}


func (*EventFile) Read(fid *srv.FFid, b []byte, offset uint64) (int, error) {
	var ev string
	if offset == 0 {
		ev = m9.state()
	} else {
		ev = waitForEvent()
	}
	buf := []byte(ev)
	for len(buf) > len(b) - 1 {
		_, size := utf8.DecodeLastRune(buf)
		buf = buf[:len(buf)-size]
	}
	copy(b[:len(buf)], buf)
	b[len(buf)] = byte('\n')
	return len(buf)+1, nil
}

func (*EventFile) Wstat(fid *srv.FFid, dir *p.Dir) error {
	return nil
}

func (slf *SongListFile) init(f func(string)) {
	slf.wr = make(map[*srv.Fid]*PartialLine)
	slf.rd = make(map[*srv.Fid] []byte)
	slf.SongAdded = f
}

func main() {
	var err error
	flag.Parse()

	uid := p.OsUsers.Uid2User(os.Geteuid())
	gid := p.OsUsers.Gid2Group(os.Getegid())
	fmt.Printf("uid = %d  gid = %d\n", os.Geteuid(), os.Getegid())

	events = make(chan string)
	register = make(chan chan string)

	m9 = NewM9Player()

	go eventer()

	root := new(srv.File)
	root.Add(nil, "/", uid, gid, p.DMDIR|0555, nil)
	ctl := new(CtlFile)
	ctl.Add(root, "ctl", uid, gid, 0644, ctl)
	list := new(ListFile)
	list.init(func(song string) {m9.Add(song)})
	list.Add(root, "list", uid, gid, 0644, list)
	queue := new(QueueFile)
	queue.init(func(song string) {m9.Enqueue(song)})
	queue.Add(root, "queue", uid, gid, 0644, queue)
	event := new(EventFile)
	event.Add(root, "event", uid, gid, 0444, event)

	s := srv.NewFileSrv(root)

	listener, err := net.Listen(*ntype, *addr)
	if err != nil {
		fmt.Printf("listen failed: %s\n", err)
		os.Exit(1)
	}
	defer listener.Close()
	sigchan := make(chan os.Signal)
	go func() {
		<- sigchan
		listener.Close()
		os.Exit(1)
	}()
	signal.Notify(sigchan, os.Interrupt, syscall.SIGTERM, syscall.SIGQUIT)

	s.Start(s)
	err = s.StartListener(listener)
	if err != nil {
		fmt.Printf("Error: %s\n", err)
	}

	return
}

