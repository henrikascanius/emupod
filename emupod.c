// emupod 06-2011 (c) Henrik Ascanius Jacobsen <ascanius@runbox.com>
// Emulating an Ipod against Becker Indianapolis
// License: BeerWare

#include <termios.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

// Defined constatns
#define MAX_SONGS	15000			// Max. total no. of songs
#define MAX_SONGLISTS	1000			// Max no. of albums/composers/playlists/genres

#define PLAYER_LATENCY 	20			// *0.1 sec. wait time for player to issue track info

enum	sl_type { SL_PLAYLIST=1, SL_ARTIST=2, SL_ALBUM=3, SL_GENRE=4, SL_SONG=5, SL_COMPOSER=6 }; // As per AAP
enum	pl_state { PS_STOP=0, PS_PLAY=1, PS_PAUSE=2 };
enum	skip_type { SK_NONE, SK_FWD, SK_REV };

// Consts
char	player_name[] = "mpg123";
char	*pl_options[] = { "-C", "--long-tag" };
char	master_pl_name[] = "All songs";
//char*	host_devname = "/dev/ttyu0";		// device name
char*	host_devname = "/dev/ttyS3";		// device name



// The currently selected typeset (All albums, all composers, ...
enum sl_type	curr_songlisttype; 
int	noofsonglists=0;
char	*curr_songlists[MAX_SONGLISTS];
int	selected_item = -1;

// The active songlist (support all songs in one list!)
int	noofsongs=0;
char	*curr_songs[MAX_SONGS];

// Current status
enum 	pl_state curr_status = PS_PLAY;
int 	curr_song = 0;
int	pollmode = 0;
enum 	skip_type skip_mode = SK_NONE;

// Status retrieved from player
int	pl_length=-1;
int	pl_time=-1;
int	pl_songno=-1;
char	pl_title[128] = "";
char	pl_album[128] = "";
char	pl_genre[128] = "";
char	pl_artist[128] = "";


// Globals
int 	debug;		// debug output fd
pid_t 	pl_pid;     	// pid of player
int 	pl;		// player fd
int 	host_fd;	// host fd

int	host_holdoff = 0;

void abend(char* s)
{
	fprintf(stderr, "*** %s\n",s);
	exit(EXIT_FAILURE);
}

void error(char* s)
{
	perror(s);
	exit(EXIT_FAILURE);
}


// The timekeeping runs in 10th of sec. >4900 days between wraps -> longer than car battery lifetime
unsigned int gettselapsed(unsigned int since)
{
	static unsigned int base=0;
	struct timeval tv;
	gettimeofday(&tv,NULL);
	if (!base) base=tv.tv_sec;
	return ((tv.tv_sec-base)*10 + (tv.tv_usec/100000)) - since;
}

int tsec_timer(unsigned int t)
{
	return gettselapsed(0) + t;
}

int timer_expired(int t)
{
	return (gettselapsed(t) >> 31 == 0);
}

int cmd(char *cmd)
{
	int pstatus;
	pid_t pid = fork();
	if (pid==-1) error("fork, cmd");
	if (pid != 0) {	waitpid(pid, &pstatus, 0); return pstatus; }
	execlp("sh", "sh", "-c", cmd, NULL);
	perror("exec, cmd");
}

FILE* cmd_open(char *cmd, pid_t *cpid)
{
	// Execute cmd in shell; return stream for reading stdout
	int pfd[2];
	
	// Create pipe for IPC
	if (pipe(pfd) == -1) error("pipe");

	// Create proc.	
	*cpid = fork();
	if (*cpid == -1) error("fork");
	
	if (*cpid != 0)
	{	// Receiving end of pipe
		close(pfd[1]);
		return fdopen(pfd[0], "r");
	}
	// Spawned proc...
	close(pfd[0]);	// close unused
	if (dup2(pfd[1],1) != 1) error("dup2");  // redirect stdout to pipe
	execlp("sh", "sh", "-c", cmd, NULL);
	perror("exec");
}

int cmd_get(char* cmd, char **sv, int max)
{
	pid_t pid;
	int i, len, pstatus;
	char buf[256];
	FILE *outp;

	for (i=0; i<max; i++) sv[i] = NULL;
	outp = cmd_open(cmd, &pid);
	i=0;
	while (i<max && fgets(buf, sizeof(buf), outp) != NULL)
	{ 
		int j = strlen(buf);
		if (j && buf[j-1] == '\n') buf[j-1] = 0;	// kill NL
		sv[i] = malloc(strnlen(buf,sizeof(buf))+1);
		strcpy(sv[i],buf);  // 0-term ensured
		i++;
	}
	if (i>=max-1) kill(pid,9);
	waitpid(pid,&pstatus,0);
	fclose(outp);
	return i;
}

