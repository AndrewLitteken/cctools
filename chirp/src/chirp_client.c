/*
Copyright (C) 2003-2004 Douglas Thain and the University of Wisconsin
Copyright (C) 2005- The University of Notre Dame
This software is distributed under a BSD-style license.
See the file COPYING for details.
*/

#include "chirp_protocol.h"
#include "chirp_client.h"
#include "chirp_group.h"

#include "sleeptools.h"

#include "link.h"
#include "auth.h"
#include "auth_hostname.h"
#include "domain_name_cache.h"
#include "full_io.h"
#include "macros.h"
#include "debug.h"
#include "copy_stream.h"
#include "list.h"
#include "url_encode.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/stat.h>
#ifdef HAS_SYS_STATFS_H
#include <sys/statfs.h>
#endif
#include <sys/param.h>
#include <sys/mount.h>
#include <signal.h>

#include <unistd.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

/* The maximum chunk of memory the server will allocate to handle I/O */
#define MAX_BUFFER_SIZE (16*1024*1024)

static INT64_T get_stat_result( struct chirp_client *c, struct chirp_stat *info, time_t stoptime );
static INT64_T get_statfs_result( struct chirp_client *c, struct chirp_statfs *info, time_t stoptime );
static INT64_T send_command(struct chirp_client *c, time_t stoptime, char const *fmt,...);
static INT64_T get_result( struct chirp_client *c, time_t stoptime );
static INT64_T simple_command(struct chirp_client *c, time_t stoptime, char const *fmt,...);

static int global_serial = 0;

struct chirp_client {
	struct link *link;
	char hostport[CHIRP_PATH_MAX];
	int broken;
	int serial;
};

struct chirp_client * chirp_client_connect_condor( time_t stoptime )
{
	FILE *file;
	int fields;
	int save_errno;
	struct chirp_client *client;
	char host[CHIRP_LINE_MAX];
	char hostport[CHIRP_LINE_MAX];
	char cookie[CHIRP_LINE_MAX];
	int port;
	int result;

	file = fopen("chirp.config","r");
	if(!file) return 0;

	fields = fscanf(file,"%s %d %s",host,&port,cookie);
	fclose(file);

	if(fields!=3) {
		errno = EINVAL;
		return 0;
	}

	sprintf(hostport,"%s:%d",host,port);

	client = chirp_client_connect(hostport,0,stoptime);
	if(!client) return 0;

	result = chirp_client_cookie(client,cookie,stoptime);
	if(result!=0) {
		save_errno = errno;
		chirp_client_disconnect(client);
		errno = save_errno;
		return 0;
	}

	return client;
}

struct chirp_client * chirp_client_connect( const char *hostport, int negotiate_auth, time_t stoptime )
{
	struct chirp_client *c;
	char addr[LINK_ADDRESS_MAX];
	char host[DOMAIN_NAME_MAX];
	int save_errno;
	int port;

	if(sscanf(hostport,"%[^:]:%d",host,&port)==2) {
		/* use the split host and port */
	} else {
		strcpy(host,hostport);
		port = CHIRP_PORT;
	}

	if(!domain_name_cache_lookup(host,addr)) {
		errno = ENOENT;
		return 0;
	}

	c = malloc(sizeof(*c));
	if(c) {
	        c->link = link_connect(addr,port,stoptime);
		c->broken = 0;
		c->serial = global_serial++;
		if(c->link) {
		        link_tune(c->link,LINK_TUNE_INTERACTIVE);
			if(negotiate_auth) {
				char *type, *subject;
				if(auth_assert(c->link,&type,&subject,stoptime)) {
					free(type);
					free(subject);
					strcpy(c->hostport,hostport);

					return c;
				} else {
					chirp_client_disconnect(c);
					c = 0;
					if(time(0)>=stoptime) {
						errno = ECONNRESET;
					} else {
						errno = EACCES;
					}
				}
			} else {
				return c;
			}
		}
		save_errno = errno;
		free(c);
		errno = save_errno;
	}

	return 0;
}

void chirp_client_disconnect( struct chirp_client *c )
{
	link_close(c->link);
	free(c);
}

INT64_T chirp_client_serial( struct chirp_client *c )
{
	return c->serial;
}

INT64_T chirp_client_cookie( struct chirp_client *c, const char *cookie, time_t stoptime )
{
	return simple_command(c,stoptime,"cookie %s\n",cookie);
}

INT64_T chirp_client_login( struct chirp_client *c, const char *name, const char *password, time_t stoptime  )
{
	return simple_command(c,stoptime,"login %s %s\n",name,password);
}

INT64_T chirp_client_lookup( struct chirp_client *c, const char *logical_name, char **url, time_t stoptime  )
{
	INT64_T result;
	INT64_T actual;

	result = simple_command(c,stoptime,"lookup %s\n",logical_name);
	fprintf(stderr, "%d\n", result);

	if(result>0) {
		*url = malloc(result);
		if(*url) {
			actual = link_read(c->link,*url,result,stoptime);
			if(actual!=result) {
				c->broken = 1;
				errno = ECONNRESET;
				return -1;
			}
		} else {
			c->broken = 1;
			errno = ECONNRESET;
			return -1;
		}
	}

	return result;
}

