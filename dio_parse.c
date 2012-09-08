/*
	The parser for binary result of dio-shark
	
	This source is free on GNU General Public License.
*/

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <fcntl.h>
#include <stdarg.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "dio_shark.h"
#include "list.h"
#include "rbtree.h"
#include "blktrace_api.h"

/*--------------	struct and defines	------------------*/
#define SECONDS(x)              ((unsigned long long)(x) / 1000000000)
#define NANO_SECONDS(x)         ((unsigned long long)(x) % 1000000000)
#define DOUBLE_TO_NANO_ULL(d)   ((unsigned long long)((d) * 1000000000))

#define BLK_ACTION_STRING		"QMFGSRDCPUTIXBAad"
#define GET_ACTION_CHAR(x)      (0<(x&0xffff) && (x&0xffff)<sizeof(BLK_ACTION_STRING))?BLK_ACTION_STRING[(x & 0xffff) - 1]:'?'

#define BE_TO_LE16(word) \
	(((word)>>8 & 0x00FF) | ((word)<<8 & 0xFF00))

#define BE_TO_LE32(dword) \
	(((dword)>>24 & 0x000000FF) | ((dword)<<24 & 0xFF000000) |\
	((dword)>>8  & 0x0000FF00) | ((dword)<<8  & 0x00FF0000))

#define BE_TO_LE64(qword) \
	(((qword)>>56 & 0x00000000000000FF) | ((qword)<<56 & 0xFF00000000000000) |\
	((qword)>>40 & 0x000000000000FF00) | ((qword)<<40 & 0x00FF000000000000) |\
	((qword)>>24 & 0x0000000000FF0000) | ((qword)<<24 & 0x0000FF0000000000) |\
	((qword)>>8  & 0x00000000FF000000) | ((qword)<<8  & 0x000000FF00000000))

#define BE_TO_LE_BIT(bit) \
	(bit).magic 	= BE_TO_LE32((bit).magic);\
	(bit).sequence 	= BE_TO_LE32((bit).sequence);\
	(bit).time	= BE_TO_LE64((bit).time);\
	(bit).sector	= BE_TO_LE64((bit).sector);\
	(bit).bytes	= BE_TO_LE32((bit).bytes);\
	(bit).action	= BE_TO_LE32((bit).action);\
	(bit).pid	= BE_TO_LE32((bit).pid);\
	(bit).device	= BE_TO_LE32((bit).device);\
	(bit).cpu	= BE_TO_LE32((bit).cpu);\
	(bit).error	= BE_TO_LE16((bit).error);\
	(bit).pdu_len	= BE_TO_LE16((bit).pdu_len)


// dio_rbentity used for handling nuggets as sector order
struct dio_rbentity{
	struct rb_node rblink;		//red black tree link
	struct list_head nghead;	//head of nugget list
	uint64_t sector;
};

// dio_nugget is a treated data of bit
// it will be linked at dio_rbentity 's nghead
#define MAX_ELEMENT_SIZE 50
#define NG_ACTIVE	1
#define NG_BACKMERGE	2
#define NG_FRONTMERGE	3
#define NG_COMPLETE	4
struct dio_nugget{
	struct list_head nglink;	//link of dio_nugget datatype

	//real nugget data
	int elemidx;	//element index. (elemidx-1) is count of nugget states
	int category;
	char states[MAX_ELEMENT_SIZE];	//action
	uint64_t times[MAX_ELEMENT_SIZE];	//states[elemidx] is occured at times[elemidx]
	int size;	//size of nugget
	uint64_t sector;	//sector number of bit who was requested. is it really need?
	uint32_t pid;
	struct dio_nugget* mlink;	//if it was merged, than mlink points the other nugget
	int ngflag;
};

// list node of blk_io_trace
// it just maintain the time ordered bits
struct bit_entity{
	struct list_head link;
	
	struct blk_io_trace bit;
};

struct dio_nugget_path
{
	struct list_head link;

	char states[MAX_ELEMENT_SIZE];
	int count_nugget;
	int count_read;
	int count_write;
	unsigned int total_time;
	unsigned int average_time;
	unsigned int max_time;
	unsigned int min_time;
	int* interval_time;
};

// statistic initialize function.
typedef void(*statistic_init_func)(void);

// statistic traveling function. 
// rb traveling function will give the each nugget as a parameter
typedef void(*statistic_travel_func)(struct dio_nugget*);

