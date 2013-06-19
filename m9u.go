package main

import (
	"code.google.com/p/go9p/p"
	"code.google.com/p/go9p/p/srv"
	"strings"
	"bytes"
	"flag"
	"fmt"
	"os"
	"strconv"
	"unicode/utf8"
)

type M9Player struct {
	playlist []string
	position int
	queue []string

	player *os.Process
	song *string
}

var m9 *M9Player

func (m9 *M9Player) spawn(song string) {
	player, err := os.StartProcess("/bin/echo", []string{"m9play", song}, new(os.ProcAttr))
	if err != nil {
		fmt.Printf("couldn't spawn player: %s\n", err)
		return
	}
	m9.player = player
	events <- "Play " + song
	go func() {
		player.Wait()
		if len(m9.queue) == 0 && len(m9.playlist) > 0 {
			m9.position = (m9.position + 1) % len(m9.playlist)
		}
		if m9.player != nil {
			m9.player = nil
			m9.Play("")
		}
	}()
}

func (m9 *M9Player) state() string {
	var s string
	if m9.player == nil {
		s = "Stop"
	} else {
		s = "Play"
	}

	if m9.song != nil {
		return s + " " + *m9.song
	} else if len(m9.queue) > 0 {
		return s + " " + m9.queue[0]
	} else if len(m9.playlist) > 0 {
		return s + " " + m9.playlist[m9.position]
	}
	return s
}

func (m9 *M9Player) Add(song string) {
	m9.playlist = append(m9.playlist, song)
}

func (m9 *M9Player) Clear() {
	m9.playlist = make([]string, 0)
}

func (m9 *M9Player) Enqueue(song string) {
	m9.queue = append(m9.queue, song)
}