INT64_T chirp_client_getlongdir( struct chirp_client *c, const char *path, chirp_longdir_t callback, void *arg, time_t stoptime )
{
	char name[CHIRP_LINE_MAX];
	struct chirp_stat info;
	int result;

	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));

	result = simple_command(c,stoptime,"getlongdir %s\n",safepath);
	if(result<0) return result;

	while(link_readline(c->link,name,sizeof(name),stoptime)) {

		if(!name[0]) return 0;

		if(get_stat_result(c,&info,stoptime)>=0) {
			callback(name,&info,arg);
		} else {
			break;
		}
	}

	c->broken = 1;
	errno = ECONNRESET;
	return -1;
}

INT64_T chirp_client_getdir( struct chirp_client *c, const char *path, chirp_dir_t callback, void *arg, time_t stoptime )
{
	INT64_T result;
	const char *name;

	result = chirp_client_opendir(c,path,stoptime);
	if(result==0) {
		while((name=chirp_client_readdir(c,stoptime))) {
			callback(name,arg);
		}
	}

	return result;
}

INT64_T chirp_client_opendir( struct chirp_client *c, const char *path, time_t stoptime  )
{
	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));

	return simple_command(c,stoptime,"getdir %s\n",safepath);
}

const char * chirp_client_readdir( struct chirp_client *c, time_t stoptime )
{
	static char name[CHIRP_PATH_MAX];

	if(link_readline(c->link,name,sizeof(name),stoptime)) {
		if(name[0]) {
			return name;
		} else {
			return 0;
		}
	} else {
		c->broken = 1;
		errno = ECONNRESET;
		return 0;
	}

}

INT64_T chirp_client_getacl( struct chirp_client *c, const char *path, chirp_dir_t callback, void *arg, time_t stoptime )
{
	INT64_T result;
	const char *name;

	result = chirp_client_openacl(c,path,stoptime);
	if(result==0) {
		while((name=chirp_client_readacl(c,stoptime))) {
			callback(name,arg);
		}
	}

	return result;
}

INT64_T chirp_client_openacl( struct chirp_client *c, const char *path, time_t stoptime  )
{
	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));

	return simple_command(c,stoptime,"getacl %s\n",safepath);
}

const char * chirp_client_readacl( struct chirp_client *c, time_t stoptime )
{
	static char acl[CHIRP_PATH_MAX];

	if(link_readline(c->link,acl,sizeof(acl),stoptime)) {
		if(acl[0]) {
			return acl;
		} else {
			return 0;
		}
	} else {
		c->broken = 1;
		errno = ECONNRESET;
		return 0;
	}

}

INT64_T chirp_client_setacl( struct chirp_client *c, const char *path, const char *user, const char *acl, time_t stoptime )
{
	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));
	return simple_command(c,stoptime,"setacl %s %s %s\n",safepath,user,acl);
}

INT64_T chirp_client_resetacl( struct chirp_client *c, const char *path, const char *acl, time_t stoptime )
{
	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));
	return simple_command(c,stoptime,"resetacl %s %s\n",safepath,acl);
}

INT64_T chirp_client_locate( struct chirp_client *c, const char *path, chirp_loc_t callback, void *arg, time_t stoptime )
{
	char location[CHIRP_PATH_MAX];
	char host[CHIRP_PATH_MAX];
	sscanf(c->hostport, "%[^:]%*s", host);
	snprintf(location,CHIRP_PATH_MAX,"%s:%s",host,path);
	callback(location,arg);
	return 1;
}

INT64_T chirp_client_open( struct chirp_client *c, const char *path, INT64_T flags, INT64_T mode, struct chirp_stat *info, time_t stoptime  )
{
	INT64_T result;
	char fstr[256];

	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));

	fstr[0] = 0;

	if(flags&O_WRONLY) {
		strcat(fstr,"w");
	} else if(flags&O_RDWR) {
		strcat(fstr,"rw");
	} else {
		strcat(fstr,"r");
	}

	if(flags&O_CREAT) strcat(fstr,"c");
	if(flags&O_TRUNC) strcat(fstr,"t");
	if(flags&O_APPEND) strcat(fstr,"a");
	if(flags&O_EXCL) strcat(fstr,"x");
#ifdef O_SYNC
	if(flags&O_SYNC) strcat(fstr,"s");
#endif
	
	result = simple_command(c,stoptime,"open %s %s %lld\n",safepath,fstr,mode);
	if(result>=0) {
		if(get_stat_result(c,info,stoptime)>=0) {
			return result;
		} else {
			c->broken = 1;
			errno = ECONNRESET;
			return -1;
		}
	} else {
		return result;
	}
}

INT64_T chirp_client_close( struct chirp_client *c, INT64_T fd, time_t stoptime  )
{
	return simple_command(c,stoptime,"close %lld\n",fd);
}

INT64_T chirp_client_pread_begin( struct chirp_client *c, INT64_T fd, void *buffer, INT64_T length, INT64_T offset, time_t stoptime  )
{
	return send_command(c,stoptime,"pread %lld %lld %lld\n",fd,length,offset);
}

