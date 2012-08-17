#ifndef DIO_SHARK_H
#define DIO_SHARK_H
/*
	dio_shark.h
	The main tracing source header file.
	
	This file declare core structures and interfaces
	in dio-shark, shark means tracing thread
*/

#include <pthread.h>
#include <stdbool.h>
#include "dio_list.h"

/* defines */

//shark status
enum shark_stat{
	SHARK_READY = 0,	//shark thread is ready
	SHARK_WORKING,
	SHARK_SICK,		//shark thread has problem
	SHARK_DONE		//shark thread done all of works
};

/* structures */
#define MAX_FILENAME_LEN 256
struct io_info{
	int ifd;
	char ifname[MAX_FILENAME_LEN];
};

//shark's personal inventory
struct shark_inven{
	pthread_t td;
	enum shark_stat stat;
	bool rnflag;	//running flag. if it is -1, than stop tracing
	int shkno;	//shark No.
	struct io_info ioinfo;

	struct dl_node link;
};

/* function declares */
extern void loose_sharks();			//dealing all sharks(all tracing thread)
extern bool loose_shark(int no);		//create shark (tracing thread)
extern void wait_gunfire();			//shark(tracing thread) will be waiting a start sign
extern void wait_allsharks_done();

#endif 