// data process function.
typedef void(*statistic_process_func)(int);

// statistic printing function
typedef void(*statistic_print_func)(void);

// statistic clear function. 
// rb traveling function will give a count of nugget as a parameter
typedef void(*statistic_clear_func)(void);
#define MAX_STATISTIC_FUNCTION 10

/*--------------	function interfaces	-----------------------*/
/* function for option and print*/
bool parse_args(int argc, char** argv);
void print_time();
void print_sector();

/* function for bit list */
// insert bit_entity data into rbiten_head order by time
static void insert_proper_pos(struct bit_entity* pbiten);

/* function for rbentity */
//initialize dio_rbentity
static void init_rbentity(struct dio_rbentity* prben);
static struct dio_rbentity* rb_search_entity(uint64_t sector);
static struct dio_rbentity* rb_search_end(uint64_t sec_t);
static struct dio_rbentity* __rb_insert_entity(struct dio_rbentity* prben);
static struct dio_rbentity* rb_insert_entity(struct dio_rbentity* prben);

/* function for nugget */
static void init_nugget(struct dio_nugget* pdng);
static void copy_nugget(struct dio_nugget* destng, struct dio_nugget* srcng);
static struct dio_nugget* FRONT_NUGGET(struct list_head* png_head);

// it return a valid nugget point even if inserted 'sector' doesn't existed in rbtree
// if NULL value is returned, reason is a problem of inserting the new rbentity 
// or memory allocating the new nugget 
static struct dio_nugget* get_nugget_at(uint64_t sector);

// create active nugget on rbtree
// if there isn't rbentity of sector number 'sector', than it create rbentity automatically
// and return the pointer of created nugget
static struct dio_nugget* create_nugget_at(uint64_t sector);

// delete active nugget from rbtree
static void delete_nugget_at(uint64_t sector);

static void extract_nugget(struct blk_io_trace* pbit, struct dio_nugget* pdngbuf);
static void handle_action(uint32_t act, struct dio_nugget* pdng);

// add the statistic function to statistic function table
static void add_statistic_function(statistic_init_func stat_init_fn, 
					statistic_travel_func stat_trv_fn,
					statistic_process_func stat_proc_fn,
					statistic_print_func stat_prt_fn,
					statistic_clear_func stat_clr_fn);

// traveling the rb tree with execution the added statistic functions
static void statistic_rb_traveling();

// statistic for each list entity
static void statistic_list_for_each();

void print_path_statistic(void);
struct dio_nugget_path* find_nugget_path(struct list_head* nugget_path_head, char* states);

/*--------------	global variables	-----------------------*/
#define MAX_FILEPATH_LEN 255
#define PRINT_TYPE_TIME 0
#define PRINT_TYPE_SECTOR 1

static char respath[MAX_FILEPATH_LEN];	//result file path
static int print_type;
static FILE *output;
static __u64 time_start;		/* in nanoseconds */
static __u64 time_end;
static __u64 sector_start;
static __u64 sector_end;

static struct rb_root rben_root;	//root of rbentity tree
static struct list_head biten_head;

static statistic_init_func stat_init_fns[MAX_STATISTIC_FUNCTION];
static statistic_travel_func stat_trv_fns[MAX_STATISTIC_FUNCTION];
static statistic_process_func stat_proc_fns[MAX_STATISTIC_FUNCTION];
static statistic_print_func stat_prt_fns[MAX_STATISTIC_FUNCTION];
static statistic_clear_func stat_clr_fns[MAX_STATISTIC_FUNCTION];
static int stat_fn_cnt = 0;

#define ARG_OPTS "i:o:p:t:s"
static struct option arg_opts[] = {
	{	
		.name = "resfile",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'i',
	},	
	{
		.name = "outfile",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'o'
	}, 
	{
		.name = "print",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'p'
	},
	{
		.name = "time",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 't'
	},
	{
		.name = "sector",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 's'
	}
};