INT64_T chirp_client_pread_finish( struct chirp_client *c, INT64_T fd, void *buffer, INT64_T length, INT64_T offset, time_t stoptime  )
{
	INT64_T result;
	INT64_T actual;

	result = get_result(c,stoptime);
	if( result>0 ) {
		actual = link_read(c->link,buffer,result,stoptime);
		if(actual!=result) {
			errno = ECONNRESET;
			return -1;
		}
	}

	return result;
}

INT64_T chirp_client_pread( struct chirp_client *c, INT64_T fd, void *buffer, INT64_T length, INT64_T offset, time_t stoptime  )
{
	INT64_T result = chirp_client_pread_begin(c,fd,buffer,length,offset,stoptime);
	if(result<0) return result;
	return chirp_client_pread_finish(c,fd,buffer,length,offset,stoptime);
}

INT64_T chirp_client_sread_begin( struct chirp_client *c, INT64_T fd, void *buffer, INT64_T length, INT64_T stride_length, INT64_T stride_skip, INT64_T offset, time_t stoptime  )
{
	return send_command(c,stoptime,"sread %lld %lld %lld %lld %lld\n",fd,length,stride_length,stride_skip,offset);
}

INT64_T chirp_client_sread_finish( struct chirp_client *c, INT64_T fd, void *buffer, INT64_T length, INT64_T stride_length, INT64_T stride_skip, INT64_T offset, time_t stoptime  )
{
	INT64_T result;
	INT64_T actual;

	result = get_result(c,stoptime);
	if( result>0 ) {
		actual = link_read(c->link,buffer,result,stoptime);
		if(actual!=result) {
			errno = ECONNRESET;
			return -1;
		}
	}

	return result;
}

INT64_T chirp_client_sread( struct chirp_client *c, INT64_T fd, void *buffer, INT64_T length, INT64_T stride_length, INT64_T stride_skip, INT64_T offset, time_t stoptime  )
{
	INT64_T result = chirp_client_sread_begin(c,fd,buffer,length,stride_length,stride_skip,offset,stoptime);
	if(result<0) return result;
	return chirp_client_sread_finish(c,fd,buffer,length,stride_length,stride_skip,offset,stoptime);
}

INT64_T chirp_client_getfile( struct chirp_client *c, const char *path, FILE *stream, time_t stoptime  )
{
	INT64_T length;

	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));

	length = simple_command(c,stoptime,"getfile %s\n",safepath);

	if( length>=0 ) {
		if(link_stream_to_file(c->link,stream,length,stoptime)==length) {
			return length;
		} else {
			c->broken = 1;
			errno = ECONNRESET;
		}
	}

	return -1;
}

INT64_T chirp_client_getfile_buffer( struct chirp_client *c, const char *path, char **buffer, time_t stoptime  )
{
	INT64_T length;
	INT64_T result;

	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));

	*buffer = 0;

	length = simple_command(c,stoptime,"getfile %s\n",safepath);
	if(length<=0) return length;

	*buffer = malloc(length+1);
	if(!*buffer) {
		c->broken = 1;
		errno = ENOMEM;
		return -1;
	}

	result = link_read(c->link,*buffer,length,stoptime);
	if(result<0) {
		free(*buffer);
		c->broken = 1;
		return -1;
	}

	(*buffer)[length] = 0;

	return result;
}

INT64_T chirp_client_readlink( struct chirp_client *c, const char *path, char *buffer, INT64_T length, time_t stoptime  )
{
	INT64_T result;
	INT64_T actual;

	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));

	result = simple_command(c,stoptime,"readlink %s %lld\n",safepath,length);

	if( result>0 ) {
		actual = link_read(c->link,buffer,result,stoptime);
		if(actual!=result) {
			c->broken = 1;
			errno = ECONNRESET;
			return -1;
		}
	}

	return result;
}

INT64_T chirp_client_localpath( struct chirp_client *c, const char *path, char *localpath, int length, time_t stoptime  )
{
	INT64_T result;
	INT64_T actual;

	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));

	result = simple_command(c,stoptime,"localpath %s\n",path);

	if( result>0 ) {
		actual = link_read(c->link,localpath,result,stoptime);
		if(actual!=result) {
			c->broken = 1;
			errno = ECONNRESET;
			return -1;
		}
	}

	return result;
}

INT64_T chirp_client_whoami( struct chirp_client *c, char *buffer, INT64_T length, time_t stoptime  )
{
	INT64_T result;
	INT64_T actual;

	result = simple_command(c,stoptime,"whoami %lld\n",length);

	if( result>0 ) {
		actual = link_read(c->link,buffer,result,stoptime);
		if(actual!=result) {
			c->broken = 1;
			errno = ECONNRESET;
			return -1;
		}
		buffer[actual] = 0;
	}

	return result;
}

INT64_T chirp_client_whoareyou( struct chirp_client *c, const char *rhost, char *buffer, INT64_T length, time_t stoptime )
{
	INT64_T result;
	INT64_T actual;

	result = simple_command(c,stoptime,"whoareyou %s %lld\n",rhost,length);

	if( result>0 ) {
		actual = link_read(c->link,buffer,result,stoptime);
		if(actual!=result) {
			c->broken = 1;
			errno = ECONNRESET;
			return -1;
		}
	}

	return result;
}

