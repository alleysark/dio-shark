#ifndef DIO_LIST_H
#define DIO_LIST_H

/*
	dio_list.h
	This contains declaration and implementation about double linked list structure
	dio_list is not general linked list, it just use for handling dio-shark tracing data

	frequently used variable names
		pdlh : it indicates pointer of dio list head
		pdln : it indicates pointer of dio list node
*/

// dio list entity
// contains : 	next, prev pointer to moving
struct dl_node{
	struct dl_node *nxt, *prv;
};


/* list manipulating functions */

// initializing dio list node
// this is only for internal list manapulation
// @param pdln :	the pointer of list node
#define __init_dl_node(pdln)\
		pdln->nxt = NULL;\
		pdln->prv = NULL;\

// initializing dio list head
// @param pdlh : 	the pointer of list head
#define INIT_DL_HEAD(pdlh) \
		(pdlh)->nxt = (pdlh);\
		(pdlh)->prv = (pdlh);\

// get offset of member in type
// @param type : 	type contains the member
// @param member :	member name which you want to know offset
#define offsetof(type, member) ((intptr_t)(&((type*)0)->member))

// get dio-list entry
#define dl_entry(pdln, type, member) ({\
		typeof(((type*)0)->member)* __mptr = (pdln);\
		(type*)((char*)__mptr - offsetof(type, member));})
		

// foreach macro to iterating dio list
// @param container :	pointer of iterator
// @param pdlh :	pointer of list head
#define __foreach_list(container, pdlh) \
		for( (container) = (pdlh)->nxt; (container) != (pdlh); (container) = (container)->nxt )

	
// insert node macro.
// this is only for internal list manapulation
// @param node_ptr :	dl_node pointer to inserted
// @param before :	dl_node pointer which indicates where to insertion
// @param after :	dl_node pointer which indicates where to insertion
#define __insert_node(node_ptr, after, before)\
		(node_ptr)->prv = (after);\
		(after)->nxt = (node_ptr);\
		(node_ptr)->nxt = (before);\
		(before)->prv = (node_ptr);

// push back the new node
// @param dlh :		the dio list head which is the type of dl_node
// @param dln :		the dl_node value you want to push
#define dl_push_back(dlh, dln) \
		__insert_node( &(dln), (dlh.prv), &(dlh));


/*
// insert data into the index of 'idx'
// @param pdlh :	the pointer of list head
// @param pdata :	the pointer of a data which wants to be inserted
// @param idx :		the index which the data is inerted
//
// @return :		return true when successfully inserted
static bool insert_data(struct dl_head* pdlh, void* pdata, int idx){
	struct dl_node* after = NULL;
	if( idx == 0)
		after = &(pdlh->head);
	else if( idx == pdlh->length )
		after = pdlh->head.prv;
	else
		after = search_at(pdlh, idx-1);

	if( after == NULL )
		return false;
	
	struct dl_node* pdlnbuf = __create_list_node(pdata);
	
	__insert_node(pdlnbuf, after, after->nxt);

	pdlh->length++;
	return true;
}

// push back to dio list
// this function same witch insert_data(pdlh, pdata, pdlh->length)
// @param pdlh:		the pointer of list head
// @param pdata :	the pointer of a data which wants to be inserted
static void push_back_data(struct dl_head* pdlh, void* pdata){
	struct dl_node* pdlnbuf = __create_list_node(pdata);

	__insert_node(pdlnbuf, pdlh->head.prv, &(pdlh->head));
	pdlh->length++;
}
*/

#endif
