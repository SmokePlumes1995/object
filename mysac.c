/*
 * Copyright (c) 2009 Thierry FOURNIER
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License.
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

#include "mysac_mysql.h"

#include "mysac_decode_field.h"
#include "mysac_decode_row.h"
#include "mysac.h"
#include "mysac_utils.h"

ssize_t mysac_read(int fd, void *buf, size_t count, int *err);
ssize_t mysac_write(int fd, const void *buf, size_t len, int *err);
int mysac_socket_connect(const char *socket_name, int *fd);
int mysac_socket_connect_check(int fd);

enum my_response_t {
	MYSAC_RET_EOF = 1000,
	MYSAC_RET_OK,
	MYSAC_RET_ERROR,
	MYSAC_RET_DATA
};

const char *mysac_type[] = {
	[MYSQL_TYPE_DECIMAL]     = "MYSQL_TYPE_DECIMAL",
	[MYSQL_TYPE_TINY]        = "MYSQL_TYPE_TINY",
	[MYSQL_TYPE_SHORT]       = "MYSQL_TYPE_SHORT",
	[MYSQL_TYPE_LONG]        = "MYSQL_TYPE_LONG",
	[MYSQL_TYPE_FLOAT]       = "MYSQL_TYPE_FLOAT",
	[MYSQL_TYPE_DOUBLE]      = "MYSQL_TYPE_DOUBLE",
	[MYSQL_TYPE_NULL]        = "MYSQL_TYPE_NULL",
	[MYSQL_TYPE_TIMESTAMP]   = "MYSQL_TYPE_TIMESTAMP",
	[MYSQL_TYPE_LONGLONG]    = "MYSQL_TYPE_LONGLONG",
	[MYSQL_TYPE_INT24]       = "MYSQL_TYPE_INT24",
	[MYSQL_TYPE_DATE]        = "MYSQL_TYPE_DATE",
	[MYSQL_TYPE_TIME]        = "MYSQL_TYPE_TIME",
	[MYSQL_TYPE_DATETIME]    = "MYSQL_TYPE_DATETIME",
	[MYSQL_TYPE_YEAR]        = "MYSQL_TYPE_YEAR",
	[MYSQL_TYPE_NEWDATE]     = "MYSQL_TYPE_NEWDATE",
	[MYSQL_TYPE_VARCHAR]     = "MYSQL_TYPE_VARCHAR",
	[MYSQL_TYPE_BIT]         = "MYSQL_TYPE_BIT",
	[MYSQL_TYPE_NEWDECIMAL]  = "MYSQL_TYPE_NEWDECIMAL",
	[MYSQL_TYPE_ENUM]        = "MYSQL_TYPE_ENUM",
	[MYSQL_TYPE_SET]         = "MYSQL_TYPE_SET",
	[MYSQL_TYPE_TINY_BLOB]   = "MYSQL_TYPE_TINY_BLOB",
	[MYSQL_TYPE_MEDIUM_BLOB] = "MYSQL_TYPE_MEDIUM_BLOB",
	[MYSQL_TYPE_LONG_BLOB]   = "MYSQL_TYPE_LONG_BLOB",
	[MYSQL_TYPE_BLOB]        = "MYSQL_TYPE_BLOB",
	[MYSQL_TYPE_VAR_STRING]  = "MYSQL_TYPE_VAR_STRING",
	[MYSQL_TYPE_STRING]      = "MYSQL_TYPE_STRING",
	[MYSQL_TYPE_GEOMETRY]    = "MYSQL_TYPE_GEOMETRY"
};

enum read_state {
	RDST_INIT = 0,
	RDST_READ_LEN,
	RDST_READ_DATA,
	RDST_DECODE_DATA
};

static int my_response(MYSAC *m) {
	int i;
	int err;
	int errcode;
	char *read;
	unsigned long len;
	unsigned long rlen;
	char nul;

	switch ((enum read_state)m->readst) {

	case RDST_INIT:
		m->len = 0;
		m->readst = RDST_READ_LEN;

	/* read length */
	case RDST_READ_LEN:
		/* check for avalaible size in buffer */
		if (m->read_len < 4) {	
			m->errorcode = MYERR_BUFFER_OVERSIZE;
			return MYSAC_RET_ERROR;
		}
		err = mysac_read(m->fd, m->read + m->len,
		                 4 - m->len, &errcode);
		if (err == -1) {
			m->errorcode = errcode;
			return errcode;
		}

		m->len += err;
		if (m->len < 4) {
			m->errorcode = MYERR_WANT_READ;
			return MYERR_WANT_READ;
		}

		/* decode */
		m->packet_length = uint3korr(&m->read[0]);

		/* packet number */
		m->packet_number = m->read[3];

		/* update read state */
		m->readst = RDST_READ_DATA;
		m->len = 0;

	/* read data */
	case RDST_READ_DATA:
		/* check for avalaible size in buffer */
		if (m->read_len < m->packet_length) {	
			m->errorcode = MYERR_BUFFER_OVERSIZE;
			return MYSAC_RET_ERROR;
		}	
		err = mysac_read(m->fd, m->read + m->len,
		                 m->packet_length - m->len, &errcode);
		if (err == -1)
			return errcode;

		m->len += err;
		if (m->len < m->packet_length) {
			m->errorcode = MYERR_WANT_READ;
			return MYERR_WANT_READ;
		}
		m->read_len -= m->packet_length;

		/* re-init eof */
		m->readst = RDST_DECODE_DATA;
		m->eof = 1;

	/* decode data */
	case RDST_DECODE_DATA:

		/* error */
		if ((unsigned char)m->read[0] == 255) {
		
			/* defined mysql error */
			if (m->packet_length > 3) {
	
				/* read error code */
				// TODO: voir quoi foutre de ca plus tard
				// m->errorcode = uint2korr(&m->read[1]);

				/* read mysal code and message */
				for (i=3; i<3+5; i++)
					m->read[i] = m->read[i+1];
				m->read[8] = ' ';
				m->mysql_error = &m->read[3];
				m->read[m->packet_length] = '\0';
				m->errorcode = MYERR_MYSQL_ERROR;
			}
	
			/* unknown error */
			else
				m->errorcode = MYERR_UNKNOWN_ERROR;
	
			return MYSAC_RET_ERROR;
		}
	
		/* EOF marker: marque la fin d'une serie
			(la fin des headers dans une requete) */
		else if ((unsigned char)m->read[0] == 254) {
			m->warnings = uint2korr(&m->read[1]);
			m->status = uint2korr(&m->read[3]);
			m->eof = 1;
			return MYSAC_RET_EOF;
		}
	
		/* success */
		else if ((unsigned char)m->read[0] == 0) {
	
			read = &m->read[1];
			rlen = m->packet_length - 1;

			/* affected rows */
			len = my_lcb(read, &m->affected_rows, &nul, rlen);
			rlen -= len;
			read += len;
			/* m->affected_rows = uint2korr(&m->read[1]); */

			/* insert id */
			len = my_lcb(read, &m->insert_id, &nul, rlen);
			rlen -= len;
			read += len;

			/* server status */
			m->status = uint2korr(read);
			read += 2;
	
			/* server warnings */
			m->warnings = uint2korr(read);
	
			return MYSAC_RET_OK;
		}
	
		/* read response ... 
		 *
		 * Result Set Packet			  1-250 (first byte of Length-Coded Binary)
		 * Field Packet					 1-250 ("")
		 * Row Data Packet				 1-250 ("")
		 */
		else
			return MYSAC_RET_DATA;
	
	default:
		m->errorcode = MYERR_UNEXPECT_R_STATE;
		return MYSAC_RET_ERROR;
	}

	m->errorcode = MYERR_PACKET_CORRUPT;
	return MYSAC_RET_ERROR;
}