INT64_T chirp_client_pwrite_begin( struct chirp_client *c, INT64_T fd, const void *buffer, INT64_T length, INT64_T offset, time_t stoptime  )
{
	INT64_T result;

	if( length > MAX_BUFFER_SIZE )
		length = MAX_BUFFER_SIZE;

	result = send_command(c,stoptime,"pwrite %lld %lld %lld\n",fd,length,offset);
	if(result<0) return result;

	result = link_write(c->link,buffer,length,stoptime);
	if(result!=length) {
		c->broken = 1;
		errno = ECONNRESET;
		return -1;
	}

	return result;
}

INT64_T chirp_client_pwrite_finish( struct chirp_client *c, INT64_T fd, const void *buffer, INT64_T length, INT64_T offset, time_t stoptime  )
{
	return get_result(c,stoptime);
}

INT64_T chirp_client_pwrite( struct chirp_client *c, INT64_T fd, const void *buffer, INT64_T length, INT64_T offset, time_t stoptime  )
{
	INT64_T result = chirp_client_pwrite_begin(c,fd,buffer,length,offset,stoptime);
	if(result<0) return result;
	return chirp_client_pwrite_finish(c,fd,buffer,length,offset,stoptime);
}

INT64_T chirp_client_swrite_begin( struct chirp_client *c, INT64_T fd, const void *buffer, INT64_T length, INT64_T stride_length, INT64_T stride_skip, INT64_T offset, time_t stoptime  )
{
	INT64_T result;

	if( length > MAX_BUFFER_SIZE )
		length = MAX_BUFFER_SIZE;

	result = send_command(c,stoptime,"swrite %lld %lld %lld %lld %lld\n",fd,length,stride_length,stride_skip,offset);
	if(result<0) return result;

	result = link_write(c->link,buffer,length,stoptime);
	if(result!=length) {
		c->broken = 1;
		errno = ECONNRESET;
		return -1;
	}

	return result;
}

INT64_T chirp_client_swrite_finish( struct chirp_client *c, INT64_T fd, const void *buffer, INT64_T length, INT64_T stride_length, INT64_T stride_skip, INT64_T offset, time_t stoptime  )
{
	return get_result(c,stoptime);
}

INT64_T chirp_client_swrite( struct chirp_client *c, INT64_T fd, const void *buffer, INT64_T length, INT64_T stride_length, INT64_T stride_skip, INT64_T offset, time_t stoptime  )
{
	INT64_T result = chirp_client_swrite_begin(c,fd,buffer,length,stride_length,stride_skip,offset,stoptime);
	if(result<0) return result;
	return chirp_client_swrite_finish(c,fd,buffer,length,stride_length,stride_skip,offset,stoptime);
}

INT64_T chirp_client_putfile( struct chirp_client *c, const char *path, FILE *stream, INT64_T mode, INT64_T length, time_t stoptime )
{
	INT64_T result;

	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));

	result = simple_command(c,stoptime,"putfile %s %lld %lld\n",safepath,mode,length);
	if(result<0) return result;

	result = link_stream_from_file(c->link,stream,length,stoptime);
	if(result!=length) {
		c->broken = 1;
		errno = ECONNRESET;
		return -1;
	}
	
	return get_result(c,stoptime);
}

INT64_T chirp_client_putfile_buffer( struct chirp_client *c, const char *path, const char *buffer, INT64_T mode, INT64_T length, time_t stoptime )
{
	INT64_T result;

	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));

	result = simple_command(c,stoptime,"putfile %s %lld %lld\n",safepath,mode,length);
	if(result<0) return result;

	result = link_write(c->link,buffer,length,stoptime);
	if(result!=length) {
		c->broken = 1;
		errno = ECONNRESET;
		return -1;
	}
		
	return get_result(c,stoptime);
}

INT64_T chirp_client_getstream( struct chirp_client *c, const char *path, time_t stoptime )
{
	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));

	return simple_command(c,stoptime,"getstream %s\n",path);
}

INT64_T chirp_client_getstream_read( struct chirp_client *c, void *buffer, INT64_T length, time_t stoptime )
{
	return link_read_avail(c->link,buffer,length,stoptime);
}

INT64_T chirp_client_putstream( struct chirp_client *c, const char *path, time_t stoptime )
{
	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));
	return simple_command(c,stoptime,"putstream %s\n",path);
}

INT64_T chirp_client_putstream_write( struct chirp_client *c, const char *data, INT64_T length, time_t stoptime )
{
	return link_write(c->link,data,length,stoptime);
}

INT64_T chirp_client_thirdput( struct chirp_client *c, const char *path, const char *hostname, const char *newpath, time_t stoptime ) {
	char safepath[CHIRP_LINE_MAX];
	char safenewpath[CHIRP_LINE_MAX];

	url_encode(path,safepath,sizeof(safepath));
	url_encode(newpath,safenewpath,sizeof(safenewpath));

	return simple_command(c,stoptime, "thirdput %s %s %s\n",safepath,hostname,safenewpath);
}

INT64_T chirp_client_fchmod( struct chirp_client *c, INT64_T fd, INT64_T mode, time_t stoptime )
{
	return simple_command(c,stoptime,"fchmod %lld %lld\n",fd,mode);
}

