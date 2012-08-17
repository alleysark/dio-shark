/*
	dio_shark.c
	the main project source file
	it contains 'main' function

	dio-shark is disk block tracing and analysis tool
	it will loosing sharks to each cpu
*/

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include "dio_shark.h"
#include "dio_list.h"

/* ----------------[ define macro and structure define 	]------ */
struct dio_trace_setup{
	char name[32];
	uint16_t act_mask;
	uint32_t buf_size;
	uint32_t buf_nr;
	uint64_t start_lba;
	uint64_t end_lba;
	uint32_t pid;
};

//trace result
struct dio_trace_res{
	uint32_t magic;
	uint32_t sequence;
	uint64_t time;
	uint64_t sector;
	uint32_t bytes;
	uint32_t action;
	uint32_t pid;
	uint32_t device;
	uint32_t cpu;
	uint16_t error;
	uint16_t pdu_len;
};

#define TRACESETUP _IOWR(0x12, 115, struct dio_trace_setup)
#define TRACESTART _IO(0x12, 116)
#define TRACESTOP _IO(0x12, 117)
#define TRACEEND _IO(0x12, 118)

#define DBG_PATH "/sys/kernel/debug"

//the head of all sharks
struct shark_head{
	struct dl_node lsp;	//list start point
	int count;	//created shark count
	int totshk;	//total shark (include running sharks, sick sharks, done sharks,...)

	pthread_mutex_t sh_mtx;	//mutex for shark head status
	int rnshk;	//running count
};

/* -------------------[ global variables ]---------------------	*/
static int cpucnt = 0;	//number of CPUs
static int dfd;	//dev file descriptor
static char devpathname[MAX_FILENAME_LEN];
static struct shark_head sh;	//shark head

#define ARG_OPTS "d:o:"
static struct option arg_opts[] = {
	{
		.name = "device",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'd'
	},
	{
		.name = "outfile",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'o'
	}
};

char usage_detail[] = 	"\n"\
			"  [ -d <device> ]\n"\
			"  [ -o <outfile> ]\n"\
			"\n"\
			"\t-d : device which is traced\n"\
			"\t-o : output file name\n";



//locks
// lock and condition variable for gunfire
static pthread_cond_t gf_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t gf_mtx = PTHREAD_MUTEX_INITIALIZER;

// lock and condition variable for shark synchronous
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;

/* -------------------[ function declaration ]-----------------	*/
static void sig_handler(__attribute__((__unused__)) int sig);
bool parse_args(int argc, char** argv);

static void init_shark_head(struct shark_head* psh);

static void setup_dts();	//setup dio trace setup
static void start_dts();	//start dio trace setup
static void stop_dts();
static void end_dts();

void* shark_body(void* param);

/* -------------------[ function implementations ]-------------	*/
int main(int argc, char** argv){
	//init settings
	cpucnt = sysconf(_SC_NPROCESSORS_ONLN);
	if( cpucnt <= 0 ){
		fprintf(stderr, "cannot detect cpus\n");
		goto cancel;
	}

	signal(SIGINT, sig_handler);
	signal(SIGHUP, sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGPIPE, SIG_IGN);
/*
	if( !parse_args(argc, argv) ){
		fprintf(stderr, "dio-shark argument error.\n");
		goto cancel;
	}
*/
	dfd = open("/dev/sda", O_RDONLY);
	if( dfd < 0 ){
		perror("Failed to open dev");
		goto cancel;
	}

	loose_sharks();

	//check end states

	return 0;
cancel:
	//clear
	return -1;

}

void sig_handler(int sig){
	struct dl_node* p = NULL;
	__foreach_list(p, &sh.lsp){
		struct shark_inven* pinv = dl_entry(p, struct shark_inven, link);
		pinv->rnflag = false;
	}
}

/* start parse_args */
bool parse_args(int argc, char** argv){
	char tok;
	int cnt = 0;

	while( (tok = getopt_long(argc, argv, ARG_OPTS, arg_opts, NULL)) >= 0){
		switch(tok){
		case 'd':
			printf(" option d!\n");
			//set device info
			break;
		case 'o':
			printf(" option o!\n");
			//set output file
			break;
		default:
			printf("USAGE : %s %s\n", argv[0], usage_detail);
			return false;
		};
		cnt++;
	}

	if(cnt == 0){
		fprintf(stderr, "dio-shark has no argument.\n");
		return false;
	}

	return true;
}
/* end parse_args */

void init_shark_head(struct shark_head* psh){
	psh->rnshk = 0;
	psh->count = 0;
	psh->totshk = 0;
	
	INIT_DL_HEAD( &(psh->lsp) );
	pthread_mutex_init(&(psh->sh_mtx), NULL);
}