/*--------------	function implementations	---------------*/
int main(int argc, char** argv){
	INIT_LIST_HEAD(&biten_head);
	rben_root = RB_ROOT;

	print_type = PRINT_TYPE_TIME;
	time_start = 0;
	time_end = 0;
	sector_start = 0;
	sector_end = 0;


	int ifd = -1;
	int rdsz = 0;
	char pdubuf[256];

	strncpy(respath, "dioshark.output", MAX_FILEPATH_LEN);

	parse_args(argc, argv);
	ifd = open(respath, O_RDONLY);
	if( ifd < 0 ){
		perror("failed to open result file");
		goto err;
	}
	
	struct bit_entity* pbiten = NULL;
	struct dio_nugget* pdng = NULL;

	int i = 0;
	while(1){
		if( pbiten == NULL ){
			pbiten = (struct bit_entity*)malloc(sizeof(struct bit_entity));
			if( pbiten == NULL ){
				perror("failed to allocate memory");
				goto err;
			}
		}

		rdsz = read(ifd, &(pbiten->bit), sizeof(struct blk_io_trace));
		if( rdsz < 0 ){
			perror("failed to read");
			goto err;
		}
		else if( rdsz == 0 ){
			DBGOUT(">end read\n");
			break;
		}

		//BE_TO_LE_BIT(pbiten->bit);

		//DBGOUT(">pdu_len : %d\n", pbiten->bit.pdu_len);
		if( pbiten->bit.pdu_len > 0 ){
			//rdsz = read(ifd, pdubuf, pbiten->bit.pdu_len);
			//pdubuf[rdsz] = '\0';
			//DBGOUT(">pdu data : %s\n", pdubuf);
			lseek(ifd, pbiten->bit.pdu_len, SEEK_CUR);
		}
		
		if( (pbiten->bit.action >> BLK_TC_SHIFT) == BLK_TC_NOTIFY )
			continue;
			
		//insert into list order by time
		insert_proper_pos(pbiten);

		pbiten = NULL;
	}

	//build up the rbtree order by number of sector
	struct bit_entity* p = NULL;
	uint64_t recentsect = 0;
	list_for_each_entry(p, &biten_head, link){
#if 0
		if( p->bit.sector != 0 )
			recentsect = p->bit.sector;
#endif
		
		pdng = get_nugget_at(p->bit.sector);

		if( pdng == NULL ){
			DBGOUT(">failed to get nugget at sector %llu\n", p->bit.sector);
			goto err;
		}
		extract_nugget(&p->bit, pdng);
	}



	if(output==NULL) {
		output = stdout;
	}

	if(print_type == PRINT_TYPE_TIME) {
		print_time();
	} else if(print_type == PRINT_TYPE_SECTOR) {
		print_sector();
	}

	if(output!=stdout){
		fclose(output);
	}

	print_path_statistic();

	//clean all list entities
	return 0;
err:
	if( ifd < 0 )
		close(ifd);
	if( pbiten != NULL )
		free(pbiten);
	return 0;
}

bool parse_args(int argc, char** argv){
    char tok;
	char *p;
    while( (tok = getopt_long(argc, argv, ARG_OPTS, arg_opts, NULL)) >= 0){
        switch(tok){
			case 'i':
				memset(respath,0,sizeof(char)*MAX_FILEPATH_LEN);
				strcpy(respath,optarg);
				break;
            case 'p':
				if(!strcmp("sector",optarg)) {
					print_type = PRINT_TYPE_SECTOR;
				} else if(!strcmp("time",optarg)) {
					print_type = PRINT_TYPE_TIME;
				} else {
					printf("Print Type Error\n");
					exit(1);
				}
                break;
            case 'o':
				output = fopen(optarg,"w");
				if(output==NULL) {
					printf("Output File Open Error\n");
					exit(1);
				}
                break;
			case 't':
				p = strtok(optarg,",");
				time_start = atoll(p);
				p = strtok(NULL,",");
				time_end = atoll(p);
				break;
			case 's':
				p = strtok(optarg,",");
				sector_start = atoll(p);
				p = strtok(NULL,",");
				sector_end = atoll(p);
				break;
        };
    }


    return true;
}

void insert_proper_pos(struct bit_entity* pbiten){
	struct list_head* p = NULL;
	struct bit_entity* _pbiten = NULL;

	//list foreach back
	for(p = biten_head.prev; p != &(biten_head); p = p->prev){
		_pbiten = list_entry(p, struct bit_entity, link);
		if( _pbiten->bit.time <= pbiten->bit.time ){
			list_add(&(pbiten->link), p);
			return;
		}
	}
	list_add(&(pbiten->link), &(biten_head));
}