INT64_T chirp_client_fchown( struct chirp_client *c, INT64_T fd, INT64_T uid, INT64_T gid, time_t stoptime )
{
	return simple_command(c,stoptime,"fchown %lld %lld %lld\n",fd,uid,gid);
}

INT64_T chirp_client_ftruncate( struct chirp_client *c, INT64_T fd, INT64_T length, time_t stoptime )
{
	return simple_command(c,stoptime,"ftruncate %lld %lld\n",fd,length);
}

INT64_T chirp_client_fstat_begin( struct chirp_client *c, INT64_T fd, struct chirp_stat *info, time_t stoptime )
{
	return send_command(c,stoptime,"fstat %lld\n",fd);
}

INT64_T chirp_client_fstat_finish( struct chirp_client *c, INT64_T fd, struct chirp_stat *info, time_t stoptime )
{
	INT64_T result = get_result(c,stoptime);
	if(result>=0) return get_stat_result(c,info,stoptime);
	return result;
}

INT64_T chirp_client_fstat( struct chirp_client *c, INT64_T fd, struct chirp_stat *info, time_t stoptime )
{
	INT64_T result = chirp_client_fstat_begin(c,fd,info,stoptime);
	if(result>=0) return chirp_client_fstat_finish(c,fd,info,stoptime);
	return result;
}

INT64_T chirp_client_stat( struct chirp_client *c, const char *path, struct chirp_stat *info, time_t stoptime )
{
	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));
	INT64_T result = simple_command(c,stoptime,"stat %s\n",safepath);
	if(result>=0) result = get_stat_result(c,info,stoptime);
	return result;
}

INT64_T chirp_client_lstat( struct chirp_client *c, const char *path, struct chirp_stat *info, time_t stoptime )
{
	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));
	INT64_T result = simple_command(c,stoptime,"lstat %s\n",safepath);
	if(result>=0) result = get_stat_result(c,info,stoptime);
	return result;
}

INT64_T chirp_client_fstatfs( struct chirp_client *c, INT64_T fd, struct chirp_statfs *info, time_t stoptime )
{
	INT64_T result = simple_command(c,stoptime,"fstatfs %lld\n",fd);
	if(result>=0) result = get_statfs_result(c,info,stoptime);
	return result;
}

INT64_T chirp_client_statfs( struct chirp_client *c, const char *path, struct chirp_statfs *info, time_t stoptime )
{
	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));
	INT64_T result = simple_command(c,stoptime,"statfs %s\n",safepath);
	if(result>=0) result = get_statfs_result(c,info,stoptime);
	return result;
}

INT64_T chirp_client_mkfifo( struct chirp_client *c, const char *path, time_t stoptime )
{
	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));
	return simple_command(c,stoptime,"mkfifo %s\n",safepath);
}

INT64_T chirp_client_unlink( struct chirp_client *c, const char *path, time_t stoptime )
{
	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));
	return simple_command(c,stoptime,"unlink %s\n",safepath);
}

INT64_T chirp_client_rename( struct chirp_client *c, const char *oldpath, const char *newpath, time_t stoptime )
{
	char safeoldpath[CHIRP_LINE_MAX];
	char safenewpath[CHIRP_LINE_MAX];

	url_encode(oldpath,safeoldpath,sizeof(safeoldpath));
	url_encode(newpath,safenewpath,sizeof(safenewpath));

	return simple_command(c,stoptime,"rename %s %s\n",safeoldpath,safenewpath);
}

INT64_T chirp_client_link( struct chirp_client *c, const char *oldpath, const char *newpath, time_t stoptime )
{
	char safeoldpath[CHIRP_LINE_MAX];
	char safenewpath[CHIRP_LINE_MAX];

	url_encode(oldpath,safeoldpath,sizeof(safeoldpath));
	url_encode(newpath,safenewpath,sizeof(safenewpath));

	return simple_command(c,stoptime,"link %s %s\n",safeoldpath,safenewpath);
}

INT64_T chirp_client_symlink( struct chirp_client *c, const char *oldpath, const char *newpath, time_t stoptime )
{
	char safeoldpath[CHIRP_LINE_MAX];
	char safenewpath[CHIRP_LINE_MAX];

	url_encode(oldpath,safeoldpath,sizeof(safeoldpath));
	url_encode(newpath,safenewpath,sizeof(safenewpath));

	debug(D_CHIRP, "symlink %s %s", safeoldpath, safenewpath);
	return simple_command(c,stoptime,"symlink %s %s\n",safeoldpath,safenewpath);
}

INT64_T chirp_client_fsync_begin( struct chirp_client *c, INT64_T fd, time_t stoptime )
{
	return send_command(c,stoptime,"fsync %lld\n",fd);
}

INT64_T chirp_client_fsync_finish( struct chirp_client *c, INT64_T fd, time_t stoptime )
{
	return get_result(c,stoptime);
}

INT64_T chirp_client_fsync( struct chirp_client *c, INT64_T fd, time_t stoptime )
{
	INT64_T result = chirp_client_fsync_begin(c,fd,stoptime);
	if(result>=0) return get_result(c,stoptime);
	return result;
}