void sv_discard(char **sv, int no)
{
	int i;
	for (i=0; i<no; i++) free(sv[i]);
}

int get_songlists(enum sl_type type)
{
	if (noofsonglists && curr_songlisttype != SL_PLAYLIST) sv_discard(curr_songlists, noofsonglists);
	switch (type)
	{
	case SL_PLAYLIST:	// Only "master" playlist implemented
		curr_songlists[0] = master_pl_name;
		noofsonglists = 1;
		break;
	case SL_ALBUM:
		noofsonglists = cmd_get("find . -type d | grep -v thumb | sed -e \"s/\\.\\///\" | grep / | sed -e \"s/.*\\///\" | sort", curr_songlists, MAX_SONGLISTS);
//		noofsonglists = cmd_get("find . -type d | sed -e \"s/\\.\\///\" | grep / | sed -e \"s/.*\\///\" | sort", curr_songlists, MAX_SONGLISTS);
		break;
	case SL_ARTIST:
		noofsonglists = cmd_get("find . -type d -maxdepth 1 | grep / | grep -v thumb | sed -e \"s/\\.\\///\" | sort", curr_songlists, MAX_SONGLISTS);
		break;
	default:
		noofsonglists = 0;
		break;
	}
	curr_songlisttype = type;
	selected_item=-1;
	return noofsonglists;
}

int select_item(enum sl_type type, int item)
{
	char cmd[256];
	char *path[1];
	
	if (type==SL_SONG) { dprintf(debug,"select song item?\n"); return 0; }
	if (type != curr_songlisttype) get_songlists(type);
	if (item > noofsonglists-1) return 0;
	if (item != selected_item)
	{	// Bug: cannot handle 2 albums w. same title by 2 diffrent artists in album mode (will always get the first)
		strcpy(cmd, "find . -name \"");
		strncat(cmd, curr_songlists[item], 128);
		strcat(cmd, "\"");
		if (cmd_get(cmd, path, 1) < 1) abend("Internal error - path?");
		strcpy(cmd, "find \"");
		strncat(cmd, path[0], 128);
		strcat(cmd,"\" -type f -name \"*.mp3\" |  sort");
		noofsongs = cmd_get(cmd, curr_songs, MAX_SONGS);
		selected_item = item;
		sv_discard(path, 1);
	}
	return 1;
}


int extract_album(char *path, char *album)
{	// Extract last dir in path, i.e. the album name
	char *cp;
	
	strcpy(album, path);
	if ((cp=strrchr(album,'/'))==NULL) return 0;
	*cp=0;	// Kill filename part
	if ((cp=strrchr(album,'/'))==NULL) return 1;
	memcpy(album, cp+1, strlen(cp));  // cpy includes \0
}

int nxt(int i, int no)
{
	return i >= no-1 ? 0 : i+1;
}

int prv(int i, int no)
{
	return !i ? no-1 : i-1;
}

int jump_album(int reverse)
{	// Indianapolis hack: Use FF/FR keys for album jumping in Artist mode
	char this[128];
	char tmp[128];
	int i=curr_song;
	int j=0;
	
	if (selected_item < 0 || noofsonglists < 1 || noofsongs < 0 || curr_song < 0) return -1;
	if (!extract_album(curr_songs[curr_song], this)) return -1;
	do {
		if (j++ >= noofsongs) return -1;	// Only one album
		i = reverse ? prv(i,noofsongs) : nxt(i,noofsongs);
		extract_album(curr_songs[i], tmp);
	} while (!strncmp(this,tmp,sizeof(tmp)));
	if (!reverse) return i;		// Found 1st song of next album
	do {
		i = prv(i,noofsongs);
		extract_album(curr_songs[i], this);
	} while (!strncmp(this,tmp,sizeof(tmp)));
	return nxt(i,noofsongs);	// Found 1st song of prev. album
}