void mysac_init(MYSAC *mysac, char *buffer, unsigned int buffsize) {

	/* init */
	memset(mysac, 0, sizeof(MYSAC));
	mysac->qst = MYSAC_START;
	mysac->buf = buffer;
	mysac->bufsize = buffsize;
}

MYSAC *mysac_new(int buffsize) {
	MYSAC *m;
	char *buf;

	/* struct memory */
	m = malloc(sizeof(MYSAC));
	if (m == NULL)
		return NULL;
	
	/* buff memory */
	buf = malloc(buffsize);
	if (buf == NULL) {
		free(m);
		return NULL;
	}

	/* init */
	memset(m, 0, sizeof(MYSAC));
	m->free_it = 1;
	m->qst = MYSAC_START;
	m->buf = buf;
	m->bufsize = buffsize;

	return m;
}

void mysac_setup(MYSAC *mysac, const char *my_addr, const char *user,
                 const char *passwd, const char *db,
                 unsigned long client_flag) {
	mysac->addr     = my_addr;
	mysac->login    = user;
	mysac->password = passwd;
	mysac->database = db;
	mysac->flags    = client_flag;
}

int mysac_connect(MYSAC *mysac) {
	int err;
	int errcode;
	int i;
	int len;

	switch (mysac->qst) {

	/***********************************************
	 network connexion
	***********************************************/
	case MYSAC_START:
    printf("mysac start\n");
		err = mysac_socket_connect(mysac->addr, &mysac->fd);
		if (err != 0) {
      printf("err: %d\n", err);
			mysac->qst = MYSAC_START;
			mysac->errorcode = err;
			return err;
		}
		mysac->qst = MYSAC_CONN_CHECK;
		return MYERR_WANT_READ;

	/***********************************************
	 check network connexion
	***********************************************/
	case MYSAC_CONN_CHECK:
    printf("mysac conn check\n");
		err = mysac_socket_connect_check(mysac->fd);
		if (err != 0) {
			close(mysac->fd);
			mysac->qst = MYSAC_START;
			mysac->errorcode = err;
			return err;
		}
		mysac->qst = MYSAC_READ_GREATINGS;
		mysac->len = 0;
		mysac->readst = 0;
		mysac->read = mysac->buf;
		mysac->read_len = mysac->bufsize;

	/***********************************************
	 read greatings
	***********************************************/
	case MYSAC_READ_GREATINGS:
    printf("mysac read greatings\n");

		err = my_response(mysac);

		if (err == MYERR_WANT_READ)
			return MYERR_WANT_READ;

		/* error */
		else if (err == MYSAC_RET_ERROR)
			return mysac->errorcode;

		/* ok */
		else if (err != MYSAC_RET_DATA) {
			mysac->errorcode = MYERR_PROTOCOL_ERROR;
			return mysac->errorcode;
		}

		/* decode greatings */
		i = 0;

		/* protocol */
		mysac->protocol = mysac->buf[i];
		i++;

		/* version */
		mysac->version = &mysac->buf[i];

		/* search \0 */
		while (mysac->buf[i] != 0)
			i++;
		i++;

		/* thread id */
		mysac->threadid = uint4korr(&mysac->buf[i]);

		/* first part of salt */
		strncpy(mysac->salt, &mysac->buf[i+4], SCRAMBLE_LENGTH_323);
		i += 4 + SCRAMBLE_LENGTH_323 + 1;

		/* options */
		mysac->options = uint2korr(&mysac->buf[i]);

		/* charset */
		mysac->charset = mysac->buf[i+2];

		/* server status */
		mysac->status = uint2korr(&mysac->buf[i+3]);

		/* salt part 2 */
		strncpy(mysac->salt + SCRAMBLE_LENGTH_323, &mysac->buf[i+5+13],
		        SCRAMBLE_LENGTH - SCRAMBLE_LENGTH_323);
		mysac->salt[SCRAMBLE_LENGTH] = '\0';

		/* checks */
		if (mysac->protocol != PROTOCOL_VERSION)
			return CR_VERSION_ERROR;

		/********************************
		  prepare auth packet 
		********************************/

		/* set m->buf number */
		mysac->packet_number++;
		mysac->buf[3] = mysac->packet_number;
		
		/* set options */
		if (mysac->options & CLIENT_LONG_PASSWORD)
			mysac->flags |= CLIENT_LONG_PASSWORD;
		mysac->flags |= CLIENT_LONG_FLAG   |
		                CLIENT_PROTOCOL_41 |
		                CLIENT_SECURE_CONNECTION;
		to_my_2(mysac->flags, &mysac->buf[4]);
		
		/* set extended options */
		to_my_2(0, &mysac->buf[6]);

		/* max m->bufs */
		to_my_4(0x40000000, &mysac->buf[8]);

		/* charset */
		/* 8: swedish */
		mysac->buf[12] = 8;
		
		/* 24 unused */
		memset(&mysac->buf[13], 0, 24);
		
		/* username */
		strcpy(&mysac->buf[36], mysac->login);
		i = 36 + strlen(mysac->login) + 1;

		/* password CLIENT_SECURE_CONNECTION */
		if (mysac->options & CLIENT_SECURE_CONNECTION) {

			/* the password hash len */
			mysac->buf[i] = SCRAMBLE_LENGTH;
			i++;
			scramble(&mysac->buf[i], mysac->salt, mysac->password);
			i += SCRAMBLE_LENGTH;
		}
		
		/* password ! CLIENT_SECURE_CONNECTION */
		else {
			scramble_323(&mysac->buf[i], mysac->salt, mysac->password);
			i += SCRAMBLE_LENGTH_323 + 1;
		}
		
		/* Add database if needed */
		if ((mysac->options & CLIENT_CONNECT_WITH_DB) && 
		    (mysac->database != NULL)) {
			/* TODO : debordement de buffer */
			len = strlen(mysac->database);
			memcpy(&mysac->buf[i], mysac->database, len);
			i += len;
			mysac->buf[i] = '\0';
		}

		/* len */
		to_my_3(i-4, &mysac->buf[0]);
		mysac->len = i;
		mysac->send = mysac->buf;
		mysac->qst = MYSAC_SEND_AUTH_1;

	/***********************************************
	 send paquet
	***********************************************/
	case MYSAC_SEND_AUTH_1:
    printf("mysac send auth_1\n");
		err = mysac_write(mysac->fd, mysac->send, mysac->len, &errcode);

		if (err == -1)
			return errcode;

		mysac->len -= err;
		mysac->send += err;
		if (mysac->len > 0)
			return MYERR_WANT_WRITE;

		mysac->qst = MYSAC_RECV_AUTH_1;
		mysac->readst = 0;
		mysac->read = mysac->buf;
		mysac->read_len = mysac->bufsize;

	/***********************************************
	 read response 1
	***********************************************/
	case_MYSAC_RECV_AUTH_1:
	case MYSAC_RECV_AUTH_1:
    printf("mysac recv auth_1\n");
	/*
		MYSAC_RET_EOF,
		MYSAC_RET_OK,
		MYSAC_RET_ERROR,
		MYSAC_RET_DATA
	*/
		err = my_response(mysac);

		if (err == MYERR_WANT_READ)
			return MYERR_WANT_READ;

		/* error */
		if (err == MYSAC_RET_ERROR)
			return mysac->errorcode;

		/* ok */
		else if (err == MYSAC_RET_OK)
			return 0;

		/*
		   By sending this very specific reply server asks us to send scrambled
		   password in old format.
		*/
		else if (mysac->packet_length == 1 && err == MYSAC_RET_EOF && 
		         mysac->options & CLIENT_SECURE_CONNECTION) {
			/* continue special paquet after conditions */
		}

		/* protocol error */
		else {
			mysac->errorcode = MYERR_PROTOCOL_ERROR;
			return mysac->errorcode;
		}

		/* send scrambled password in old format */

		/* set packet number */
		mysac->packet_number++;
		mysac->buf[3] = mysac->packet_number;
		
		/* send scrambled password in old format. */
		scramble_323(&mysac->buf[4], mysac->salt, mysac->password);
		mysac->buf[4+SCRAMBLE_LENGTH_323] = '\0';

		/* len */
		to_my_3(SCRAMBLE_LENGTH_323+1, &mysac->buf[0]);
		mysac->qst = MYSAC_SEND_AUTH_2;
		mysac->len = SCRAMBLE_LENGTH_323 + 1 + 4;
		mysac->send = mysac->buf;

	/* send scrambled password in old format */
	case MYSAC_SEND_AUTH_2:
    printf("mysac send auth_2\n");
		err = mysac_write(mysac->fd, mysac->send, mysac->len, &errcode);

		if (err == -1)
			return errcode;

		mysac->len -= err;
		mysac->send += err;
		if (mysac->len > 0)
			return MYERR_WANT_WRITE;

		mysac->qst = MYSAC_RECV_AUTH_1;
		mysac->readst = 0;
		mysac->read = mysac->buf;
		mysac->read_len = mysac->bufsize;
		goto case_MYSAC_RECV_AUTH_1;
		
	default:
    /* TODO: more types or error? */
    /*mysac->errorcode = MYERR_UNEXPECT_R_STATE;
    return MYSAC_RET_ERROR;*/
    break;
	}

	return 0;
}

