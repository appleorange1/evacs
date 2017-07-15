/* This file is (C) copyright 2001 Software Improvements, Pty Ltd */

/* This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

/* for mkdir */
#include <sys/stat.h>
#include <sys/types.h>
/* for rmdir */
#include <unistd.h>
/* for opendir */        
#include <dirent.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <libpq-fe.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>
/* SIPL 2011: This file defines sprintf_malloc(),
   SQL_singleton(),
   produce_collapsed_map(),
   so include the prototypes. */
#define DEFINE_SPRINTF_MALLOC
#define DEFINE_SQL_SINGLETON
#define DEFINE_PRODUCE_COLLAPSED_MAP
#include "export_ballots.h"

#define OUTPUTROOT "/tmp/evacs_export"
#define OUTPUTDIR  "confirmed_votes"

/* Dump paper votes to CSV files in these formats

Formal Votes per electorate:
Batch Paper Pref Party Candidate  Candidate Screen
No   ,Index,No  ,Code ,Code ,        Index

Informal Votes per Electorate:
Batch Internal Vote
No   ,     Id

Paper versions per Electorate:
Batch electorate, Paper Paper
No   ,code       ,Index,Version

Parties:
Electorate Party Party Party 
Code      ,Code ,Name ,Abbreviation

Candidates:
Electorate Party Candidate Candidate
Code      ,Code ,Code     ,Name

*/

/* SIPL 2014-05-20 Electorate names may now contain spaces, hyphens,
   apostrophes.  This is a problem, when such names are embedded in
   identifiers and directory names. For example, there are database
   tables with names that contain an electorate name within them.  The
   convention in eVACS is to normalize such names by replacing all
   non-alphanumeric characters with underscores. Use this function to
   normalize an electorate name. The target string "to" must point to
   a buffer of at least the same size as the source string "from".

   Copied from common/evacs.c.
*/
static void normalize_electorate_name(char *to, const char *from);
static void normalize_electorate_name(char *to, const char *from)
{
	while (*from) {
		if (
		    ((*from >= 'A') && (*from <= 'Z')) ||
		    ((*from >= 'a') && (*from <= 'z')) ||
		    ((*from >= '0') && (*from <= '9'))
		    )
			*to = *from;
		else
			*to = '_';
		from++;
		to++;
	}
	*to = '\0';
}

void bailout(const char *fmt, ...)
     /*
       Print a message and exit fatally.
     */
{
  va_list arglist;

  va_start(arglist,fmt);
 
  /* Default handler */
  fprintf(stderr,"FATAL: ");
  vfprintf(stderr,fmt,arglist);

  va_end(arglist);
  
  exit(1);
}

static char *sprintf_malloc(const char *fmt, ...)
     /*
       Variable number of parameter interface to vsprintf_malloc().
     */
     
{
  char *str;
  va_list arglist;
  
  va_start(arglist,fmt);
  str = vsprintf_malloc(fmt,arglist);
  va_end(arglist);
  
  return(str);
}

PGconn *connect_db_host(const char *name, const char *hostname)
{
        PGconn *conn;
 	conn = PQsetdb(hostname, NULL, NULL, NULL, name);
	if (PQstatus(conn) != CONNECTION_OK) {
		PQfinish(conn);
		return NULL;
	}
	return conn;
}


char *vsprintf_malloc(const char *fmt, va_list arglist)
     /*
       This is a self-allocating vsprintf function.
       
       NOTE: This function allocates memory. It is
       the callers responsibility to free it after
       use. Also, this function does not call va_end.
       It is the callers responsibility to do this.
      */

{
  char *str=NULL;

  /* Allocate amount of space indicated by vsnprintf */

  str = (char *)malloc(vsnprintf(str,0,fmt,arglist)+1);

  /* Write into string for real this time */

  vsprintf(str,fmt,arglist);
  return(str);
}


static PGresult *run_SQL(PGconn *conn, bool bail_on_error,
			 const char *fmt,va_list arglist);