int kill_player()
{
	int i=0, pstatus;
	if (!pl_pid) return 1;
	dprintf(pl, "q");
	close(pl);
	while (i++<60)
	{
		if (waitpid(pl_pid, &pstatus, WNOHANG) == pl_pid) 
			{ pl_pid=0; printf("killed in %d\n",i); return 1; }
		if (i==5)
			{ printf("Sharpshooting to kill player...\n"); kill(pl_pid,9); }
		usleep(100000);
	}
	printf("Player would not die\n");
	return 0;
}
	
int player_running()
{
	int i=0, pstatus;
	if (!pl_pid) return 0;
	if (waitpid(pl_pid, &pstatus, WNOHANG) == pl_pid) 
		{ pl_pid=0; close(pl); return 0; }
	return 1;
}

void launch_player(char** cmdv)
{
	int sockv[2];
	
	kill_player();
	if (socketpair(PF_LOCAL, SOCK_STREAM, 0, sockv)) error("socketpair");
	pl_pid = fork();
	if (pl_pid == -1) error("fork player");
	if (pl_pid) 
	{	 // mainproc
		close(sockv[1]);
		pl = sockv[0];
		fcntl(pl, F_SETFL, fcntl(pl, F_GETFL, 0) | O_NONBLOCK);
		return;
	}
	// Spawned proc...
	close(sockv[0]);	// close unused
	if (dup2(sockv[1],0) != 0) error("dup2 player 0");  // redirect stdin to sock
	if (dup2(sockv[1],1) != 1) error("dup2 player 1");  // redirect stdout to sock
	if (dup2(sockv[1],2) != 2) error("dup2 player 2");  // redirect stderr to sock
	execvp(player_name, cmdv);
	perror("exec player");
}

void play(char** plv, int songs, char *xopt1, char *xopt2, char *xopt3, char *xopt4)
{
	int i;
	int optcnt = sizeof(pl_options)/sizeof(char*);
	char **cmdv;
	
	if (!songs) { kill_player(); return; }  // empty playlist
	cmdv = calloc(optcnt+songs+6,sizeof(char*));   // +2 : arg0 + 4 xtraoptions + final NULL
	// progname + options:
	cmdv[0] = player_name;
	memcpy(&cmdv[1], pl_options, sizeof(pl_options));
	i = 1+optcnt;
	// xtra options in call:
	if (xopt1) memcpy(&cmdv[i++], &xopt1, sizeof(char*));
	if (xopt2) memcpy(&cmdv[i++], &xopt2, sizeof(char*));
	if (xopt3) memcpy(&cmdv[i++], &xopt3, sizeof(char*));
	if (xopt4) memcpy(&cmdv[i++], &xopt4, sizeof(char*));
	
	// songs:
	memcpy(&cmdv[i], plv, songs*sizeof(char*));
	launch_player(cmdv);
	free(cmdv);
}

void switch_to_song(int item)
{
	if (item >= noofsongs) return;
	curr_song = pl_songno;
	if (!item) 
	{	// Weird mpg123 bug? cannot backspace to first track
		play(curr_songs, noofsongs, NULL, NULL, NULL, NULL);
		curr_song=0;
	}
	else
	{
		while (curr_song > item) { dprintf(pl,"d"); curr_song--; } 
		while (curr_song < item) { dprintf(pl,"f"); curr_song++; }
	}
	host_holdoff=tsec_timer(PLAYER_LATENCY);
	skip_mode = SK_NONE;
}

int getnumber(unsigned char *buf)
{
	return  ((unsigned int)buf[0]<<24) |
		((unsigned int)buf[1]<<16) |
		((unsigned int)buf[2]<<8)  |
		((unsigned int)buf[3]<<0);
}

void setnumber(char* buf, int n)
{
	buf[0] = (n>>24) & 0xff;
	buf[1] = (n>>16) & 0xff;
	buf[2] = (n>>8)  & 0xff;
	buf[3] = (n>>0)  & 0xff;
}

void host_send(char* repl, int size)
{
   char buf[256];
   int sum=0, idx=0,i=0;
   
   buf[i++] = 0xff;
   buf[i++] = 0x55;
   buf[i++] = size;
   sum = size;
   for (idx=0; idx<size; idx++) 
   {
      buf[i++] = repl[idx];
      sum += repl[idx];
   }
   buf[i++] = (0x100-sum) & 0xff;
   write(host_fd, &buf, i);
   if (0)
   {
      printf("repl: ");
      for (idx=0; idx<size; idx++) printf("%02x ",repl[idx]);
      printf("\n");
   }
}