static void init_rbentity(struct dio_rbentity* prben){
	memset(prben, 0, sizeof(struct dio_rbentity));
	INIT_LIST_HEAD(&prben->nghead);
	prben->sector = 0;
}

static struct dio_rbentity* rb_search_entity(uint64_t sector){
	struct rb_node* p = rben_root.rb_node;
	struct dio_rbentity* prben = NULL;

	while(p){
		prben = rb_entry(p, struct dio_rbentity, rblink);
		if( sector < prben->sector )
			p = prben->rblink.rb_left;
		else if( sector > prben->sector )
			p = prben->rblink.rb_right;
		else
			return prben;
	}
	return NULL;
}

struct dio_rbentity* rb_search_end(uint64_t sec_t){
	struct rb_node* p = rben_root.rb_node;
	struct dio_rbentity* prben = NULL;
	struct dio_nugget* actng = NULL;
	uint64_t calcsect = 0;

	while(p){
		prben = rb_entry(p, struct dio_rbentity, rblink);
		actng = FRONT_NUGGET(&prben->nghead);
		if( prben->sector != actng->sector ){
			DBGOUT("prben->sector != actng->sector\n");
			return NULL;
		}
		calcsect = actng->sector + actng->size/512;

		if( sec_t < calcsect )
			p = prben->rblink.rb_left;
		else if( sec_t > calcsect )
			p = prben->rblink.rb_right;
		else
			return prben;
	}
	return NULL;
}

struct dio_nugget* FRONT_NUGGET(struct list_head* png_head){
	if( list_empty(png_head) )
		DBGOUT(">>>>>>>>>>>>>>>> list empty\n");

	return list_entry(png_head->next, struct dio_nugget, nglink);
}

static struct dio_rbentity* __rb_insert_entity(struct dio_rbentity* prben){
	struct rb_node** p = &rben_root.rb_node;
	struct rb_node* parent = NULL;
	struct dio_rbentity* prbenbuf = NULL;

	while(*p){
		parent = *p;
		prbenbuf = rb_entry(parent, struct dio_rbentity, rblink);

		if( prben->sector < prbenbuf->sector )
			p = &(*p)->rb_left;
		else if( prben->sector > prbenbuf->sector )
			p = &(*p)->rb_right;
		else
			return prbenbuf;	//there already exists
	}

	rb_link_node(&(prben->rblink), parent, p);
	return NULL;	//success
}

static struct dio_rbentity* rb_insert_entity(struct dio_rbentity* prben){
	struct dio_rbentity* rbenret = NULL;
	if( (rbenret = __rb_insert_entity(prben)) )
		return rbenret;	//there already exists

	rb_insert_color(&(prben->rblink), &(rben_root));
	return NULL;	//insert successfully
}

void init_nugget(struct dio_nugget* pdng){
	memset(pdng, 0, sizeof(struct dio_nugget));
	//pdng->elemidx = 0;
}

void copy_nugget(struct dio_nugget* destng, struct dio_nugget* srcng){
	memcpy(destng, srcng, sizeof(struct dio_nugget));
}

struct dio_nugget* get_nugget_at(uint64_t sector){
	struct dio_nugget* pdng = NULL;
	struct dio_rbentity* prben = NULL;

	prben = rb_search_entity(sector);
	if( prben == NULL ){
		prben = (struct dio_rbentity*)malloc(sizeof(struct dio_rbentity));
		if( prben == NULL){
			DBGOUT("failed to get memory\n");
			return NULL;
		}
		init_rbentity(prben);
		prben->sector = sector;
		if( rb_insert_entity(prben) != NULL ){
			free(prben);
			DBGOUT(">failed to insert rbentity into rbtree\n");
			return NULL;
		}
	}

	//return the first item of nugget list
	if( !list_empty(&prben->nghead) ){
		pdng = FRONT_NUGGET(&prben->nghead);
		if( pdng->ngflag == NG_ACTIVE ){
			return pdng;
		}
	}

	//else if list is empty or first item is inactive
	pdng = NULL;
	pdng = (struct dio_nugget*)malloc(sizeof(struct dio_nugget));
	if( pdng == NULL ){
		perror("failed to allocate nugget memory");
		return NULL;
	}

	init_nugget(pdng);
	pdng->sector = sector;
	pdng->ngflag = NG_ACTIVE;
	list_add(&pdng->nglink, &prben->nghead);