int mysac_set_database(MYSAC *mysac, const char *database) {
	int i;

	/* set packet number */
	mysac->buf[3] = 0;

	/* set mysql command */
	mysac->buf[4] = COM_INIT_DB;

	/* build sql query */
	i = strlen(database);
	memcpy(&mysac->buf[5], database, i);

	/* len */
	to_my_3(i + 1, &mysac->buf[0]);

	/* send params */
	mysac->send = mysac->buf;
	mysac->len = i + 5;
	mysac->qst = MYSAC_SEND_INIT_DB;

	return 0;
}

int mysac_send_database(MYSAC *mysac) {
	int err;
	int errcode;

	switch (mysac->qst) {

	/**********************************************************
	*
	* send query on network
	*
	**********************************************************/
	case MYSAC_SEND_INIT_DB:
		err = mysac_write(mysac->fd, mysac->send, mysac->len, &errcode);

		if (err == -1)
			return errcode;

		mysac->len -= err;
		mysac->send += err;
		if (mysac->len > 0)
			return MYERR_WANT_WRITE;
		mysac->qst = MYSAC_RECV_INIT_DB;
		mysac->readst = 0;
		mysac->read = mysac->buf;
	
	/**********************************************************
	*
	* receive
	*
	**********************************************************/
	case MYSAC_RECV_INIT_DB:
		err = my_response(mysac);

		if (err == MYERR_WANT_READ)
			return MYERR_WANT_READ;

		/* error */
		if (err == MYSAC_RET_ERROR)
			return mysac->errorcode;

		/* protocol error */
		else if (err == MYSAC_RET_OK)
			return 0;

		else {
			mysac->errorcode = MYERR_PROTOCOL_ERROR;
			return mysac->errorcode;
		}
	default:
    /* TODO: more types or error? */
    /*mysac->errorcode = MYERR_UNEXPECT_R_STATE;
    return MYSAC_RET_ERROR;*/
    break;
	}
  return MYSAC_RET_ERROR;
}