void host_result(char* cmd, int result)
{
   char repl[6];
   repl[0] = 0x04;
   repl[1] = 0x00;
   repl[2] = 0x01;
   repl[3] = result;
   repl[4] = cmd[1];
   repl[5] = cmd[2];
   host_send(repl, 6);
}

void host_command(unsigned char *cmd, int size)
{
	enum sl_type type;
	int item, cnt,idx,i;
	char repl[256];
	char tmp[128];
	char *cp;
	
	if (cmd[0] == 0x00 && cmd[1] == 0x01 && cmd[2] == 0x04)
	{	// The only mode0 command we expect to see	
		dprintf(debug, "Switch to mode 4\n");
		return;
	}
	if (cmd[0] == 0x04 && cmd[1] == 0x00)
	{	// Mode 4 command
		repl[0] = 0x04;		// Prepare std. ...
		repl[1] = 0x00;
		repl[2] = cmd[2]+1;	// ...for commands with data reply
		switch (cmd[2])
		{
		case 0x12:
			 dprintf(debug,"Get pod type\n");
			 repl[3] = 0x01;	// 3G 30GB
			 repl[4] = 0x02;
			 host_send(repl,5);
			 break;
		case 0x16:  
			dprintf(debug,"Switch to main playlist\n");
			// We don't need to do anything for now...
			host_result(cmd, 0);
			break;
		case 0x17:
			type = cmd[3];
			item = getnumber(&cmd[4]);
			dprintf(debug,"Switch to item type %d index %d\n",type,item);
			if (type != curr_songlisttype) get_songlists(type);
			select_item(type, item);
			host_result(cmd, 0);
			break;
		case 0x18:
			type = cmd[3];
			if (type != SL_SONG && type != curr_songlisttype) get_songlists(type);
			setnumber(&repl[3], type==SL_SONG ? noofsongs : noofsonglists);
			dprintf(debug,"Get no. of type %d: %d\n",type,getnumber(&repl[3]));
			host_send(repl,7);
			break;
		case 0x1a:
			type = cmd[3];
			item = getnumber(&cmd[4]);			
			cnt = getnumber(&cmd[8]);			
			dprintf(debug,"Send names for type %d item %d cnt %d\n",type,item,cnt);
			if (type != curr_songlisttype && type != SL_SONG) get_songlists(type);
			while (cnt-- && item < (type == SL_SONG ? noofsongs : noofsonglists))
			{
				setnumber(&repl[3],item);
				strncpy(&repl[7], type == SL_SONG ? curr_songs[item] : curr_songlists[item],128);
				host_send(repl,8+strlen(&repl[7]));
				item++;
			}
			break;
		case 0x1c:
			dprintf(debug,"Get time and status\n");
			setnumber(&repl[3],pl_length<0 ? 0 : pl_length);
			setnumber(&repl[7],pl_time<0 ? 0 : pl_time);
			repl[11] = curr_status;
			host_send(repl,12);
			break;
		case 0x1e:
			dprintf(debug,"Get current pos. in playlist (%d)\n",curr_song);
			setnumber(&repl[3],curr_song<0 ? 0 : curr_song);
			host_send(repl,7);
			break;
		case 0x20:
			item = getnumber(&cmd[3]);
			dprintf(debug,"Get title of song number %d  curr_song: %d pl_songno: %d\n",item,curr_song,pl_songno);
			if (item >= noofsongs) repl[3] = 0; else 
			{
				if (item != pl_songno)
				{
					strncpy(tmp, curr_songs[item],sizeof(tmp));
					if ((cp=strrchr(tmp, '/')) == NULL) cp = tmp; else cp++;
					if (strstr(cp, ".mp3") == cp+strlen(cp)-4) cp[strlen(cp)-4] = 0;
				}
				strncpy(&repl[3], item==pl_songno ? pl_title : cp, 128);
			}
			host_send(repl,4+strlen(&repl[3]));
			break;
		case 0x22: 
			item = getnumber(&cmd[3]);
			dprintf(debug,"Get artist of song number %d\n",item);
			if (item != pl_songno) repl[3] = 0; else strncpy(&repl[3], pl_artist, 128);
			host_send(repl,4+strlen(&repl[3]));
			break;
		case 0x24:  
			item = getnumber(&cmd[3]);
			dprintf(debug,"Get album of song number %d\n",item);
			if (item != pl_songno) repl[3] = 0; else strncpy(&repl[3], pl_album, 128);
			host_send(repl,4+strlen(&repl[3]));
			break;
		case 0x26:
			dprintf(debug,"Set polling mode %d\n",cmd[3]);
			pollmode = cmd[3];
			host_result(cmd, 0);
			break;
		case 0x28:
			item = getnumber(&cmd[3]);
			dprintf(debug,"Execute playlist from song no. %d\n",item);
			if (item < 0) item = 0;
			if (item < noofsongs)
			{
				play(curr_songs, noofsongs, NULL, NULL, NULL, NULL);
				curr_song = item;
				while (item--) dprintf(pl,"f"); // Select the proper starting song
				host_result(cmd, 0);
				host_holdoff=tsec_timer(PLAYER_LATENCY);
				curr_status=PS_PLAY;
			}
			skip_mode = SK_NONE;
			break;
		case 0x29: 
			dprintf(debug,"Playback control %d\n",cmd[3]);
			switch (cmd[3])
			{
			case 1: 
				curr_status = curr_status == 1 ? 2 : 1;
				dprintf(pl,"s");   // Toggle play/pause - we simply don't have the "STOP" state for now...			
				break;
			case 2: kill_player(); curr_status = 0;   // STOP key
			case 3: dprintf(pl,"f"); host_holdoff=tsec_timer(PLAYER_LATENCY); break;  // skip+ ...not used on Indianapois...
			case 4: dprintf(pl,"b"); host_holdoff=tsec_timer(PLAYER_LATENCY); break;  // skip- - used to restart current song
			case 5: if ((i=jump_album(0))>=0) switch_to_song(i); else skip_mode = SK_FWD; break;
			case 6: if ((i=jump_album(1))>=0) switch_to_song(i); else skip_mode = SK_REV; break;
			case 7: skip_mode = SK_NONE; break;
			default: break;
			}
			host_result(cmd, 0);
			break;
		case 0x2e: 
			dprintf(debug,"Set shuffle mode %d\n",cmd[3]);
			host_result(cmd, 0);
			break;
		case 0x31: 
			dprintf(debug,"Set repeat mode %d\n",cmd[3]);
			host_result(cmd, 0);
			break;
		case 0x32:
			dprintf(debug,"Upload picture block\n");
			host_result(cmd,0);
			break;
		case 0x33:
			dprintf(debug,"Get max. screen size\n");
			repl[3] = 0x00;	//return something sensible to keep host happy
			repl[4] = 0x78;
			repl[5] = 0x00;
			repl[6] = 0x41;
			repl[7] = 0x01;
			host_send(repl,8);
			break;
		case 0x35:
			dprintf(debug,"Get no. of songs in playlist\n");
			setnumber(&repl[3],noofsongs<0 ? 0 : noofsongs);
			host_send(repl,7);
			break;
		case 0x37:
			item = getnumber(&cmd[3]);
			dprintf(debug,"Switch to song no. %d in playlist\n",item);
			if (item < noofsongs) switch_to_song(item);
			host_result(cmd, 0);
			skip_mode = SK_NONE;
			break;
		default:
			dprintf(debug,"Unhandled mode 4 command: ");
			for (idx=2; idx<size; idx++) dprintf(debug,"%02x ",cmd[idx]);
			dprintf(debug,"\n");
			break;
		}
	}
	else
	{
		dprintf(debug,"Unhandled command: ");
		for (idx=0; idx<size; idx++) dprintf(debug,"%02x ",cmd[idx]);
		dprintf(debug,"\n");
	}
}