static PGresult *run_SQL(PGconn *conn, bool bail_on_error,
			 const char *fmt,va_list arglist)

     /*
       Execute the query against the database open on conn.
       
       Bailout if a fatal error or bad response is returned and
       bail_on_error is true.

       Should NOT be called directly. Use SQL_query or SQL_command
       interface ONLY!
     */
{
        char *sql;

	PGresult *result;
	ExecStatusType result_status;
	sql = vsprintf_malloc(fmt,arglist);

	result = PQexec(conn,sql);
	result_status = PQresultStatus(result);
	
	if ( bail_on_error && result_status != PGRES_COMMAND_OK &&
	     result_status != PGRES_TUPLES_OK)
	        bailout("SQL command %s failed: %s (%s)\n",
			sql, PQresStatus(PQresultStatus(result)),
			PQresultErrorMessage(result));
	free(sql);
	return(result);
}

static char *SQL_single(PGconn *conn,const char *fmt,va_list arglist) 
/*
  Run the SQL query. Return the pointer to the first field of the
  first row returned. This memory is malloced so the caller must
  free. 
  
  It will bail if more that one row is returned from the
  query, or if zero or more than one field is returned.
  
  If zero rows are returned the function will return NULL.
  
  This should not be called directly, use SQL_singleton or 
  SQL_singleton_int.
*/
{
	char *ret=NULL;
 	PGresult *result;
 	unsigned int num_rows, num_fields;
	
 	result = run_SQL(conn,true,fmt,arglist);
	
 	num_rows = PQntuples(result);
 	num_fields = PQnfields(result);
	
 	if ( num_rows > 1)
 	        bailout("SQL_single %s returned %d rows!\n",
 			fmt,num_rows);
	
 	if (num_fields != 1)/*Should be one field*/ 
				    bailout("SQL_single %s returned %d fields!\n",
					    fmt,num_fields);
	
 	if (num_rows == 1)
		ret = strdup(PQgetvalue(result,0,0));
	
 	PQclear(result);
	
 	return(ret);
}

/*
  Run the SQL query. Return the pointer to the PGresult structure.
*/
PGresult *SQL_query(PGconn *conn,const char *fmt, ...)
{
        va_list arglist;
	PGresult *result;

	va_start(arglist,fmt);
	result = run_SQL(conn,true,fmt,arglist);
	va_end(arglist);

	return(result);
}

char *SQL_singleton(PGconn *conn,const char *fmt, ...)

/*       Run the SQL query and return the single field text result from the
	 one row returned. Return NULL if no row returned.*/
{
	va_list arglist;
 	char *ret;
	
 	va_start(arglist,fmt);
 	ret = SQL_single(conn,fmt,arglist);
 	va_end(arglist);
	
 	return(ret);
}

struct rotation *fetch_rotation(PGconn *conn,
 				unsigned int rotation_num,
 				unsigned int seat_count)
{
 	struct rotation *rot;
 	char *rotstring;
	/* SIPL 2014-03-18 Additional variables to support
	   arbitrary number of positions in the rotation. */
	char *rotstring_cursor;
	int rotation_index;
	int sscanf_offset;
	
  	/*Get the rotation */
 	rotstring = SQL_singleton(conn,
				  "SELECT rotation "
				  "FROM robson_rotation_%u "
				  "WHERE rotation_num = %u;",
				  seat_count, rotation_num);
 	if (rotstring == NULL)
 		return NULL;
	
 	rot = malloc(sizeof(*rot));
 	rot->size = seat_count;
	
	/* 2014-03-18 Support an arbitrary (> 1) number of positions
	   in the rotation.
	   See also fetch_rotation() in voting_server/fetch_rotation.c
	   and tools/export_confirmed.c.
	*/
	/* rotation is in the form {n,n,n,n,n} */
	rotstring_cursor = rotstring;
	sscanf(rotstring_cursor, "{%u%n", &rot->rotations[0],
	       &sscanf_offset);
	rotstring_cursor += sscanf_offset;
	rotation_index = 1;
	while ((rotation_index < seat_count) &&
	       (sscanf(rotstring_cursor, ",%u%n",
		       &rot->rotations[rotation_index],
		       &sscanf_offset) == 1)) {
		rotstring_cursor += sscanf_offset;
		rotation_index++;
	}
	
 	free(rotstring);
 	return rot;
}

  /* Create a mapping for the rotation, collapsed to this number of 
     candidates */  
 static void produce_collapsed_map(const struct rotation *rot,
 				  unsigned int map[],
 				  unsigned int num_candidates)
 {
 	unsigned int i,j=0;

/*  	run through each robson rotation position 
	adding the value to the map if there is a candidate 
	at that position*/
	
 	for (i = 0; i < rot->size; i++) {
		/*is there a candidate for this position? */
		if (rot->rotations[i] < num_candidates) {
			/*yes, add it to the map */
				map[j]=rot->rotations[i];
 	                j++;
		}
 	}
 } 