int mysac_set_stmt_prepare(MYSAC *mysac, const char *fmt, ...) {
	va_list ap;
	int len;

	/* set packet number */
	mysac->buf[3] = 0;

	/* set mysql command */
	mysac->buf[4] = COM_STMT_PREPARE;

	/* build sql query */
	va_start(ap, fmt);
	len = vsnprintf(&mysac->buf[5], mysac->bufsize - 5, fmt, ap);
	if (len >= mysac->bufsize - 5)
		return -1;

	/* len */
	to_my_3(len + 1, &mysac->buf[0]);

	/* send params */
	mysac->send = mysac->buf;
	mysac->len = len + 5;
	mysac->qst = MYSAC_SEND_STMT_QUERY;

	return 0;
}

int mysac_send_stmt_prepare(MYSAC *mysac, unsigned long *stmt_id) {
	int err;
	int errcode;

	switch (mysac->qst) {

	/**********************************************************
	*
	* send query on network
	*
	**********************************************************/
	case MYSAC_SEND_STMT_QUERY:
		err = mysac_write(mysac->fd, mysac->send, mysac->len, &errcode);

		if (err == -1)
			return errcode;

		mysac->len -= err;
		mysac->send += err;
		if (mysac->len > 0)
			return MYERR_WANT_WRITE;
		mysac->qst = MYSAC_RECV_STMT_QUERY;
		mysac->readst = 0;
		mysac->read = mysac->buf;
	
	/**********************************************************
	*
	* receive
	*
	**********************************************************/
	case MYSAC_RECV_STMT_QUERY:
		err = my_response(mysac);

		if (err == MYERR_WANT_READ)
			return MYERR_WANT_READ;

		/* error */
		if (err == MYSAC_RET_ERROR)
			return mysac->errorcode;

		/* protocol error */
		if (err != MYSAC_RET_OK) {
			mysac->errorcode = MYERR_PROTOCOL_ERROR;
			return mysac->errorcode;
		}

		/* get statement id */
		*stmt_id = uint4korr(&mysac->buf[1]);

		/* get nb of columns */
		/* TODO prendre en compte cette valeur plus tard */
		/* mysac->nb_cols = mysac->buf[5]; */
		mysac->res = *(MYSAC_RES **)&mysac->buf[5];

		mysac->read_id = 0;

		mysac->qst = MYSAC_RECV_QUERY_COLDESC;

	/**********************************************************
	*
	* receive column description
	*
	**********************************************************/
	case_MYSAC_RECV_QUERY_COLDESC_2:
	mysac->readst = 0;

	case MYSAC_RECV_QUERY_COLDESC:

		err = my_response(mysac);

		if (err == MYERR_WANT_READ)
			return MYERR_WANT_READ;

		/* error */
		if (err == MYSAC_RET_ERROR)
			return mysac->errorcode;

		/* protocol error */
		else if (err != MYSAC_RET_DATA) {
			mysac->errorcode = MYERR_PROTOCOL_ERROR;
			return mysac->errorcode;
		}

		/* TODO: voir plus atrd pour deceoder �a ... */
		/* mysac_decode_field(mysac, &mysac->cols[mysac->read_id]); */

		/* TODO ... un peu pourri */
		mysac->read_id++;
		/* if (mysac->read_id < mysac->nb_cols) */
		if (mysac->read_id < (long)mysac->res)
			goto case_MYSAC_RECV_QUERY_COLDESC_2;
		
		mysac->readst = 0;
		mysac->qst = MYSAC_RECV_QUERY_EOF1;
		mysac->read = mysac->buf;
	
	/**********************************************************
	*
	* receive EOF
	*
	**********************************************************/
	case MYSAC_RECV_QUERY_EOF1:
		err = my_response(mysac);

		if (err == MYERR_WANT_READ)
			return MYERR_WANT_READ;

		/* error */
		if (err == MYSAC_RET_ERROR)
			return mysac->errorcode;

		/* protocol error */
		else if (err != MYSAC_RET_EOF) {
			mysac->errorcode = MYERR_PROTOCOL_ERROR;
			return mysac->errorcode;
		}

		return 0;
	default:
    /* TODO: more types or error? */
    /*mysac->errorcode = MYERR_UNEXPECT_R_STATE;
    return MYSAC_RET_ERROR;*/
    break;
	}
  return MYSAC_RET_ERROR;
}