void host_input(unsigned char c)
{
	static int state=0;
	static unsigned char cmd[256];
	static int idx=0;
	static int sum;
	int sumok;
	static int size=0;
	
	// AAP deframer
        switch(state)
	{
	   case 0: if (c == 0xff) state++; return;
	   case 1: if (c == 0x55) state++; else if (c != 0xff) state=0; return;
	   case 2: size = c; sum = c; idx=0; state++; return;
	   case 3: 
	      cmd[idx++] = c;
	      sum += c;
	      if (idx == size) state++;
	      break;
           case 4:
	      sumok = (c == ((0x100 - sum) & 0xff));
	      state = 0;
	      
	      if (!sumok)
	      {
	      	      for (idx=0; idx<size; idx++) printf("%02x ",cmd[idx]);
		      dprintf(debug,"sum not ok\n");
	      }
	      else
	      	      host_command(cmd,size);
	      return;
	}
}

char *strskip(char *str, char *tag)
{	// return ptr to what follow after tag in str, also skipping blanks
	char *cp = strstr(str, tag);
	if (cp==NULL) return str;
	cp += strlen(tag);
	while (*(++cp) == ' ') ;
	return cp;
}

void got_player_line(char *line)
{
	char *cp;
	
	if (strstr(line, "TIME:"))
	{
		pl_time = strtol(strskip(line, ":"), &cp, 10);
		pl_length = strtol(cp, NULL, 10);
	} else
	if (strstr(line, "Playing MPEG stream")) pl_songno = strtol(strskip(line,"stream"), NULL, 10)-1; else
	if (strstr(line, "Title:")) strncpy(pl_title, strskip(line,":"), sizeof(pl_title)); else
	if (strstr(line, "Artist:")) strncpy(pl_artist, strskip(line,":"), sizeof(pl_artist)); else
	if (strstr(line, "Album:")) strncpy(pl_album, strskip(line,":"), sizeof(pl_album)); else
	if (strstr(line, "Genre:")) strncpy(pl_genre, strskip(line,":"), sizeof(pl_genre)); 
	return;
}