	return pdng;
}

struct dio_nugget* create_nugget_at(uint64_t sector){
	struct dio_rbentity* rben = rb_search_entity(sector);
	if( rben == NULL ){
		rben = (struct dio_rbentity*)malloc(sizeof(struct dio_rbentity));
		if( rben == NULL ){
			perror("failed to allocate rbentity memory");
			return NULL;
		}
		init_rbentity(rben);
		rben->sector = sector;

		rb_insert_entity(rben);
	}

	struct dio_nugget* newng = NULL;
	newng = (struct dio_nugget*)malloc(sizeof(struct dio_nugget));
	if( newng == NULL ){
		perror("failed to allocate nugget memory");
		return NULL;
	}
	init_nugget(newng);
	newng->sector = sector;
	newng->ngflag = NG_ACTIVE;
	list_add(&newng->nglink, &rben->nghead);

	return newng;
}

void delete_nugget_at(uint64_t sector){
	struct dio_rbentity* prben = rb_search_entity(sector);
	if( prben == NULL )
		return;
	
	if( list_empty(&prben->nghead) )
		return;

	struct dio_nugget* del = FRONT_NUGGET(&prben->nghead);
	list_del(prben->nghead.next);
	free(del);
}

void extract_nugget(struct blk_io_trace* pbit, struct dio_nugget* pdngbuf){
	pdngbuf->times[pdngbuf->elemidx] = pbit->time;
	if( pdngbuf->elemidx == 0 ){
		pdngbuf->size = pbit->bytes;
		pdngbuf->pid = pbit->pid;
		pdngbuf->category = pbit->action >> BLK_TC_SHIFT;
	}

	handle_action(pbit->action, pdngbuf);
	pdngbuf->elemidx++;
}

void handle_action(uint32_t act, struct dio_nugget* pdng){
	struct dio_nugget* ptmpng = NULL;
	struct dio_nugget* newng = NULL;
	struct dio_rbentity* prben = NULL;

	char actc = GET_ACTION_CHAR(act);
	pdng->states[pdng->elemidx] = actc;
	DBGOUT("action %x(%c), sector %"PRIu64", size %d\n", act, actc, pdng->sector, pdng->size);
	

	switch(act){
	case 'M':
		//back merged
		prben = rb_search_end(pdng->sector);
		if( prben == NULL ){
			DBGOUT("Failed to search nugget when back merging\n");
			return;
		}
		ptmpng = FRONT_NUGGET(&prben->nghead);
		
		pdng->ngflag = NG_BACKMERGE;
		pdng->mlink = ptmpng;
		ptmpng->size += pdng->size;
		break;

	case 'F':
		//front merged
		newng = create_nugget_at(pdng->sector);
		if( newng == NULL ){
			DBGOUT("Failed to create nugget\n");
			return;
		}
		prben = rb_search_entity(pdng->sector + pdng->size);
		if( prben == NULL ){
			DBGOUT("Failed to search nugget when front merging\n");
			return;
		}
		ptmpng = FRONT_NUGGET(&prben->nghead);
		copy_nugget(newng, ptmpng);

		pdng->ngflag = NG_FRONTMERGE;
		pdng->mlink = ptmpng;
		delete_nugget_at(pdng->sector + pdng->size);
		break;
	case 'C':
		pdng->ngflag = NG_COMPLETE;
		break;
	};
}

void add_statistic_function(statistic_init_func stat_init_fn, 
				statistic_travel_func stat_trv_fn,
				statistic_process_func stat_proc_fn,
				statistic_print_func stat_prt_fn,
				statistic_clear_func stat_clr_fn){
	if( stat_fn_cnt +1 >= MAX_STATISTIC_FUNCTION )
		return;
	
	stat_init_fns[stat_fn_cnt] = stat_init_fn;
	stat_trv_fns[stat_fn_cnt] = stat_trv_fn;
	stat_proc_fns[stat_fn_cnt] = stat_proc_fn;
	stat_prt_fns[stat_fn_cnt] = stat_prt_fn;
	stat_clr_fns[stat_fn_cnt] = stat_clr_fn;
	stat_fn_cnt++;
}