/* SIPL 2011-07-15 Commented out so that it will not be used. */
//  unsigned int translate_dbci_to_sci(unsigned int num_candidates, 
//				     unsigned int db_candidate_index,
//				     const struct rotation *rot)
//{
// 	unsigned int map[MAX_ELECTORATE_SEATS];
// 	unsigned int i;
//	
// 	assert(num_candidates <= rot->size);
// 	assert(db_candidate_index < rot->size);
// 	assert(db_candidate_index < num_candidates);
//	
// 	produce_collapsed_map(rot, map, num_candidates);
//	
//  	/*Look for the screen candidate index which maps onto this 
//	  db_candidate_index */
//	for (i = 0; map[i] != db_candidate_index; i++)
//		assert(i < num_candidates);
//	
// 	return i;
// }


/* SIPL 2011-07-15 New implementation of this key function that
   supports split groups. Copied and modified from the version
   written for common/cursor.c. */
static unsigned int translate_group_dbci_to_sci(
				   struct electorate *electorate,
				   unsigned int group_index,
				   unsigned int db_candidate_index,
				   const struct rotation *rot)
{
	unsigned int map[MAX_ELECTORATE_SEATS];
	unsigned int i;

	unsigned int physical_column_index;
	/* If the group is split into multiple columns, the
	   sci and dbci values of a candidate _not_ in the
	   first physical column of the group are offset by the
	   total number of candidates in all of the preceding
	   physical columns for the same group.  The variable
	   candidates_offset is used to keep track of that total.
	 */
	unsigned int candidates_offset = 0;

	unsigned int num_candidates;

	/* Skip to the appropriate physical column. */
	physical_column_index =
		electorate->map_group_to_physical_column[group_index];
	while (db_candidate_index >=
	       electorate->num_candidates_in_physical_column[
						physical_column_index]) {
		db_candidate_index = db_candidate_index -
			electorate->num_candidates_in_physical_column[
					      physical_column_index];
		candidates_offset = candidates_offset +
			electorate->num_candidates_in_physical_column[
					      physical_column_index];
		physical_column_index++;
	}
	num_candidates = electorate->num_candidates_in_physical_column[
				physical_column_index];

	assert(num_candidates <= rot->size);
	assert(db_candidate_index < rot->size);
	assert(db_candidate_index < num_candidates);

	produce_collapsed_map(rot, map, num_candidates);

	/* Look for the screen candidate index which maps onto this
	   db_candidate_index */
	for (i = 0; map[i] != db_candidate_index; i++)
		assert(i < num_candidates);

	/* Restore i to the appropriate range of values using
	   candidates_offset. */
	return i + candidates_offset;
}

/* SIPL 2011-07-15 To support split groups, must now load the
   full ballot details. This function is a modified version of
   that vound in voting_server authenticate.c. */
static void get_ballot_contents(PGconn *conn,unsigned int ecode,
				struct electorate *electorate)
{
	PGresult *result;
	unsigned int group;
	unsigned num_splits;
	unsigned int i;
	/* Data extracted from one split query result. */
	unsigned int split_party_index, split_candidate_count;
	/* The previous value of the first of the above. */
	unsigned int last_split_party_index;
	unsigned int split_candidate_count_remaining;