void player_input(unsigned char c)
{
	static unsigned char buf[256];
	static int idx;
	
	if (c==0xff) { memset(buf, 0, sizeof(buf)); idx=0; return; }	// flush
	if (c != '\n') { if (idx < sizeof(buf)-1) buf[idx++] = c; return; }
	got_player_line(buf);
	player_input(0xff);
	return;
}	

int main()
{
	struct termios tio;
	unsigned char c;
	char tmp[128];
	int poll_timer;
	int skip_timer = 0;
	char pollmsg[8];
	
	if (getenv("EMUPOD_DEBUG"))
		debug = dup(2);
	else
		debug = open("/dev/null", O_WRONLY); 
	if (debug<0) error("open debug");
	
	// Open & setup serial port
	if ((host_fd = open(host_devname, O_RDWR | O_NOCTTY | O_NONBLOCK)) < 0)	error("open host port");  
	tcgetattr(host_fd, &tio);
	tio.c_cflag = CS8 | CLOCAL | CREAD;
	tio.c_iflag = IGNPAR;
	tio.c_oflag = 0;
	tio.c_lflag = 0;
	cfsetspeed(&tio, B19200);
	tcsetattr(host_fd, TCSAFLUSH, &tio);
	
	// Kill leftover player
	strcpy(tmp, "killall -9 ");
	strcat(tmp, player_name);
	strcat(tmp," >/dev/null 2>&1");
	cmd(tmp);
	
	// We may write to player after it has termminated... don't die from that
	signal(SIGPIPE, SIG_IGN);
	
	// Start to play... something
	if (get_songlists(SL_ALBUM) <= 0) abend("No songs");
	select_item(SL_ALBUM, 0);
	play(curr_songs, noofsongs, NULL, NULL, NULL, NULL);
	dprintf(pl, "s");
	curr_status = PS_PAUSE;
	poll_timer = tsec_timer(5);
	
	// Fixed part of poll message
	pollmsg[0] = 0x04;
	pollmsg[1] = 0x00;
	pollmsg[2] = 0x27;
	
	while (1)
	{
		while (timer_expired(host_holdoff) && read(host_fd, &c, 1) == 1) host_input(c);
		
		while (read(pl, &c, 1) == 1) player_input(c);
		
		if (timer_expired(poll_timer))
		{
			if (pollmode)
			{
				if (!player_running())
				{	// No more songs
					pollmsg[3] = 0;
					skip_mode = SK_NONE;
					host_send(pollmsg,4);
				}
				else
				if (curr_song != pl_songno)
				{	// player started on another song
					pollmsg[3] = 1;
					host_send(pollmsg,4);
					curr_song = pl_songno;
					host_holdoff=tsec_timer(PLAYER_LATENCY);
				}
				else
				{
					pollmsg[3] = 4;
					setnumber(&pollmsg[4],pl_time);
					host_send(pollmsg,8);
				}
			}
			poll_timer = tsec_timer(5);
		}
		
		if (skip_mode != SK_NONE)
		{
			if (timer_expired(skip_timer))
			{
				dprintf(pl,skip_mode == SK_FWD ? ":" : ";");
				skip_timer = tsec_timer(2);
			}
		}	
		usleep(9000);
	}
}