void statistic_rb_traveling(){
	struct rb_node* node;
	int i=0, cnt=0;

	//init all statistic functions
	for(i=0; i<stat_fn_cnt; i++)
		stat_init_fns[i]();
	
	node = rb_first(&rben_root);
	do{
		struct dio_rbentity* prben = NULL;
		prben = rb_entry(node, struct dio_rbentity, rblink);

		struct dio_nugget* pdng = NULL;
		list_for_each_entry(pdng, &prben->nghead, nglink){
			for(i=0; i<stat_fn_cnt; i++)
				stat_trv_fns[i](pdng);
			cnt++;
		}
	}while((node = rb_next(node)) != NULL);

	//process data
	for(i=0; i<stat_fn_cnt; i++)
		stat_proc_fns[i](cnt);

	//print statistic
	for(i=0; i<stat_fn_cnt; i++)
		stat_prt_fns[i]();

	//clear all statistic functions
	for(i=0; i<stat_fn_cnt; i++)
		stat_clr_fns[i]();
}

//------------------- path statistics ------------------------------//
struct list_head nugget_path_head;
struct dio_nugget_path* pnugget_path;

int instr(const char* str1, const char* str2)
{
        int i, j;

        i=0;
        while(str1[i] != '\0')
        {
                j=0;
                while(str2[j] != '\0')
                {
                        if(str1[i+j] != str2[j])
                        {
                                break;
                        }
                        j++;
                }
                if(str2[j] == '\0')
                {
                        return i+1;
                }
                i++;
        }

        return 0;
}

struct dio_nugget_path* find_nugget_path(struct list_head* nugget_path_head, char* states)
{
	struct dio_nugget_path* pdngpath;

	list_for_each_entry(pdngpath, nugget_path_head, link)
	{
		if(strcmp(pdngpath->states, states) == 0)
		{
			return pdngpath;
		}
	}

	return NULL;
}

void print_time() {
	struct bit_entity* p = NULL;

	list_for_each_entry(p, &biten_head, link) {
		if(p->bit.sequence == 0) continue;
		if( (time_start == 0 && time_end ==0) || 
			(time_start > p->bit.time || time_end < p->bit.time)) continue;

		fprintf(output,"%u ",p->bit.sequence);
		fprintf(output,"%5d.%09lu ", (int)SECONDS(p->bit.time), (unsigned long)NANO_SECONDS(p->bit.time));
		fprintf(output,"%llu ",p->bit.sector);
		fprintf(output,"%u ",p->bit.pid);
		fprintf(output,"%u\n",p->bit.bytes);
	}
	
}

void print_sector() {

	struct rb_node *node;

	node = rb_first(&rben_root);

	while((node = rb_next(node)) != NULL) {

		struct list_head* nugget_head;
		struct dio_rbentity* prbentity;

		prbentity = rb_entry(node, struct dio_rbentity, rblink);

		struct dio_nugget* pdng;

		list_for_each_entry(pdng, &(prbentity->nghead), nglink) {
		
			if( (sector_start == 0 && sector_end ==0) || 
				(sector_start > p->bit.time || sector_end < p->bit.time)) continue;
			
			fprintf(output,"%llu \n",pdng->sector);

		}
		

	}
	
}

void init_path_statistic()
{
	INIT_LIST_HEAD(&nugget_path_head);
}

void travel_path_statistic(struct dio_nugget* pdng)
{
	char* 		pstates;
	uint64_t*	ptimes;
	int*		pelemidx;
	int			i;
	int			nugget_time;

	pnugget_path = find_nugget_path(&nugget_path_head, pdng->states);
	if(pnugget_path == NULL)	// if not exist
	{
		pnugget_path = (struct dio_nugget_path*)malloc(sizeof(struct dio_nugget_path));

		// Init pnugget_path's members
		memset(pnugget_path, 0, sizeof(struct dio_nugget_path));
		pnugget_path->min_time = -1;
		strncpy(pnugget_path->states, pdng->states, MAX_ELEMENT_SIZE);

		// Add list
		list_add(&(pnugget_path->link), &nugget_path_head);
	}

	// Add read/write count to distribute those.
	if(pdng->category & BLK_TC_READ)
	{
		pnugget_path->count_read++;
	}
	if(pdng->category & BLK_TC_WRITE)
	{
		pnugget_path->count_write++;
	}

	// Set data on pnugget_path.
	pnugget_path->count_nugget++;
	nugget_time = pdng->times[pdng->elemidx] - pdng->times[0];
	pnugget_path->total_time += nugget_time;
	if(pnugget_path->max_time < nugget_time)
	{
		pnugget_path->max_time = nugget_time;
	}
	if(pnugget_path->min_time > nugget_time)
	{
		pnugget_path->min_time = nugget_time;
	}
}