void setup_dts(){
	struct dio_trace_setup dts;
	memset( &dts, 0, sizeof(struct dio_trace_setup));
	dts.buf_size = 512*1024;
	dts.buf_nr = 4;
	dts.act_mask = (uint16_t)(~0U);

	if( ioctl(dfd, TRACESETUP, &dts) < 0 ){
		perror("Failed to setup dio_trace_setup");
		exit(-1);
	}else{
		strncpy(devpathname,dts.name, MAX_FILENAME_LEN);
		printf("%s\n", devpathname);
	}
}

void start_dts(){
	if( ioctl(dfd, TRACESTART) < 0 ){
		perror("Failed to start dio_trace_setup");
	}
	printf("start dio trace setup\n");
}

void stop_dts(){
	if( ioctl(dfd, TRACESTOP) < 0 ){
		perror("Failed to stop dio_trace_setup");
	}
	printf("stop dio trace setup\n");
}

void end_dts(){
	if( ioctl(dfd, TRACEEND) < 0 ){
		perror("Failed to teardown dio_trace_setup");
	}
	printf("end dio trace setup\n");
}
void loose_sharks(){
	int i;

	setup_dts();

	//init shark head
	init_shark_head( &sh );
	sh.totshk = cpucnt;

	for(i=0; i<sh.totshk; i++){
		loose_shark(i);
	}

	if( sh.count != sh.totshk ){
		fprintf(stderr, "some sharks have a problem..\n");
		goto err;
	}

	start_dts();

	//gunfire
	while(sh.rnshk != sh.totshk)
		pthread_cond_broadcast(&gf_cond);

	wait_allsharks_done();
err:
	stop_dts();
	end_dts();
}

bool loose_shark(int no){
	struct shark_inven* si = NULL;
	int i=0;

	si = (struct shark_inven*)malloc(sizeof(struct shark_inven));
	//init shark inventory
	si->shkno = no;
	si->rnflag = true;

	if( pthread_create(&(si->td), NULL, shark_body, si) ){	
		fprintf(stderr, "shark can not create his body.\n");
		return false;
	}

	dl_push_back(sh.lsp, si->link);
	sh.count++;

	pthread_mutex_lock(&s_mtx);
	pthread_cond_wait(&s_cond, &s_mtx);
	pthread_mutex_unlock(&s_mtx);

	if( si->stat != SHARK_READY ){
		printf("shark has problem..\n");
		return false;
	}

	return true;
}

void* shark_body(void* param){
	struct shark_inven* inven = (struct shark_inven*)param;

	//set dev-cpu input file descriptor
	snprintf(inven->ioinfo.ifname, MAX_FILENAME_LEN, "%s/block/%s/trace%d",
		DBG_PATH, devpathname, inven->shkno);
	inven->ioinfo.ifd = open(inven->ioinfo.ifname, O_RDONLY);
	if( inven->ioinfo.ifd < 0){
		perror("Failed to open ifd");
		inven->stat = SHARK_SICK;
		pthread_cond_signal(&s_cond);
		goto end;
	}
	//set shark's status to ready
	inven->stat = SHARK_READY;
	pthread_cond_signal(&s_cond);

	wait_gunfire();
	
	//set shark's status to working 
	inven->stat = SHARK_WORKING;
	pthread_mutex_lock(&(sh.sh_mtx));
	sh.rnshk++;	//increase running shark count
	pthread_mutex_unlock(&(sh.sh_mtx));

	printf("go\n");

	int dtrsz = sizeof(struct dio_trace_res);
	int rdsz = 0;
	int zerocnt = 0;
	struct dio_trace_res dtrbuf;
	while( inven->rnflag){
		memset(&dtrbuf, 0, dtrsz);
		
		rdsz = read(inven->ioinfo.ifd, &dtrbuf, dtrsz);
		if( rdsz == 0){
			zerocnt++;
			if( zerocnt >= 100000000 )
				break;
		}
		else if( rdsz < 0){
			perror("Failed to read");
			break;
		}
		printf("read size : %d, seq : %d, time %ld, action %d\n",rdsz, dtrbuf.sequence, dtrbuf.time, dtrbuf.action);
	}
	
	//signal done
	inven->stat = SHARK_DONE;
	close(inven->ioinfo.ifd);
end:
	return NULL;
}

void wait_gunfire(){
	pthread_mutex_lock(&gf_mtx);
	pthread_cond_wait(&gf_cond, &gf_mtx);
	pthread_mutex_unlock(&gf_mtx);
}

void wait_allsharks_done(){
	struct dl_node* con = NULL;
	__foreach_list(con, &sh.lsp){
		struct shark_inven* entry = dl_entry(con, struct shark_inven, link);
		pthread_join(entry->td, NULL);
	}
	printf("all sharks work doen\n");
}