	result = SQL_query(conn,
			   "SELECT party_index,count(index) FROM candidate "
			   "WHERE electorate_code = %u "
			   "GROUP BY party_index;",ecode);

	if ( (electorate->num_groups = PQntuples(result)) == 0 )
	  bailout("get_ballot_contents failed. "
		  "No groups found for this electorate.\n");

	electorate->num_candidates =
		malloc(electorate->num_groups*sizeof(unsigned int));
	if (!electorate->num_candidates) {
	  bailout("get_ballot_contents failed. "
		  "malloc() failed.\n");
	}

	/* Allocate space for map.  Note that the length is num_groups+1,
	   not num_groups.
	   The last entry in the map is a dummy entry that points to
	   the location after the end of the
	   num_candidates_in_physical_column array.
	   For example, if there are 5 groups,
	   and 8 physical columns,
	   map_group_to_physical_column[5] == 8.
	   See the comments in export_ballots.h.
	 */
	electorate->map_group_to_physical_column =
		malloc((electorate->num_groups+1)*sizeof(unsigned int));
	if (!electorate->map_group_to_physical_column) {
		bailout("get_ballot_contents failed. "
			"malloc() failed.\n");
	}

	for (group=0;group<electorate->num_groups;group++) {
		electorate->num_candidates[atoi(PQgetvalue(result,group,0))] =
		  atoi(PQgetvalue(result,group,1));
	}

	PQclear(result);

	/* Now add any details about split groups. */
	/* Fetch all split data. Note ORDER BY clause, required to
	   ensure splits are in sequence. */
	result = SQL_query(conn,
			   "SELECT party_index,physical_column_index,"
			   "candidate_count FROM column_splits "
			   "WHERE electorate_code = %u "
			   "ORDER BY party_index, physical_column_index;",
			   ecode);
	num_splits = PQntuples(result);

	electorate->num_physical_columns = electorate->num_groups + num_splits;

	electorate->num_candidates_in_physical_column =
	    malloc(electorate->num_physical_columns*sizeof(unsigned int));
	if (!electorate->num_candidates_in_physical_column) {
		bailout("get_ballot_contents failed. "
			"malloc() failed.\n");
	}

	if (num_splits == 0) {
	    /* No group has been split, so copy the group size
	       data across. */
	    for (i = 0; i < electorate->num_groups; i++) {
		electorate->map_group_to_physical_column[i] = i;
		electorate->num_candidates_in_physical_column[i] =
		    electorate->num_candidates[i];
	    }
	} else {
	    /* Extract details of split groups and fill in the
	       arrays accordingly. */

	    /* Iterator over "split" query results. */
	    unsigned int split_index = 0;
	    /* Iterator over num_candidates_in_physical_column[]. */
	    unsigned int physical_column_index = 0;
	    /* Iterator over num_candidates[]. */
	    unsigned int group_index = 0;

	    /* Get first split. */
	    split_party_index = atoi(PQgetvalue(result,0,0));
	    split_candidate_count = atoi(PQgetvalue(result,0,2));
	    split_candidate_count_remaining =
		electorate->num_candidates[split_party_index];

	    while (physical_column_index < electorate->num_physical_columns) {
		/* Copy data until the next split group. */
		while (group_index < split_party_index) {
		    electorate->map_group_to_physical_column[group_index] =
			physical_column_index;
		    electorate->num_candidates_in_physical_column[
				physical_column_index] =
			electorate->num_candidates[group_index];
		    group_index++;
		    physical_column_index++;
		}

		/* Is there a split group to process? */
		if (split_party_index < electorate->num_groups) {
		    electorate->map_group_to_physical_column[group_index] =
			physical_column_index;
		    do {
			/* This loop fills in all but the last of the physical
			   columns for this split group. */
			electorate->num_candidates_in_physical_column[
				physical_column_index] =
			    split_candidate_count;
			split_candidate_count_remaining =
			    split_candidate_count_remaining -
			    split_candidate_count;
			physical_column_index++;

			/* Get next split data. */

			last_split_party_index = split_party_index;

			split_index++;
			if (split_index < num_splits) {
				split_party_index =
					atoi(PQgetvalue(result,split_index,0));
				split_candidate_count =
					atoi(PQgetvalue(result,split_index,2));
			}
		    } while ((split_index < num_splits) &&
			     (split_party_index == last_split_party_index));
		    /* Fill in the remaining physical column
		       for this split group. */
		    electorate->num_candidates_in_physical_column[
				physical_column_index] =
			split_candidate_count_remaining;
		    physical_column_index++;
		    group_index++;
		    if (split_party_index != last_split_party_index)
			    /* New group to be split; reset count of
			       remaining candidates. */
			    split_candidate_count_remaining =
				electorate->num_candidates[split_party_index];

		    /* If no more split data, set split_party_index
		       so that the first inner loop will
		       go round to complete
		       filling in the arrays. */
		    if (split_index == num_splits)
			split_party_index = electorate->num_groups;
		}
	    }
	}