void process_path_statistic(int ng_cnt)
{
	// Calculate average time.
	list_for_each_entry(pnugget_path, &nugget_path_head, link)
	{
		pnugget_path->average_time = pnugget_path->total_time / pnugget_path->count_nugget;
	}
}

void print_path_statistic(void)
{
	printf("%20s %8s %8s %4s %12s %12s %12s \n", " ", "횟수", "읽기횟수", "쓰기횟수", "평균수행시간", "최대수행시간", "최소수행시간");
	list_for_each_entry(pnugget_path, &nugget_path_head, link)
	{
		if(instr(pnugget_path->states, "P") || instr(pnugget_path->states, "U") || instr(pnugget_path->states, "?"))
		{
			continue;
		}

		printf("%20s %4d %8d %8d %2llu:%.9llu %2llu:%.9llu %2llu:%.9llu \n", pnugget_path->states, pnugget_path->count_nugget,
			pnugget_path->count_read, pnugget_path->count_write,
			SECONDS(pnugget_path->average_time), NANO_SECONDS(pnugget_path->average_time),
			SECONDS(pnugget_path->max_time), NANO_SECONDS(pnugget_path->max_time),
			SECONDS(pnugget_path->min_time), NANO_SECONDS(pnugget_path->min_time)
		);

	}
}

void clear_path_statistic(void)
{
	struct dio_nugget_path* tmpdng_path;

	// Free all dynamic allocated variables.
	list_for_each_entry_safe(pnugget_path, tmpdng_path, &nugget_path_head, link)
	{
		list_del(&pnugget_path->link);
		free(pnugget_path);
	}
}
//------------------- section statistics (for example)------------------------------//
#define MAX_MON_SECTION 10
static char mon_section[MAX_MON_SECTION][2];
static uint64_t mon_sec_time[MAX_MON_SECTION];
static int mon_sec_cnt[MAX_MON_SECTION];
static int mon_cnt = 0;

void add_monitored_section(char section[2]){
	if( mon_cnt+1 >= MAX_MON_SECTION )
		return;
	
	memcpy(mon_section[mon_cnt], section, 2);
	mon_cnt++;
}

int find_section(char* states, int mon_sec_num){
	if( mon_sec_num >= mon_cnt )
		return -1;

	int i=0;
	for(; i<MAX_ELEMENT_SIZE-1; i++){
		if( states[i] == 0 )
			break;
		if( states[i] == mon_section[mon_sec_num][0] &&
			states[i+1] == mon_section[mon_sec_num][1] )
			return i;
	}
	return -1;
}
			
void init_section_statistic(){
	memset(mon_section, 0, sizeof(char)*MAX_MON_SECTION*2);
	memset(mon_sec_time, 0, sizeof(uint64_t)*MAX_MON_SECTION);
	memset(mon_sec_cnt, 0, sizeof(mon_sec_cnt));
	mon_cnt = 0;
}

void travel_section_statistic(struct dio_nugget* pdng){
	int i=0, pos=0;
	for(; i<mon_cnt; i++){
		pos = find_section(pdng->states, i);
		if( i == -1 )
			continue;
		
		mon_sec_time[i] += (pdng->times[pos+1] - pdng->times[pos]);
		mon_sec_cnt[i] ++;
	}
}

void process_section_statistic(int ng_cnt){
	int i=0;
	for(; i<mon_cnt; i++){
		//calculate the average spending time for each section
		mon_sec_time[i] /= mon_sec_cnt[i];
	}
}

void clear_section_statistic(){
}


//---------------------------------------- pid statistic -------------------------------------------------//
//global variables (and data structure) for pid statistic
struct pid_stat_data{
	struct rb_node link;

	uint32_t pid;
	uint64_t r_mint, r_maxt, r_tott, r_avgt;	//read min time, read max time, reat total time
	uint64_t w_mint, w_maxt, w_tott, w_avgt;	//write min time, write max time, write total time
	int r_cnt, w_cnt;	//count of occur
};

static struct rb_root psd_root = RB_ROOT;	//pid stat data root