INT64_T chirp_client_mkdir( struct chirp_client *c, char const *path, INT64_T mode, time_t stoptime )
{
	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));
	return simple_command(c,stoptime,"mkdir %s %lld\n",safepath,mode);
}

INT64_T chirp_client_rmdir( struct chirp_client *c, char const *path, time_t stoptime )
{
	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));
	return simple_command(c,stoptime,"rmdir %s\n",safepath);
}

INT64_T chirp_client_rmall( struct chirp_client *c, char const *path, time_t stoptime )
{
	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));
	return simple_command(c,stoptime,"rmall %s\n",safepath);
}

INT64_T chirp_client_truncate( struct chirp_client *c, char const *path, INT64_T length, time_t stoptime )
{
	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));
	return simple_command(c,stoptime,"truncate %s %lld\n",safepath,length);
}

INT64_T chirp_client_utime( struct chirp_client *c, char const *path, time_t actime, time_t modtime, time_t stoptime )
{
	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));
	return simple_command(c,stoptime,"utime %s %u %u\n",safepath,actime,modtime);
}

INT64_T chirp_client_access( struct chirp_client *c, char const *path, INT64_T mode, time_t stoptime )
{
	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));
	return simple_command(c,stoptime,"access %s %lld\n",safepath,mode);
}

INT64_T chirp_client_chmod( struct chirp_client *c, char const *path, INT64_T mode, time_t stoptime )
{
	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));
	return simple_command(c,stoptime,"chmod %s %lld\n",safepath,mode);
}

INT64_T chirp_client_chown( struct chirp_client *c, char const *path, INT64_T uid, INT64_T gid, time_t stoptime)
{
	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));
	return simple_command(c,stoptime,"chown %s %lld %lld\n",safepath,uid,gid);
}

INT64_T chirp_client_lchown( struct chirp_client *c, char const *path, INT64_T uid, INT64_T gid, time_t stoptime )
{
	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));
	return simple_command(c,stoptime,"lchown %s %lld %lld\n",safepath,uid,gid);
}

INT64_T chirp_client_job_begin( struct chirp_client *c, const char *cwd, const char *infile, const char *outfile, const char *errfile, const char *cmdline, time_t stoptime )
{
	char safeinfile[CHIRP_PATH_MAX];
	char safeoutfile[CHIRP_PATH_MAX];
	char safeerrfile[CHIRP_PATH_MAX];

	url_encode(infile,safeinfile,sizeof(safeinfile));
	url_encode(outfile,safeoutfile,sizeof(safeoutfile));
	url_encode(errfile,safeerrfile,sizeof(safeerrfile));

	return simple_command(c,stoptime,"job_begin %s %s %s %s %s\n",cwd,safeinfile,safeoutfile,safeerrfile,cmdline);
}

INT64_T chirp_client_job_commit( struct chirp_client *c, INT64_T jobid, time_t stoptime )
{
	return simple_command(c,stoptime,"job_commit %lld\n",jobid);
}

INT64_T chirp_client_job_kill( struct chirp_client *c, INT64_T jobid, time_t stoptime )
{
	return simple_command(c,stoptime,"job_kill %lld\n",jobid);
}

INT64_T chirp_client_job_remove( struct chirp_client *c, INT64_T jobid, time_t stoptime )
{
	return simple_command(c,stoptime,"job_remove %lld\n",jobid);
}

INT64_T chirp_client_job_wait( struct chirp_client *c, INT64_T jobid, struct chirp_job_state *state, int wait_time, time_t stoptime )
{
	char line[CHIRP_LINE_MAX];
	INT64_T result = simple_command(c,stoptime,"job_wait %lld %lu\n",jobid,wait_time);
	if(result>=0) {
		if(link_readline(c->link,line,sizeof(line),stoptime)) {
			if(sscanf(line,"%lld %s %s %d %d %lu %lu %lu %d",
				&state->jobid,
				state->command,
				state->owner,
				(int*)&state->state,
				&state->exit_code,
				&state->submit_time,
				&state->start_time,
				&state->stop_time,
				&state->pid)
			) {
				return 0;
			} else {
				errno = ECONNRESET;
				return -1;
			}
		}
	}
	return result;
}

INT64_T chirp_client_job_list( struct chirp_client *c, chirp_joblist_t callback, void *arg, time_t stoptime )
{
	char line[CHIRP_LINE_MAX];
	INT64_T result = simple_command(c,stoptime,"job_list\n");
	if(result>=0) {
		while(link_readline(c->link,line,sizeof(line),stoptime)) {
			struct chirp_job_state state;
			if(!line[0]) return 0;
			if(sscanf(line,"%lld %s %s %d %d %lu %lu %lu %d",
				&state.jobid,
				state.command,
				state.owner,
				(int*)&state.state,
				&state.exit_code,
				&state.submit_time,
				&state.start_time,
				&state.stop_time,
				&state.pid)
			) {
				callback(&state,arg);
				continue;
			} else {
				errno = ECONNRESET;
				return -1;
			}
		}
	}
	return result;
}