func (m9 *M9Player) Play(song string) {
	player := m9.player
	if player != nil {
		/* already playing; stop the current player first */
		m9.player = nil
		player.Kill()
	}
	if song != "" {
		m9.spawn(song)
	} else {
		if len(m9.queue) > 0 {
			m9.spawn(m9.queue[0])
			m9.queue = m9.queue[1:]
		} else if len(m9.playlist) > 0 {
			m9.spawn(m9.playlist[m9.position])
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

func play(song string) {
	fmt.Printf("play: %s\n", song)
	m9.Play(song)
}

func skip(amount string) error {
	i, err := strconv.Atoi(amount)
	if err != nil {
		return err
	}
	m9.Skip(i)
	return nil
}

func stop() {
	fmt.Printf("stop:\n")
	m9.Stop()
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

var addr = flag.String("addr", ":5640", "network address")

type CtlFile struct {
	srv.File
}
type ListFile struct {
	srv.File
	rd map[*srv.Fid] []byte
	wr map[*srv.Fid] *PartialLine
}
type QueueFile struct {
	srv.File
	rd map[*srv.Fid] []byte
	wr map[*srv.Fid] *PartialLine
}
type EventFile struct {
	srv.File
}

func (*CtlFile) Write(fid *srv.FFid, b []byte, offset uint64) (n int, err error) {
	cmd := string(b)
	fmt.Printf("write:  %s\n", cmd)
	if strings.HasPrefix(cmd, "play") {
		play(strings.Trim(cmd[4:], " \n"))
	} else if strings.HasPrefix(cmd, "skip") {
		err = skip(strings.Trim(cmd[4:], " \n"))
	} else if strings.HasPrefix(cmd, "stop") {
		stop()
	} else {
		err = m9err("/ctl", "write", "syntax error")
	}
	if err == nil {
		n = len(b)
	}
	return n, err
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
		fmt.Printf("/list: wr\n");
		lstfile.wr[fid.Fid] = new(PartialLine)
		if mode & p.OTRUNC != 0 {
			m9.Clear()
		}
	}
	if mode & 3 == p.OREAD || mode & 3 == p.ORDWR {
		fmt.Printf("/list: rd\n");
		lstfile.rd[fid.Fid] = mkbuf(m9.playlist)
	}
	return nil
}

func (lstfile *ListFile) Clunk(fid *srv.FFid) error {
	delete(lstfile.rd, fid.Fid)
	delete(lstfile.wr, fid.Fid)
	return nil
}

type M9Error struct {
	file string
	op string
	msg string
}

func (e *M9Error) Error() string {
	return e.file + ": " + e.op + ": " + e.msg
}

func m9err(file string, op string, msg string) *M9Error {
	err := new(M9Error)
	err.file = file
	err.op = op
	err.msg = msg
	return err
}

func (lstfile *ListFile) Write(fid *srv.FFid, b []byte, offset uint64) (int, error) {
	prefix, ok := lstfile.wr[fid.Fid]
	if !ok {
		return 0, m9err("/list", "write", "bad state")
	}
	i := 0
	for {
		j := bytes.IndexByte(b[i:], '\n')
		if j == -1 {
			break
		}
		song := prefix.append(b[i:j])
		m9.Add(song)
		i = j+1
	}
	if i < len(b) {
		prefix.leftover = b[i:]
	}
	return len(b), nil
}

func min(a uint64, b uint64) uint64 {
	if a < b {
		return a
	}
	return b
}

func (lstfile *ListFile) Read(fid *srv.FFid, b []byte, offset uint64) (int, error) {
	buf, ok := lstfile.rd[fid.Fid]
	if !ok {
		return 0, m9err("/list", "read", "bad state")
	}
	remaining := uint64(len(buf)) - offset
	n := min(remaining, uint64(len(b)))
	copy(b, buf[offset:offset + n])
	return int(n), nil
}


func (qf *QueueFile) Open(fid *srv.FFid, mode uint8) error {
	if mode & 3 == p.OWRITE || mode & 3 == p.ORDWR {
		fmt.Printf("/list: wr\n");
		qf.wr[fid.Fid] = new(PartialLine)
	}
	if mode & 3 == p.OREAD || mode & 3 == p.ORDWR {
		fmt.Printf("/list: rd\n");
		qf.rd[fid.Fid] = mkbuf(m9.playlist)
	}
	return nil
}

func (qf *QueueFile) Clunk(fid *srv.FFid) error {
	delete(qf.wr, fid.Fid)
	delete(qf.rd, fid.Fid)
	return nil
}

func (qf *QueueFile) Write(fid *srv.FFid, b []byte, offset uint64) (int, error) {
	var aux *PartialLine
	if fid.Fid.Aux == nil {
		aux = new(PartialLine)
		fid.Fid.Aux = aux
	} else {
		aux, _ = fid.Fid.Aux.(*PartialLine)
	}
	i := 0
	for j := 0; j != -1; j = bytes.IndexByte(b[i:], '\n') {
		song := aux.append(b[i:j])
		m9.Enqueue(song)
		i = j+1
	}
	if i < len(b) {
		aux.leftover = b[i:]
	}
	return len(b), nil
}

func (qf *QueueFile) Read(fid *srv.FFid, b []byte, offset uint64) (int, error) {
	buf, ok := qf.rd[fid.Fid]
	if !ok {
		return 0, m9err("/list", "read", "bad state")
	}
	remaining := uint64(len(buf)) - offset
	n := min(remaining, uint64(len(b)))
	copy(b, buf[offset:offset + n])
	return int(n), nil
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

func main() {
	var err error

	_, _ = strconv.Atoi("1")

	uid := p.OsUsers.Uid2User(os.Geteuid())
	gid := p.OsUsers.Gid2Group(os.Getegid())
	fmt.Printf("uid = %d  gid = %d\n", os.Geteuid(), os.Getegid())

	events = make(chan string)
	register = make(chan chan string)

	m9 = new(M9Player)

	go eventer()

	root := new(srv.File)
	root.Add(nil, "/", uid, gid, p.DMDIR|0555, nil)
	ctl := new(CtlFile)
	ctl.Add(root, "ctl", uid, gid, 0644, ctl)
	list := new(ListFile)
	list.wr = make(map[*srv.Fid]*PartialLine)
	list.rd = make(map[*srv.Fid] []byte)
	list.Add(root, "list", uid, gid, 0644, list)
	queue := new(QueueFile)
	queue.Add(root, "queue", uid, gid, 0644, queue)
	event := new(EventFile)
	event.Add(root, "event", uid, gid, 0444, event)

	s := srv.NewFileSrv(root)
	s.Start(s)
	err = s.StartNetListener("tcp", *addr)
	if err != nil {
		fmt.Printf("Error: %s\n", err)
	}

	return
}