int mysac_set_stmt_execute(MYSAC *mysac, MYSAC_RES *res, unsigned long stmt_id) {

	/* set packet number */
	mysac->buf[3] = 0;

	/* len */
	to_my_3(5, &mysac->buf[0]);

	/* set mysql command */
	mysac->buf[4] = COM_STMT_EXECUTE;

	/* build sql query */
	to_my_4(stmt_id, &mysac->buf[5]);

	/* send params */
	mysac->res = res;
	mysac->send = mysac->buf;
	mysac->len = 9;
	mysac->qst = MYSAC_SEND_QUERY;

	return 0;
}

inline
int mysac_b_set_query(MYSAC *mysac, MYSAC_RES *res, const char *query, int len) {

	/* set packet number */
	mysac->buf[3] = 0;

	/* set mysql command */
	mysac->buf[4] = COM_QUERY;

	/* build sql query */
	if (mysac->bufsize - 5 < len) {
		mysac->errorcode = MYERR_BUFFER_TOO_SMALL;
		mysac->len = 0;
		return -1;
	}
	memcpy(&mysac->buf[5], query, len);

	/* len */
	to_my_3(len + 1, &mysac->buf[0]);

	/* send params */
	mysac->res = res;
	mysac->send = mysac->buf;
	mysac->len = len + 5;
	mysac->qst = MYSAC_SEND_QUERY;

	return 0;
}