INT64_T chirp_client_md5( struct chirp_client *c, const char *path, unsigned char digest[16], time_t stoptime  )
{
	INT64_T result;
	INT64_T actual;

	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));

	result = simple_command(c,stoptime,"md5 %s\n",path);

	if( result==16 ) {
		actual = link_read(c->link,(char *)digest,16,stoptime);
		if(actual!=result) {
			errno = ECONNRESET;
			result = -1;
		}
		
	} else if(result>=0) {
		result = -1;
		errno = ECONNRESET;
	}
	return result;
}

INT64_T chirp_client_audit( struct chirp_client *c, const char *path, struct chirp_audit **list, time_t stoptime )
{	
	INT64_T result;
	struct chirp_audit *entry;
	int i, actual;
	char line[CHIRP_LINE_MAX];

	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));

	result = simple_command(c,stoptime,"audit %s\n",safepath);
	if(result<=0) return result;

	*list = malloc(sizeof(struct chirp_audit)*result);
	entry = *list;

	for(i=0;i<result;i++) {
		actual = link_readline(c->link,line,sizeof(line),stoptime);
		if(actual<=0) {
			free(*list);
			result = -1;
			errno = ECONNRESET;
			break;
		} else {
			sscanf(line,"%s %lld %lld %lld",entry->name,&entry->nfiles,&entry->ndirs,&entry->nbytes);
		}
		entry++;
	}

	return result;
}

INT64_T chirp_client_mkalloc( struct chirp_client *c, char const *path, INT64_T size, INT64_T mode, time_t stoptime )
{
	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));
	return simple_command(c,stoptime,"mkalloc %s %lld %lld\n",safepath,size,mode);
}

INT64_T chirp_client_lsalloc( struct chirp_client *c, char const *path, char *allocpath, INT64_T *total, INT64_T *inuse, time_t stoptime )
{
	int result;
	char line[CHIRP_LINE_MAX];

	char safepath[CHIRP_LINE_MAX];
	url_encode(path,safepath,sizeof(safepath));

	result = simple_command(c,stoptime,"lsalloc %s\n",safepath);
	if(result==0) {
		if(link_readline(c->link,line,sizeof(line),stoptime)) {
			sscanf(line,"%s %lld %lld",allocpath,total,inuse);
		} else {
			c->broken = 1;
			errno = ECONNRESET;
			result = -1;
		}
	}
	return result;
}

INT64_T chirp_client_group_create( struct chirp_client *c, char *group, time_t stoptime )
{
        return simple_command(c, stoptime, "group_create %s\n", group);
}

INT64_T chirp_client_group_add( struct chirp_client *c, char *group, char *user, time_t stoptime )
{
        return simple_command(c, stoptime, "group_add %s %s\n", group, user);
}

INT64_T chirp_client_group_remove( struct chirp_client *c, char *group, char *user, time_t stoptime )
{
        return simple_command(c, stoptime, "group_remove %s %s\n", group, user);
}

INT64_T chirp_client_group_lookup( struct chirp_client *c, const char *group, const char *user, time_t stoptime )
{
       return simple_command(c, stoptime, "group_lookup %s %s\n", group, user);
}

INT64_T chirp_client_group_list( struct chirp_client *c, const char *group, chirp_dir_t callback, void *arg, time_t stoptime )
{
	char line[CHIRP_LINE_MAX];

	INT64_T result = simple_command(c, stoptime, "group_list %s\n",group);
	if(result<0) return result;

	while(link_readline(c->link,line,sizeof(line),stoptime)) {
		if(line[0]) {
			callback(line,arg);
		} else {
			return 0;
			break;
		}
	}

	return -1;
}

INT64_T chirp_client_group_cache_update( struct chirp_client *c, const char *group, time_t mod_time, time_t stoptime )
{
        return simple_command(c, stoptime, "group_cache_update %s %ld\n", group, mod_time);
}

INT64_T chirp_client_group_policy_set( struct chirp_client *c, char *group, unsigned long int file_duration, unsigned long int dec_duration, time_t stoptime )
{
        return simple_command(c, stoptime, "group_policy_set %s %ld %ld\n", group, file_duration, dec_duration);
}
INT64_T chirp_client_group_policy_get( struct chirp_client *c, const char *group, int *policy, int *file_duration, int *dec_duration, time_t stoptime )
{
	char line[CHIRP_LINE_MAX];
	INT64_T result;

	sprintf(line, "group_policy_get %s\n", group);
	result = link_write(c->link,line,strlen(line),stoptime);
	debug(D_CHIRP,"= %lld",result);
 
	link_readline(c->link,line,CHIRP_LINE_MAX,stoptime);
	result = get_result(c,stoptime);
	sscanf(line,"%d %d %d", policy, file_duration, dec_duration);

	return result;
}