//function for handling data structure for pid statistic
static struct pid_stat_data* rb_search_psd(uint32_t pid){
	struct rb_node* n = psd_root.rb_node;
	struct pid_stat_data* ppsd = NULL;
	
	while(n){
		ppsd = rb_entry(n, struct pid_stat_data, link);

		if( pid < ppsd->pid )
			n = n->rb_left;
		else if( pid > ppsd->pid )
			n = n->rb_right;
		else 
			return ppsd;
	}
	return NULL;
}

static struct pid_stat_data* __rb_insert_psd(struct pid_stat_data* newpsd){
	struct pid_stat_data* ret;
	struct rb_node** p = &(psd_root.rb_node);
	struct rb_node* parent = NULL;
	
	while(*p){
		parent = *p;
		ret = rb_entry(parent, struct pid_stat_data, link);

		if( newpsd->pid < ret->pid )
			p = &(*p)->rb_left;
		else if( newpsd->pid > ret->pid )
			p = &(*p)->rb_right;
		else
			return ret;
	}

	rb_link_node(&newpsd->link, parent, p);
	return NULL;
}

static struct pid_stat_data* rb_insert_psd(struct pid_stat_data* newpsd){
	struct pid_stat_data* ret = NULL;
	if( (ret = __rb_insert_psd(newpsd) ) )
		return ret;
	rb_insert_color(&newpsd->link, &psd_root);
	return ret;
}

void init_pid_statistic(){
}

void travel_pid_statistic(struct dio_nugget* pdng){
	struct pid_stat_data* ppsd = rb_search_psd(pdng->pid);
	if( ppsd == NULL ){
		ppsd = (struct pid_stat_data*)malloc(sizeof(struct pid_stat_data));
		ppsd->pid = pdng->pid;
		ppsd->r_mint = ppsd->w_mint = (uint64_t)(-1);
		ppsd->r_maxt = ppsd->w_maxt = 0;
		ppsd->r_tott = ppsd->w_tott = 0;
		ppsd->r_avgt = ppsd->w_avgt = 0;
		ppsd->r_cnt = ppsd->w_cnt = 0;
		
		rb_insert_psd(ppsd);
	}
	
	uint64_t tmpt = 0;
	if( pdng->category & BLK_TC_READ ){
		tmpt = pdng->times[pdng->elemidx-1] - pdng->times[0];
		if( ppsd->r_mint > tmpt )
			ppsd->r_mint = tmpt;
		else if( ppsd->r_maxt < tmpt )
			ppsd->r_maxt = tmpt;
		ppsd->r_tott += tmpt;
		ppsd->r_cnt ++;
	}
	else if( pdng->category & BLK_TC_WRITE ){
		tmpt = pdng->times[pdng->elemidx-1] - pdng->times[0];
		if( ppsd->w_mint > tmpt )
			ppsd->w_mint = tmpt;
		else if( ppsd->w_maxt < tmpt )
			ppsd->w_maxt = tmpt;
		ppsd->w_tott = tmpt;
		ppsd->w_cnt ++;
	}
}

void process_pid_statistic(int ng_cnt){
	struct rb_node* node = NULL;
	node = rb_first(&psd_root);
	do{
		struct pid_stat_data* ppsd = NULL;
		ppsd = rb_entry(node, struct pid_stat_data, link);
		
		ppsd->r_avgt = ppsd->r_tott / ppsd->r_cnt;
		ppsd->w_avgt = ppsd->w_tott / ppsd->w_cnt;
	}while( (node = rb_next(node)) != NULL );
}

void print_pid_statistic(){
	struct rb_node* node = NULL;
	node = rb_first(&psd_root);
	do{
		struct pid_stat_data* ppsd = NULL;
		ppsd = rb_entry(node, struct pid_stat_data, link);

		//print data
	}while( (node = rb_next(node)) != NULL );
}

static void __clear_pid_stat(struct rb_node* p){
	if( p->rb_left != NULL )
		__clear_pid_stat(p->rb_left);
	if( p->rb_right != NULL )
		__clear_pid_stat(p->rb_right);
	
	struct pid_stat_data* psd = rb_entry(p, struct pid_stat_data, link);
	free(psd);
}

void clear_pid_statistic(){
	struct rb_node* parent = psd_root.rb_node;
	__clear_pid_stat(parent);
}