	/* Fill in the dummy entry at the end. */
	electorate->map_group_to_physical_column[electorate->num_groups] =
		electorate->num_physical_columns;

	PQclear(result);
}



void fetch_electorates(PGconn *conn, struct electorate *electorates[0])
{
	struct electorate *temp;
	int i;
	PGresult *result;

	result = SQL_query(conn,
			   "SELECT code, seat_count, name FROM electorate "
			   "ORDER BY code;");

	for (i=0; i < PQntuples(result); i++) {
		temp = malloc(sizeof(*temp) + 
			      strlen(PQgetvalue(result, i, 2)) + 1);
		temp->code = atoi(PQgetvalue(result, i, 0));
		temp->num_seats = atoi(PQgetvalue(result, i, 1));
		strcpy(temp->name, PQgetvalue(result, i, 2));
		electorates[i] = temp;
		/* SIPL 2011-07-15 Fill in remaining details of the
		   electorate. */
		get_ballot_contents(conn, temp->code, temp);
	}
	electorates[PQntuples(result)]=NULL;
	PQclear(result);
}

void free_electorates(struct electorate *elec[])
{
	unsigned int i;
	for (i=0; elec[i]; i++) 
	{
		free(elec[i]);
	}
}

/* void free_pref_set(struct preference_set *pref_set) */
/* { */
/* 	unsigned int i; */
/* 	for (i=0;pref_set->candidates[i].prefnum > 0; i++) */
/* 	{ */
/* 		free(pref_set->candidates[i]);	 */
/* 	} */
/* 	free(pref_set); */
/* } */


struct preference_set *unpack_preferences(const char *preference_list)
{
	struct preference_set *vote;
	char *pref_ptr;   
	unsigned int num_preferences=0,i;
	unsigned int pref_number, group_index, db_cand_index;

	for (pref_ptr=(char *)preference_list;
	     strlen(pref_ptr)>=DIGITS_PER_PREF;
	     pref_ptr += DIGITS_PER_PREF*sizeof(char),num_preferences++);
	
	if ( strlen(pref_ptr)) 
		bailout("Malformed preference list: '%s'\n",preference_list);
	
	vote = malloc(sizeof(*vote)
			+ sizeof( vote->candidates[0]) * num_preferences);
	vote->num_preferences = num_preferences;
	
	/* They may not be in order */
	for (pref_ptr=(char *)preference_list, i = 0;
	     i < vote->num_preferences; 
	     i++,pref_ptr += DIGITS_PER_PREF*sizeof(char) )
	{
		sscanf(pref_ptr,"%2u%2u%2u",&pref_number,&group_index,&db_cand_index);
	
		vote->candidates[pref_number-1].prefnum=pref_number;
		vote->candidates[pref_number-1]
			.group_index = group_index;
		vote->candidates[pref_number-1]
			.db_candidate_index = db_cand_index;
	}
	
	return vote;
	
}