static INT64_T convert_result( INT64_T result )
{
	if(result>=0) {
		return result;
	} else {
		switch(result) {
		case CHIRP_ERROR_NOT_AUTHENTICATED:
		case CHIRP_ERROR_NOT_AUTHORIZED:
			errno = EACCES;
			break;
		case CHIRP_ERROR_DOESNT_EXIST:
			errno = ENOENT;
			break;
		case CHIRP_ERROR_ALREADY_EXISTS:
			errno = EEXIST;
			break;
		case CHIRP_ERROR_TOO_BIG:
			errno = EFBIG;
			break;
		case CHIRP_ERROR_NO_SPACE:
			errno = ENOSPC;
			break;
		case CHIRP_ERROR_NO_MEMORY:
			errno = ENOMEM;
			break;
		case CHIRP_ERROR_INVALID_REQUEST:
			errno = EINVAL;
			break;
		case CHIRP_ERROR_TOO_MANY_OPEN:
			errno = EMFILE;
			break;
		case CHIRP_ERROR_BUSY:
			errno = EBUSY;
			break;
		case CHIRP_ERROR_TRY_AGAIN:
			errno = EAGAIN;
			break;
		case CHIRP_ERROR_NOT_DIR:
			errno = ENOTDIR;
			break;
		case CHIRP_ERROR_IS_DIR:
			errno = EISDIR;
			break;
		case CHIRP_ERROR_NOT_EMPTY:
			errno = ENOTEMPTY;
			break;
		case CHIRP_ERROR_CROSS_DEVICE_LINK:
			errno = EXDEV;
			break;
		case CHIRP_ERROR_NO_SUCH_PROCESS:
			errno = ESRCH;
			break;
		case CHIRP_ERROR_IS_A_PIPE:
			errno = ESPIPE;
			break;
		case CHIRP_ERROR_GRP_UNREACHABLE:
		case CHIRP_ERROR_TIMED_OUT:
		case CHIRP_ERROR_DISCONNECTED:
		case CHIRP_ERROR_UNKNOWN:
			errno = ECONNRESET;
			break;
		}
		return -1;
	}
}

static INT64_T get_stat_result( struct chirp_client *c, struct chirp_stat *info, time_t stoptime )
{
	char line[CHIRP_LINE_MAX];
	INT64_T fields;

	memset(info,0,sizeof(*info));

	if(!link_readline(c->link,line,CHIRP_LINE_MAX,stoptime)) {
		c->broken = 1;
		errno = ECONNRESET;
		return -1;
	}

	fields = sscanf(line,"%lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %llu %llu %llu\n",
			&info->cst_dev,
			&info->cst_ino,
			&info->cst_mode,
			&info->cst_nlink,
			&info->cst_uid,
			&info->cst_gid,
			&info->cst_rdev,
			&info->cst_size,
			&info->cst_blksize,
			&info->cst_blocks,
			&info->cst_atime,
			&info->cst_mtime,
			&info->cst_ctime);

	info->cst_dev = -1;
	info->cst_rdev = 0;

	if(fields!=13) {
		c->broken = 1;
		errno = ECONNRESET;
		return -1;
	}

	return 0;
}

static INT64_T get_statfs_result( struct chirp_client *c, struct chirp_statfs *info, time_t stoptime )
{
	char line[CHIRP_LINE_MAX];
	INT64_T fields;

	memset(info,0,sizeof(*info));

	if(!link_readline(c->link,line,CHIRP_LINE_MAX,stoptime)) {
		c->broken = 1;
		errno = ECONNRESET;
		return -1;
	}

	fields = sscanf(line,"%lld %lld %lld %lld %lld %lld %lld\n",
			&info->f_type,
			&info->f_bsize,
			&info->f_blocks,
			&info->f_bfree,
			&info->f_bavail,
			&info->f_files,
			&info->f_ffree);

	if(fields!=7) {
		c->broken = 1;
		errno = ECONNRESET;
		return -1;
	}

	return 0;
}

static INT64_T get_result( struct chirp_client *c, time_t stoptime )
{
	char line[CHIRP_LINE_MAX];
	INT64_T result;
	INT64_T fields;

	if(!link_readline(c->link,line,CHIRP_LINE_MAX,stoptime)) {
		c->broken = 1;
		errno = ECONNRESET;
		return -1;
	}

	fields = sscanf(line,"%lld",&result);
	if(fields!=1) {
		errno = ECONNRESET;
		return -1;
		c->broken = 1;
	}

	result = convert_result(result);
	if(result>=0) {
		debug(D_CHIRP," = %lld",result);
	} else {
		debug(D_CHIRP," = %lld (%s)",result,strerror(errno));
	}

	return result;
}

static INT64_T send_command_varargs( struct chirp_client *c, time_t stoptime, char const *fmt, va_list args )
{
	INT64_T	result;
	char    command[CHIRP_LINE_MAX];

	vsprintf(command,fmt,args);

	if(c->broken) {
		errno = ECONNRESET;
		return -1;
	}

	debug(D_CHIRP,"%s: %s",c->hostport,command);

	result = link_write(c->link,command,strlen(command),stoptime);
	if(result < 0) {
		c->broken = 1;
		errno = ECONNRESET;
	}

	return result;
}

static INT64_T send_command( struct chirp_client *c, time_t stoptime, char const *fmt, ... )
{
	INT64_T result;
	va_list args;

	va_start(args,fmt);
	result = send_command_varargs(c,stoptime,fmt,args);
	va_end(args);

	return result;
}

static INT64_T simple_command( struct chirp_client *c, time_t stoptime, char const *fmt, ... )
{
	INT64_T	result;
	va_list args;

	va_start(args,fmt);
	result = send_command_varargs(c,stoptime,fmt,args);
	va_end(args);

	if(result>=0) {
		return get_result(c,stoptime);
	} else {
		return result;
	}
}

