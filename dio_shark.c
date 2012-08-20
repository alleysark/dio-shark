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
#include <sys/poll.h>
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
#define TRACETEARDOWN _IO(0x12, 118)

#define DBG_PATH "/sys/kernel/debug"

//the head of all sharks
struct shark_head{
	struct dl_node lsp;	//list start point. shark list's entry is struct shark_inven
	int count;	//created shark count
	int totshk;	//total shark (include running sharks, sick sharks, done sharks,...)

	pthread_mutex_t sh_mtx;	//mutex for shark head status
	int rnshk;	//running count
};

#define MAX_FILENAME_LEN 128
enum dev_stat { 
	OPEN=0,
	SETUP,
	START,
	STOP,
	TEARDOWN,
	CLOSE
};

struct dev_entity{
	struct dl_node link;
	char devname[MAX_FILENAME_LEN];
	int dfd;	//device file descriptor
	enum dev_stat stat;
};

#define DEFAULT_TIMEOUT 1000	//10sec
struct dlst_head{
	struct dl_node lsp;	//dev list start point
	int count;
	uint32_t timeout;	//polling timeout value
};

/* -------------------[ global variables ]---------------------	*/
static int cpucnt = 0;	//number of CPUs
static struct shark_head sh;	//shark head
static struct dlst_head dlsth;	//dev list head

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

static void init_shark_head();
static void destroy_shark_head();

static void init_dlst_head();
static bool add_dev(char* devpath);	//add and open
static void close_devs();
static void clear_devs();
static void check_devstat();

static bool open_fds(struct shark_inven* pshk);
static void clear_fds(struct shark_inven* pshk);

static bool setup_dts();	//setup dio trace setup
static bool start_dts();	//start dio trace setup
static void stop_dts();
static void teardown_dts();

void* shark_body(void* param);