/* fill in all the groups */
void write_groups(PGconn *conn, const char *filename)
{
	PGresult *result;
	unsigned int i;
	FILE *fh;
	char *path=sprintf_malloc("%s/%s/%s",OUTPUTROOT,OUTPUTDIR,filename);

	/* open file and print header */
	fh = fopen(path,"w");
	if (!fh) bailout("Can't open %s for writing\n",path);
	free(path);

	fprintf(fh,"\"Electorate Code\", \"Party Code\", "
		   "\"Party Name\", \"Party Abbreviation\"\r\n");

	result = SQL_query(conn,
			   "SELECT electorate_code, index, name, abbreviation FROM party "
			   "ORDER by electorate_code, index;");
	for (i = 0; i < PQntuples(result); i++) {
		fprintf(fh, "\"%i\",\"%i\",\"%s\",\"%s\"\r\n",
			atoi(PQgetvalue(result,i,0)),
			atoi(PQgetvalue(result,i,1)),
			PQgetvalue(result,i,2),
			PQgetvalue(result,i,3)
			);
	}
	PQclear(result);
	fclose(fh);
}

/* Given the group information, return the candidate list */
void write_candidates(PGconn *conn, const char *filename, unsigned int num_in_group[0]) 
{
	unsigned int i, elec, prev_elec, prev_group, this_group, candidate_counter,ix;
	PGresult *result;
	FILE *fh;
	char *path=sprintf_malloc("%s/%s/%s",OUTPUTROOT,OUTPUTDIR,filename);

	/* open file and print header */
	fh = fopen(path,"w");
	if (!fh) bailout("Can't open %s for writing\n",path);
	fprintf(fh,"\"Electorate Code\", \"Party Code\", "
		   "\"Candidate Code\", \"Candidate Name\"\r\n");
	free(path);

	result = SQL_query(conn,
			   "SELECT electorate_code, party_index, index, name "
			   "FROM candidate "
			   "ORDER BY electorate_code, party_index, index;");

        candidate_counter=0;
        prev_group=atoi(PQgetvalue(result,0,1));
	prev_elec=atoi(PQgetvalue(result,0,0));;

	for (i = 0; i < PQntuples(result); i++) {
		elec=atoi(PQgetvalue(result,i,0));

		this_group=atoi(PQgetvalue(result,i,1));
		
		fprintf(fh, "\"%i\",\"%i\",\"%i\",\"%s\"\r\n",
			elec,
			this_group,
			atoi(PQgetvalue(result,i,2)),
			PQgetvalue(result,i,3)
			);
		/* SIPL 2011-06-03 The next line only tested
		 (this_group == prev_group).  This assumed that
		 there were at least two groups in each electorate. */
		if ((elec == prev_elec) && (this_group == prev_group))
			candidate_counter++;
		else
		{
			ix = (prev_elec-1)*(MAX_GROUPS) + prev_group;
		        num_in_group[ix]=candidate_counter;
			prev_group=this_group;
			prev_elec=elec;
			candidate_counter = 1; 
		}      		
	}
/* record the number of candidates in the last group for the last electorate */
	ix = (prev_elec-1)*(MAX_GROUPS) + prev_group;
	num_in_group[ix]=candidate_counter;

	/* clean up */
	PQclear(result);
	fclose(fh);
}
bool directory_exists(const char *path);
bool directory_exists(const char *path) {
	int ret=true;
	DIR *dir;
	if ((dir=opendir(path))) {
		closedir(dir);
	} else {
		ret=false;
	}
	return ret;
}

