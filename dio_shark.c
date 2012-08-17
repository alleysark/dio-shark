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
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include "dio_shark.h"
#include "dio_list.h"

/* ----------------[ define macro and structure define 	]------ */
#define TRACESETUP _IOWR(0x12, 115)
#define TRACESTART _IO(0x12, 116)
#define TRACESTOP _IO(0x12, 117)
#define TRACEEND _IO(0x12, 118)

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
static struct shark_head sh;	//shark head

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
void* shark_body(void* param);

/* -------------------[ function implementations ]-------------	*/
int main(int argc, char** argv){
	
	//init settings
	cpucnt = sysconf(_SC_NPROCESSORS_ONLN);
	if( cpucnt <= 0 ){
		fprintf(stderr, "cannot detect cpus\n");
		goto cancel;
	}

/*
	signal(SIGINT, sig_handler);
	signal(SIGHUP, sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGPIPE, SIG_IGN);

	if( !parse_args(argc, argv) ){
		fprintf(stderr, "dio-shark argument error.\n");
		goto cancel;
	}
*/

	loose_sharks();

	//check end states

	return 0;

cancel:
	//clear
	return -1;

}

void sig_handler(int sig){
	
}

/* start parse_args */
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

void loose_sharks(){
	int i;

	//init shark head
	init_shark_head( &sh );
	sh.totshk = cpucnt;

	for(i=0; i<sh.totshk; i++){
		loose_shark(i);
	}

	if( sh.count != sh.totshk ){
		fprintf(stderr, "some sharks have a problem..\n");
		exit(-1);
	}

	//gunfire
	while(sh.rnshk != sh.totshk)
		pthread_cond_broadcast(&gf_cond);

	wait_allsharks_done();
}

bool loose_shark(int no){
	struct shark_inven* si = NULL;
	int i=0;

	si = (struct shark_inven*)malloc(sizeof(struct shark_inven));

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
		//oh dear..
	}

	return true;
}

void* shark_body(void* param){
	struct shark_inven* inven = (struct shark_inven*)param;

	//get cpu info
	//set outfd
	inven->stat = SHARK_READY;
	pthread_cond_signal(&s_cond);

	wait_gunfire();
	inven->stat = SHARK_WORKING;
	pthread_mutex_lock(&(sh.sh_mtx));
	sh.rnshk++;
	pthread_mutex_unlock(&(sh.sh_mtx));

	printf("go\n");

	//ioctl start
/*
	while(1){
	}
*/
	//ioctl stop
	//signal done

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