/* -------------------[ function implementations ]-------------	*/
int main(int argc, char** argv){
	//init settings
	cpucnt = sysconf(_SC_NPROCESSORS_ONLN);
	if( cpucnt <= 0 ){
		perror("cannot detect cpus.");
		goto cancel;
	}

	init_shark_head();
	sh.totshk = cpucnt;	

	init_dlst_head();
	
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
	//open device path
	if( !add_dev("/dev/sda") ){
		goto cancel;
	}

	loose_sharks();

	//check end states
	close_devs();
	check_devstat();
	clear_devs();

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

void init_shark_head(){
	sh.rnshk = 0;
	sh.count = 0;
	sh.totshk = 0;
	
	INIT_DL_HEAD( &(sh.lsp) );
	pthread_mutex_init(&(sh.sh_mtx), NULL);
}

void destroy_shark_head(){
	//DESTROY_DL_HEAD( &(sh.lsp) );
	pthread_mutex_destroy( &(sh.sh_mtx) );
}

void init_dlst_head(){
	dlsth.count = 0;
	dlsth.timeout = DEFAULT_TIMEOUT;

	INIT_DL_HEAD( &(dlsth.lsp));
}

bool add_dev(char* devpath){
	struct dev_entity* dv = (struct dev_entity*)malloc(sizeof(struct dev_entity));
	memset(dv, 0, sizeof(struct dev_entity));
	
	dv->dfd = open(devpath, O_RDONLY | O_NONBLOCK);
	if( dv->dfd < 0 ){
		perror("Failed to open dev path");
		free(dv);
		return false;
	}
	dv->stat = OPEN;

	dl_push_back(dlsth.lsp, dv->link);
	dlsth.count++;
	printf(" > dev %s is added\n", devpath);

	return true;
}

void close_devs(){
	struct dl_node* p = NULL;

	__foreach_list(p, &dlsth.lsp){
		struct dev_entity* pdv = dl_entry(p, struct dev_entity, link);
		if( pdv->stat == START ){
			printf(" > %s is not stoped and teardown\n", pdv->devname);
		}
		else if( pdv->stat == STOP ){
			printf(" > %s is not teardown\n", pdv->devname);
		}
		else{
			close(pdv->dfd);
			pdv->stat = CLOSE;
		}
	}

}

void clear_devs(){
	struct dl_node* p = NULL;

	__foreach_list(p, &dlsth.lsp){
		struct dev_entity* pdv = dl_entry(p, struct dev_entity, link);
		close(pdv->dfd);
		
		p->prv->nxt = p->nxt;
		p = p->prv;
		free(pdv);
	}
}

void check_devstat(){
	struct dl_node* p = NULL;

	__foreach_list(p, &dlsth.lsp){
		struct dev_entity* pdv = dl_entry(p, struct dev_entity, link);
		switch(pdv->stat){
		case OPEN:
			printf(" > %s is OPEN state\n", pdv->devname);
			break;
		case START:
			printf(" > %s is START state\n", pdv->devname);
			break;
		case STOP:
			printf(" > %s is STOP state\n", pdv->devname);
			break;
		case TEARDOWN:
			printf(" > %s is TEARDOWN state\n", pdv->devname);
			break;
		};
	}
}

bool open_fds(struct shark_inven* pshk){
	struct dl_node* p = NULL;
	char fnbuf[MAX_FILENAME_LEN];

	pshk->pfds = (struct pollfd*)malloc(sizeof(struct pollfd)*dlsth.count);
	memset(pshk->pfds, 0, sizeof(struct pollfd)*dlsth.count);

	int idx = 0;
	__foreach_list(p, &dlsth.lsp){
		struct dev_entity* pdv = dl_entry(p, struct dev_entity, link);

		memset(fnbuf, 0, MAX_FILENAME_LEN);
		snprintf(fnbuf, MAX_FILENAME_LEN, "%s/block/%s/trace%d",
			DBG_PATH, pdv->devname, pshk->shkno);
		printf(" > open fds path for cpu \'%d\' : %s\n",pshk->shkno, fnbuf);

		pshk->pfds[idx].events = POLLIN;
		pshk->pfds[idx].revents = 0;
		pshk->pfds[idx].fd = open(fnbuf, O_RDONLY | O_NONBLOCK);
		if( pshk->pfds[idx].fd < 0 ){
			perror("Failed to open fds");
			free(pshk->pfds);
			return false;
		}
		idx++;
	}
	return true;
}

void clear_fds(struct shark_inven* pshk){
	int i;
	for(i=0; i<dlsth.count; i++){
		pshk->pfds[i].events = 0;
		pshk->pfds[i].revents = 0;
		close(pshk->pfds[i].fd);
	}
	free(pshk->pfds);
}

bool setup_dts(){
	struct dio_trace_setup dts;

	memset( &dts, 0, sizeof(struct dio_trace_setup));
	dts.buf_size = 512*1024;
	dts.buf_nr = 4;
	dts.act_mask = (uint16_t)(~0U);

	struct dl_node* p = NULL;
	__foreach_list(p, &dlsth.lsp){
		struct dev_entity* pdv = dl_entry(p, struct dev_entity, link);
		if( pdv->stat == OPEN ){
			if( ioctl(pdv->dfd, TRACESETUP, &dts) < 0 ){
				perror("Failed to setup dio_trace_setup");
				return false;
			}else{
				strncpy(pdv->devname, dts.name, strlen(dts.name));
				pdv->stat = SETUP;
				printf(" > setup dts for %s\n", pdv->devname);
			}
		}
	}
	return true;
}

bool start_dts(){
	struct dl_node* p = NULL;
	__foreach_list(p, &dlsth.lsp){
		struct dev_entity* pdv = dl_entry(p, struct dev_entity, link);
	
		if( pdv->stat == SETUP ){
			if( ioctl(pdv->dfd, TRACESTART) < 0 ){
				perror("Failed to start dio_trace_setup");
				return false;
			}
			else{
				pdv->stat = START;
			}
		}
	}
	return true;
}

void stop_dts(){
	struct dl_node* p = NULL;
	__foreach_list(p, &dlsth.lsp){
		struct dev_entity* pdv = dl_entry(p, struct dev_entity, link);

		if( pdv->stat == START ){
			if( ioctl(pdv->dfd, TRACESTOP) < 0 ){
				perror("Failed to stop dio_trace_setup");
			}
			else{
				pdv->stat = STOP;
			}
		}
	}
}

void teardown_dts(){
	struct dl_node* p = NULL;
	__foreach_list(p, &dlsth.lsp){
		struct dev_entity* pdv = dl_entry(p, struct dev_entity, link);

		if( pdv->stat == STOP ){
			if( ioctl(pdv->dfd, TRACETEARDOWN) < 0 ){
				perror("Failed to teardown dio_trace_setup");
			}
			else{
				pdv->stat = TEARDOWN;
			}
		}
	}
}

void add_shark(struct shark_inven* pshk){
	pthread_mutex_lock(&sh.sh_mtx);
	dl_push_back(sh.lsp, pshk->link);
	sh.count++;
	pthread_mutex_unlock(&sh.sh_mtx);
}

void loose_sharks(){
	int i;

	if( !setup_dts() )
		goto err;

	for(i=0; i<sh.totshk; i++){
		if( !loose_shark(i) )
			break;
	}

	if( sh.count != sh.totshk ){
		fprintf(stderr, "some sharks have a problem..\n");
		goto err;
	}

	if( !start_dts() ){
		call_sharks_off();
	}

	gun_fire();
	
	wait_allsharks_done();
err:
	stop_dts();
	teardown_dts();
}

bool loose_shark(int no){
	struct shark_inven* si = NULL;
	int i=0;

	si = (struct shark_inven*)malloc(sizeof(struct shark_inven));

	//init shark inventory
	memset(si, 0, sizeof(struct shark_inven));
	si->shkno = no;
	si->rnflag = true;

	if( pthread_create(&(si->td), NULL, shark_body, si) ){	
		perror("shark can not create his body.");
		return false;
	}

	//add shark to shark head
	add_shark(si);
	
	//wait shark's signal
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
	bool err = false;

	//set dev-cpu input file descriptor
	if( !open_fds(inven) ){
		inven->stat = SHARK_SICK;
		pthread_cond_signal(&s_cond);
		return NULL;
	}

	//set shark's status to ready
	inven->stat = SHARK_READY;
	pthread_cond_signal(&s_cond);

	wait_gunfire();
	
	if( inven->rnflag == false ){
		err = false;
		goto end;
	}

	//set shark's status to working 
	inven->stat = SHARK_WORKING;
	pthread_mutex_lock(&(sh.sh_mtx));
	sh.rnshk++;	//increase running shark count
	pthread_mutex_unlock(&(sh.sh_mtx));

	printf(" > go tracing\n");

	int retevs = 0;
	int dtr_sz = sizeof(struct dio_trace_res);
	int read_sz = 0;
	int zerocnt = 0;
	int i=0;
	struct dio_trace_res dtrbuf;

	while( inven->rnflag){
		memset(&dtrbuf, 0, dtr_sz);
		retevs = poll(inven->pfds,dlsth.count, dlsth.timeout);
		if( retevs == 0 ){ //timeout
			if( !inven->rnflag ){
				err = false;
				goto end;
			}
				
			printf(" > poll timeout\n");
			continue;
		}
		else if(retevs < 0){ //error
			perror("Failed to poll");
			err = true;
			goto end;
		}
		

		for(i=0; i<dlsth.count; i++){
			if( inven->pfds[i].revents & POLLIN ){
				
				read_sz = read(inven->pfds[i].fd, &dtrbuf, dtr_sz);
				if( read_sz == 0){
					zerocnt++;
					printf(" > read zero size\n");
					if( zerocnt >= 100 ){
						err = false;
						goto end;
					}
				}
				else if( read_sz < 0){
					perror("Failed to read");
					err = true;
					goto end;
				}
				printf("read size : %d, seq : %d, time %ld, action %d\n",
					read_sz, dtrbuf.sequence, dtrbuf.time, dtrbuf.action);
			}
		}
	}
	
	//signal done
end:
	inven->rnflag = false;
	clear_fds(inven);

	if( !err )
		inven->stat = SHARK_DONE;
	else
		inven->stat = SHARK_SICK;

	return NULL;
}

void call_sharks_off(){
	struct dl_node* p = NULL;
	__foreach_list(p, &sh.lsp){
		struct shark_inven* pshk = dl_entry(p, struct shark_inven, link);
		pshk->rnflag = false;
	}
}
void gun_fire(){
	while(sh.rnshk != sh.totshk)
		pthread_cond_broadcast(&gf_cond);
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