int main(void) {

  PGconn *conn;
  PGresult *result_votes;
  
  /* SIPL 2011-06-03: allocate space for the NULL inserted by
     fetch_electorates(). */
  struct electorate *electorates[MAX_ELECTORATES+1];
  struct rotation *rotation = { NULL };
  unsigned int num_votes, i,j,k;
  unsigned int polling_place_code,electorate_code,batch_number;
  int paper_version,vote_id;
  unsigned int num_in_group[(MAX_ELECTORATES)*MAX_GROUPS];
  char *pv_filename,*formal_filename=NULL,*informal_filename=NULL,*preference_list;
  FILE *fh_formal=NULL, *fh_informal=NULL,*fh_paper_version=NULL;
  char *path;
  struct preference_set *vote;

  /* SIPL 2014-05-20 Support electorate names with spaces. */
  char *electorate_name_normalized;

/*  conn = connect_db_host(DATABASE_NAME,SERVER_ADDRESS);
    if (!conn) {
	  fprintf (stderr, "Can't connect to database '%s' at '%s'\n"
		   "Trying local database\n",
		   DATABASE_NAME,SERVER_ADDRESS);*/
	  conn=connect_db_host(DATABASE_NAME,NULL);
	  if (!conn) { 
		  fprintf (stderr, "Can't connect to local database '%s'\n",
			   DATABASE_NAME);
		  exit(1);
	  }
/*  }*/

  /* create output root directory if necessary */
  umask((mode_t) 022);
  
  if (directory_exists(OUTPUTROOT) == false) 
	  if (mkdir(OUTPUTROOT,(mode_t) 0777)) 
		  bailout("Can't create directory %s\n",OUTPUTROOT);
  
  path=sprintf_malloc("%s/%s",OUTPUTROOT,OUTPUTDIR);
  /* remove and create output directory */
  
  if (directory_exists(path) == false) 
	  if (mkdir(path,(mode_t) 0777)) 
		  bailout("Can't create directory %s\n",path);
  free(path);

  fetch_electorates(conn,electorates);
  write_groups(conn,"Groups.csv");
  write_candidates(conn,"Candidates.csv",num_in_group);

  /* open paper_version file */
  pv_filename=sprintf_malloc("%s/%s/%s",OUTPUTROOT,OUTPUTDIR,"Confirmed_Paper_Versions.csv");
  fh_paper_version = fopen((const char *) pv_filename,"w");
  if (!fh_paper_version) bailout("Can't open %s\n",pv_filename);
  /* SIPL 2011-06-10 Changed the following heading to match the output data.
  fprintf(fh_paper_version,"\"Batch Number\", \"Polling Place Code\","
	                   "\"Paper Index\",\"Paper Version\"\r\n");
  */
  fprintf(fh_paper_version,"\"Batch Number\", \"Paper Index\", "
			   "\"Electorate Code\", "
	                   "\"Polling Place Code\", \"Paper Version\"\r\n");
  
  free(pv_filename);
 
  
  /* Get all committed votes & preferences */
  
  for (i=0;electorates[i]!=NULL;i++)
  {
	  fprintf(stderr,"Retrieving Confirmed Votes for %s...",electorates[i]->name);
	  fflush(stderr);

	/* SIPL 2014-05-20 Support electorate names with spaces. */
	  electorate_name_normalized = malloc(strlen(electorates[i]->name) + 1);
	  if (!electorate_name_normalized)
		  bailout("Out of memory while allocating space for electorate name!\n");
	  normalize_electorate_name(electorate_name_normalized, electorates[i]->name);

	  result_votes =  SQL_query(conn,
				    "SELECT cv.batch_number,b.electorate_code, "
				    "b.polling_place_code, cv.paper_version, "
				    "cv.preference_list, cv.id "
				    "FROM %s_confirmed_vote cv, batch b "
				    "WHERE cv.batch_number = b.number "
				    "ORDER BY id,batch_number;",
				    electorate_name_normalized
		  );
	  
	  fprintf(stderr,"Found %i Confirmed Votes\n",PQntuples(result_votes));
	  
	  /* skip this electorate if no votes returned */
	  if (PQntuples(result_votes) < 1) 
	  {
		  fprintf(stderr, "No Preferences in Database for %s!\n",electorates[i]->name);
		  continue;
	  }
         /* open and initialise new electorate files */	
	  formal_filename = 
		  sprintf_malloc("%s/%s/%s_formal_confirmed.csv", 
				 OUTPUTROOT,OUTPUTDIR,
				 electorate_name_normalized);
	  fh_formal = fopen(formal_filename,"w");
	  if (!fh_formal) bailout("Can't open %s!\n",formal_filename);
	  free(formal_filename);
	  
	  informal_filename = 
		  sprintf_malloc("%s/%s/%s_informal_confirmed.csv", 
				 OUTPUTROOT,OUTPUTDIR,
				 electorate_name_normalized);
	  fh_informal = fopen(informal_filename,"w");
	  if (!fh_informal) bailout("Can't open %s!\n",informal_filename);
	  free(informal_filename);

	  /* SIPL 2014-05-20 Done with electorate_name_normalized. */
	  free(electorate_name_normalized);


	  fprintf(fh_formal,"\"Batch Number\",\"Paper Index\",\"Preference Number\",\"Party Code\",\"Candidate Code\",\"Candidate Screen Index\"\r\n"); 
	  fprintf(fh_informal,"\"Batch Number\",\"Internal Vote Id\"\r\n"); 
	  
	  /* user feedback */
	  fprintf(stderr,"Processing Confirmed Votes for %s.\n", electorates[i]->name);
	  
	  num_votes=1;
	  /* For each vote, get its details */
	  for (j = 0; j < PQntuples(result_votes); j++) {
		  batch_number = atoi(PQgetvalue(result_votes,j,0));
		  electorate_code = atoi(PQgetvalue(result_votes,j,1));
		  polling_place_code = atoi(PQgetvalue(result_votes,j,2));
		  paper_version = atoi(PQgetvalue(result_votes,j,3)); 
		  preference_list = PQgetvalue(result_votes,j,4);
		  vote_id =  atoi(PQgetvalue(result_votes,j,5));
		  
		  if (paper_version >= 1)
			  rotation=fetch_rotation(conn,paper_version,
						  electorates[electorate_code-1]->num_seats);
		  
		  vote=unpack_preferences(preference_list);
		  vote->paper_version=paper_version;
		  
		  /* informal?  */
		  if (vote->num_preferences ==0) {
			    fprintf(fh_informal,"%i,%i\r\n",
				    batch_number,vote_id);
		  }
			 		  
		  /* iterate through each preference, converting DB index to screen index*/
		  for(k=0;k<vote->num_preferences;k++) 
		  {
			  if (paper_version >=1)
				  /* find the screen position within group for this preference*/

				  /* SIPL 2011-06-03: The first parameter was
				     electorates[electorate_code-1]->num_seats,
				     but it should be the number of candidates
				     in the group. */

				  /* SIPL 2011-07-15: Now support
				     split groups.  This was:
				   vote->candidates[k].screen_candidate_index =
				     translate_dbci_to_sci(
				   num_in_group[(electorate_code-1)*(MAX_GROUPS)
				     +vote->candidates[k].group_index],
				     vote->candidates[k].db_candidate_index,
				     rotation);
				  */
				vote->candidates[k].screen_candidate_index =
				  translate_group_dbci_to_sci(
					electorates[i],
					vote->candidates[k].group_index,
					vote->candidates[k].db_candidate_index,
					rotation);
			  else 
				  vote->candidates[k].screen_candidate_index = -1;
	  
			  /* output preference to file */
			  fprintf(fh_formal,"%2i,%2i,%2i,%2i,%2i,%2i\r\n",
				  batch_number,
				  vote_id,
				  vote->candidates[k].prefnum,
				  vote->candidates[k].group_index,
				  vote->candidates[k].db_candidate_index,
				  vote->candidates[k].screen_candidate_index
				  );
		  } /* END for k (vote preferences) */
	  
		  /* print to paper_version file  */
		  fprintf(fh_paper_version, "%7i,%7i,%2i,%3i,%3i\r\n",
			  batch_number,
			  vote_id,
			  electorate_code,
			  polling_place_code,
			  paper_version);	 
		  /* user_feedback - print counter */
		  fprintf(stderr,"\b\b\b\b\b\b%i",num_votes++);
/*		  fflush(stderr); */

		  free(vote);
	  } /* END for j (votes) */
	  PQclear(result_votes);
	  
	  fclose(fh_formal);
	  fclose(fh_informal);
	  
	  fprintf(stderr,"...Done\n\n");
	  fflush(stderr);
  } /* END for i (electorates) */

  fclose(fh_paper_version);
  PQfinish(conn);
  free_electorates(electorates);
  exit(0);
}