int mysac_s_set_query(MYSAC *mysac, MYSAC_RES *res, const char *query) {
	return mysac_b_set_query(mysac, res, query, strlen(query));
}

inline
int mysac_v_set_query(MYSAC *mysac, MYSAC_RES *res, const char *fmt, va_list ap) {
	int len;

	/* set packet number */
	mysac->buf[3] = 0;

	/* set mysql command */
	mysac->buf[4] = COM_QUERY;

	/* build sql query */
	len = vsnprintf(&mysac->buf[5], mysac->bufsize - 5, fmt, ap);
	if (len >= mysac->bufsize - 5) {
		mysac->errorcode = MYERR_BUFFER_TOO_SMALL;
		mysac->len = 0;
		return -1;
	}

	/* len */
	to_my_3(len + 1, &mysac->buf[0]);

	/* send params */
	mysac->res = res;
	mysac->send = mysac->buf;
	mysac->len = len + 5;
	mysac->qst = MYSAC_SEND_QUERY;

	return 0;
}

int mysac_set_query(MYSAC *mysac, MYSAC_RES *res, const char *fmt, ...) {
	va_list ap;

	va_start(ap, fmt);
	return mysac_v_set_query(mysac, res, fmt, ap);
}

int mysac_send_query(MYSAC *mysac) {
	int err;
	int errcode;
	int i;
	int len;
	MYSAC_RES *res;

	res = mysac->res;

	switch (mysac->qst) {

	/**********************************************************
	*
	* send query on network
	*
	**********************************************************/
	case MYSAC_SEND_QUERY:
		err = mysac_write(mysac->fd, mysac->send, mysac->len, &errcode);

		if (err == -1)
			return errcode;

		mysac->len -= err;
		mysac->send += err;
		if (mysac->len > 0)
			return MYERR_WANT_WRITE;
		mysac->qst = MYSAC_RECV_QUERY_COLNUM;
		mysac->readst = 0;
	
	/**********************************************************
	*
	* receive
	*
	**********************************************************/

	/* prepare struct 

	 +---------------+-----------------+
	 | MYSQL_FIELD[] | char[]          |
	 | resp->nb_cols | all fields name |
	 +---------------+-----------------+

	 */

	res->nb_lines = 0;
	res->cols = (MYSQL_FIELD *)(res->buffer + sizeof(MYSAC_RES));
	INIT_LIST_HEAD(&res->data);
	mysac->read = res->buffer;
	mysac->read_len = res->buffer_len;

	case MYSAC_RECV_QUERY_COLNUM:
		err = my_response(mysac);

		if (err == MYERR_WANT_READ)
			return MYERR_WANT_READ;

		/* error */
		if (err == MYSAC_RET_ERROR)
			return mysac->errorcode;

		/* ok ( for insert or no return data command ) */
		if (err == MYSAC_RET_OK)
			return 0;

		/* protocol error */
		if (err != MYSAC_RET_DATA) {
			mysac->errorcode = MYERR_PROTOCOL_ERROR;
			return mysac->errorcode;
		}

		/* get nb col TODO: pas sur que ce soit un byte */
		res->nb_cols = mysac->read[0];
		mysac->read_id = 0;
		mysac->qst = MYSAC_RECV_QUERY_COLDESC;
	
		/* prepare cols space */

		/* check for avalaible size in buffer */
		if (mysac->read_len < sizeof(MYSQL_FIELD) * res->nb_cols) {
			mysac->errorcode = MYERR_BUFFER_OVERSIZE;
			return mysac->errorcode;
		}
		res->cols = (MYSQL_FIELD *)mysac->read;
		mysac->read += sizeof(MYSQL_FIELD) * mysac->res->nb_cols;
		mysac->read_len -= sizeof(MYSQL_FIELD) * mysac->res->nb_cols;

	/**********************************************************
	*
	* receive column description
	*
	**********************************************************/

	case_MYSAC_RECV_QUERY_COLDESC:
	mysac->readst = 0;

	case MYSAC_RECV_QUERY_COLDESC:

		err = my_response(mysac);

		if (err == MYERR_WANT_READ)
			return MYERR_WANT_READ;

		/* error */
		if (err == MYSAC_RET_ERROR)
			return mysac->errorcode;

		/* protocol error */
		else if (err != MYSAC_RET_DATA) {
			mysac->errorcode = MYERR_PROTOCOL_ERROR;
			return mysac->errorcode;
		}

		/* decode mysql packet with field desc, use packet buffer for storing
		   mysql data (field name) */
#if 0
		for (i=0; i<mysac->packet_length; i++) {
			fprintf(stderr, "%02x ", (unsigned char)mysac->read[i]);
		}
		fprintf(stderr, "\n");
#endif
		len = mysac_decode_field(mysac->read, mysac->packet_length,
		                         &res->cols[mysac->read_id]);

		if (len < 0) {
			mysac->errorcode = len * -1;
			return mysac->errorcode;
		}
		mysac->read += len;
		mysac->read_len += mysac->packet_length - len;

		mysac->read_id++;
		if (mysac->read_id < res->nb_cols)
			goto case_MYSAC_RECV_QUERY_COLDESC;
		
		mysac->readst = 0;
		mysac->qst = MYSAC_RECV_QUERY_EOF1;
	
	/**********************************************************
	*
	* receive EOF
	*
	**********************************************************/
	case MYSAC_RECV_QUERY_EOF1:
		err = my_response(mysac);

		if (err == MYERR_WANT_READ)
			return MYERR_WANT_READ;

		/* error */
		if (err == MYSAC_RET_ERROR)
			return mysac->errorcode;

		/* protocol error */
		else if (err != MYSAC_RET_EOF) {
			mysac->errorcode = MYERR_PROTOCOL_ERROR;
			return mysac->errorcode;
		}

		mysac->qst = MYSAC_RECV_QUERY_DATA;

	/**********************************************************
	*
	* read data lines
	*
	**********************************************************/
	case_MYSAC_RECV_QUERY_DATA:

	/*
	   +-------------------+----------------+-----------------+----------------+
	   | struct mysac_rows | MYSAC_ROW[]    | unsigned long[] | struct tm[]    |
	   |                   | mysac->nb_cols | mysac->nb_cols  | mysac->nb_time |
	   +-------------------+----------------+-----------------+----------------+
	 */

	/* check for avalaible size in buffer */
	if (mysac->read_len < sizeof(MYSAC_ROWS) + ( res->nb_cols * (
	                      sizeof(MYSAC_ROW) + sizeof(unsigned long) ) ) ) {
		mysac->errorcode = MYERR_BUFFER_OVERSIZE;
		return mysac->errorcode;
	}
	mysac->read_len -= sizeof(MYSAC_ROWS) + ( res->nb_cols * (
	                   sizeof(MYSAC_ROW) + sizeof(unsigned long) ) );

	/* reserve space for MYSAC_ROWS and add it into chained list */
	res->cr = (MYSAC_ROWS *)mysac->read;
	list_add_tail(&res->cr->link, &res->data);
	mysac->read += sizeof(MYSAC_ROWS);

	/* space for each field definition into row */
	res->cr->data = (MYSAC_ROW *)mysac->read;
	mysac->read += sizeof(MYSAC_ROW) * res->nb_cols;

	/* space for length table */
	res->cr->lengths = (unsigned long *)mysac->read;
	mysac->read += sizeof(unsigned long) * res->nb_cols;

	/* struct tm */
	for (i=0; i<mysac->res->nb_cols; i++) {
		switch(mysac->res->cols[i].type) {
		case MYSQL_TYPE_TIME:
		case MYSQL_TYPE_YEAR:
		case MYSQL_TYPE_TIMESTAMP:
		case MYSQL_TYPE_DATETIME:
		case MYSQL_TYPE_DATE:
			if (mysac->read_len < sizeof(struct tm)) {
				mysac->errorcode = MYERR_BUFFER_OVERSIZE;
				return mysac->errorcode;
			}
			mysac->res->cr->data[i].tm = (struct tm *)mysac->read;
			mysac->read += sizeof(struct tm);
			mysac->read_len -= sizeof(struct tm);
			memset(mysac->res->cr->data[i].tm, 0, sizeof(struct tm));
    default:
      /* TODO: more types or error? */
      /*mysac->errorcode = MYERR_UNEXPECT_R_STATE;
      return MYSAC_RET_ERROR;*/
      break;
		}
	}

	/* set state at 0 */
	mysac->readst = 0;

	case MYSAC_RECV_QUERY_DATA:
		err = my_response(mysac);

		if (err == MYERR_WANT_READ)
			return MYERR_WANT_READ;

		/* error */
		else if (err == MYSAC_RET_ERROR)
			return mysac->errorcode;

		/* EOF */
		else if (err == MYSAC_RET_EOF) {
			list_del(&mysac->res->cr->link);
			mysac->res->cr = NULL;
			return 0;
		}

		/* read data in string type */
		else if (err == MYSAC_RET_DATA) {
#if 0
			for (i=0; i<mysac->packet_length; i+=20) {
				int j;

				for(j=i;j<i+20;j++)
					fprintf(stderr, "%02x ", (unsigned char)mysac->read[j]);

				for(j=i;j<i+20;j++)
					if (isprint(mysac->read[j]))
						fprintf(stderr, "%c", (unsigned char)mysac->read[j]);
					else
						fprintf(stderr, ".");

				fprintf(stderr, "\n");
			}
			fprintf(stderr, "\n\n");
#endif
			len = mysac_decode_string_row(mysac->read, mysac->packet_length,
			                              res, res->cr);
			if (len < 0) {
				mysac->errorcode = len * -1;
				return mysac->errorcode;
			}
			mysac->read += len;
			mysac->read_len += mysac->packet_length - len;
		}

		/* read data in binary type */
		else if (err == MYSAC_RET_OK) {
#if 0
			for (i=0; i<mysac->packet_length; i++) {
				fprintf(stderr, "%02x ", (unsigned char)mysac->read[i]);
			}
			fprintf(stderr, "\n");
#endif
			len = mysac_decode_binary_row(mysac->read, mysac->packet_length,
			                              res, res->cr);
			if (len == -1) {
				mysac->errorcode = MYERR_BINFIELD_CORRUPT;
				return mysac->errorcode;
			}
			mysac->read += len;
			mysac->read_len += mysac->packet_length - len;
		}

		/* protocol error */
		else {
			mysac->errorcode = MYERR_PROTOCOL_ERROR;
			return mysac->errorcode;
		}

		/* next line */
		mysac->res->nb_lines++;
		goto case_MYSAC_RECV_QUERY_DATA;
	default:
    /* TODO: more types or error? */
    /*mysac->errorcode = MYERR_UNEXPECT_R_STATE;
    return MYSAC_RET_ERROR;*/
    break;
	}

  /* XXX: return a value? */
  return MYSAC_RET_ERROR;
}

