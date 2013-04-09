/*
 * Copyright (c) 2002 - 2003
 * NetGroup, Politecnico di Torino (Italy)
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions 
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright 
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright 
 * notice, this list of conditions and the following disclaimer in the 
 * documentation and/or other materials provided with the distribution. 
 * 3. Neither the name of the Politecnico di Torino nor the names of its 
 * contributors may be used to endorse or promote products derived from 
 * this software without specific prior written permission. 
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR 
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT 
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 */

#include <pcap.h>		// for libpcap/WinPcap calls
#include <errno.h>		// for the errno variable
#include <stdlib.h>		// for malloc(), free(), ...
#include <string.h>		// for strlen(), ...
#include <pthread.h>
#include "pcap-remote.h"
#include "rpcapd.h"
#include "daemon.h"
#include "sockutils.h"	// for socket calls

#ifndef WIN32			// for select() and such
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <pwd.h>		// for password management
#endif

#ifdef linux
#include <fcntl.h>
#include <shadow.h>		// for password management
#include <linux/if_packet.h>

int
set_non_blocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    flags |= O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
}
#endif

struct daemon_ctx {
    pcap_t *fp;
    /*! \brief '1' if we're the network client; needed by several functions (like pcap_setfilter() ) to know if
        they have to use the socket or they have to open the local adapter. */
    int rmt_clientside;

    SOCKET rmt_sockctrl;        //!< socket ID of the socket used for the control connection
    SOCKET rmt_sockdata;        //!< socket ID of the socket used for the data connection
    int rmt_flags;              //!< we have to save flags, since they are passed by the pcap_open_live(), but they are used by the pcap_startcapture()
    int rmt_capstarted;         //!< 'true' if the capture is already started (needed to knoe if we have to call the pcap_startcapture()
    struct pcap_samp rmt_samp;  //!< Keeps the parameters related to the sampling process.
    char *currentfilter;        //!< Pointer to a buffer (allocated at run-time) that stores the current filter. Needed when flag PCAP_OPENFLAG_NOCAPTURE_RPCAP is turned on.

    unsigned int TotCapt;
    unsigned int svrcapt;
    unsigned int eagain_pkts;
    unsigned int eagain_sleep;
    unsigned int read_timeouts;
    unsigned int sendring_full;
    unsigned int sendring_buf_full;
    unsigned int sendring_full_sleep;
    unsigned int sendring_buf_full_sleep;
    unsigned int sendthr_sleep;

    int cb_rc;
    char *sendbuf;
    char *errbuf;
};

pcap_t *pcap_open_live_ex(const char *source, int buffer_size, int snaplen, int promisc, int to_ms, char *errbuf)
{
    pcap_t *p;
    int status;

    p = pcap_create(source, errbuf);
    if (p == NULL)
        return (NULL);
    status = pcap_set_buffer_size(p, buffer_size);
    if (status < 0) {
        printf("set_buffer_size failed\n");
    }
    status = pcap_set_snaplen(p, snaplen);
    if (status < 0)
        goto fail;
    status = pcap_set_promisc(p, promisc);
    if (status < 0)
        goto fail;
    status = pcap_set_timeout(p, to_ms);
    if (status < 0)
        goto fail;
    status = pcap_activate(p);
    if (status < 0)
        goto fail;
    return (p);
fail:
    pcap_close(p);
    return (NULL);
}

pcap_t *pcap_open_ex(const char *source, int buffer_size, int snaplen, int flags, int read_timeout, struct pcap_rmtauth *auth, char *errbuf)
{
char host[PCAP_BUF_SIZE], port[PCAP_BUF_SIZE], name[PCAP_BUF_SIZE];
int type;
pcap_t *fp;
int result;

    if (strlen(source) > PCAP_BUF_SIZE)
    {
        snprintf(errbuf, PCAP_ERRBUF_SIZE, "The source string is too long. Cannot handle it correctly.");
        return NULL;
    }

    // determine the type of the source (file, local, remote)
    if (pcap_parsesrcstr(source, &type, host, port, name, errbuf) == -1)
        return NULL;

    if (type != PCAP_SRC_IFLOCAL) {
        snprintf(errbuf, PCAP_ERRBUF_SIZE, "Source is not local interface.");
        return NULL;
    }

    return pcap_open_live_ex(name, buffer_size, snaplen, (flags & PCAP_OPENFLAG_PROMISCUOUS), read_timeout, errbuf);
}

static struct daemon_ctx *
daemon_ctx_open(const char *source, int snaplen, int flags, int read_timeout, struct pcap_rmtauth *auth, char *errbuf)
{
    struct daemon_ctx *fp = malloc(sizeof(struct daemon_ctx));
    if (fp) {
        memset(fp, 0, sizeof(struct daemon_ctx));
        fp->fp = pcap_open_ex(source, rpcapd_opt.ringbuf_max_pkt_data, snaplen, (flags & PCAP_OPENFLAG_PROMISCUOUS), read_timeout, NULL, errbuf);
        if (!fp->fp) {
            free(fp);
            fp = NULL;
        }
    }
    else {
        snprintf(errbuf, PCAP_ERRBUF_SIZE, "malloc failed");
    }
    return fp;
}

static void
daemon_ctx_close(struct daemon_ctx *fp)
{
    if (fp->fp) {
        pcap_close(fp->fp);
        fp->fp = NULL;
    }
    free(fp);
}

// Locally defined functions
int daemon_checkauth(SOCKET sockctrl, int nullAuthAllowed, char *errbuf);
int daemon_AuthUserPwd(char *username, char *password, char *errbuf);

int daemon_findalldevs(SOCKET sockctrl, char *errbuf);

int daemon_opensource(SOCKET sockctrl, char *source, int srclen, uint32 plen, char *errbuf);
struct daemon_ctx *daemon_startcapture(SOCKET sockctrl, pthread_t *threaddata, char *source, int active,
							struct rpcap_sampling *samp_param, uint32 plen, char *errbuf);
int daemon_endcapture(struct daemon_ctx *fp, pthread_t *threaddata, char *errbuf);

int daemon_updatefilter(struct daemon_ctx *fp, uint32 plen);
int daemon_unpackapplyfilter(struct daemon_ctx *fp, unsigned int *nread, int *plen, char *errbuf);

int daemon_getstats(struct daemon_ctx *fp);
int daemon_getstatsnopcap(SOCKET sockctrl, unsigned int ifdrops, unsigned int ifrecv, 
						  unsigned int krnldrop, unsigned int svrcapt, char *errbuf);

int daemon_setsampling(SOCKET sockctrl, struct rpcap_sampling *samp_param, int plen, char *errbuf);

void daemon_seraddr(struct sockaddr_storage *sockaddrin, struct sockaddr_storage *sockaddrout);
void *daemon_thrdatamain(void *ptr);





/*!
	\brief Main serving funtion
	This function is the one which does the job. It is the main() of the child
	thread, which is created as soon as a new connection is accepted.

	\param ptr: a void pointer that keeps the reference of the 'pthread_chain'
	value corrisponding to this thread. This variable is casted into a 'pthread_chain'
	value in order to retrieve the socket we're currently using, the therad ID, and 
	some pointers to the previous and next elements into this struct.

	\return None.
*/
void daemon_serviceloop( void *ptr )
{
char errbuf[PCAP_ERRBUF_SIZE + 1];		// keeps the error string, prior to be printed
char source[PCAP_BUF_SIZE];				// keeps the string that contains the interface to open
struct rpcap_header header;				// RPCAP message general header
struct daemon_ctx *fp= NULL;
struct daemon_slpars *pars;				// parameters related to the present daemon loop

pthread_t threaddata= 0;				// handle to the 'read from daemon and send to client' thread

unsigned int ifdrops, ifrecv, krnldrop, svrcapt;	// needed to save the values of the statistics

struct rpcap_sampling samp_param;		// in case sampling has been requested

// Structures needed for the select() call
fd_set rfds;						// set of socket descriptors we have to check
struct timeval tv;					// maximum time the select() can block waiting for data
int retval;							// select() return value


	pars= (struct daemon_slpars *) ptr;
	
	*errbuf= 0;	// Initialize errbuf

	// If we're in active mode, this is not a separate thread
	if (! pars->isactive)
	{
		// Modify thread params so that it can be killed at any time
		if (pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL) )
			goto end;
		if (pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL) )
			goto end;
	}

auth_again:
	// If we're in active mode, we have to check for the initial timeout
	if (!pars->isactive)
	{
		FD_ZERO(&rfds);
		// We do not have to block here
		tv.tv_sec = RPCAP_TIMEOUT_INIT;
		tv.tv_usec = 0;
		
		FD_SET(pars->sockctrl, &rfds);

		retval = select(pars->sockctrl + 1, &rfds, NULL, NULL, &tv);
		if (retval == -1)
		{
			sock_geterror("select(): ", errbuf, PCAP_ERRBUF_SIZE);
			rpcap_senderror(pars->sockctrl, errbuf, PCAP_ERR_NETW, NULL);
			goto end;
		}

		// The timeout has expired
		// So, this was a fake connection. Drop it down
		if (retval == 0)
		{
			rpcap_senderror(pars->sockctrl, "The RPCAP initial timeout has expired", PCAP_ERR_INITTIMEOUT, NULL);
			goto end;
		}
	}


	retval= daemon_checkauth(pars->sockctrl, pars->nullAuthAllowed, errbuf);

	if (retval)
	{
		// the other user requested to close the connection
		// It can be also the case of 'active mode', in which this host is not
		// allowed to connect to the other peer; in that case, it drops down the connection
		if (retval == -3) 
			goto end;

		// It can be an authentication failure or an unrecoverable error
		rpcap_senderror(pars->sockctrl, errbuf, PCAP_ERR_AUTH, NULL);

		// authentication error
		if (retval == -2)
		{
			// suspend for 1 sec
			// WARNING: this day is inserted only in this point; if the user drops down the connection
			// and it connects again, this suspension time does not have any effects.
			pthread_suspend(RPCAP_SUSPEND_WRONGAUTH*1000);
			goto auth_again;
		}

		 // Unrecoverable error
		if (retval == -1)
			goto end;
	}

	memset(&samp_param, 0, sizeof(samp_param));

	while (1)
	{
	int retval;

		errbuf[0]= 0;	// clear errbuf

		// Avoid zombies connections; check if the connection is opens but no commands are performed
		// from more than RPCAP_TIMEOUT_RUNTIME
		// Conditions:
		// - I have to be in normal mode (no active mode)
		// - if the device is open, I don't have to be in the middle of a capture (fp->rmt_sockdata)
		// - if the device is closed, I have always to check if a new command arrives
		//
		// Be carefully: the capture can have been started, but an error occurred (so fp != NULL, but
		//  rmt_sockdata is 0
		if ( (!pars->isactive) &&  ( (fp == NULL) || ( (fp != NULL) && (fp->rmt_sockdata == 0) ) ))
		{
			// Check for the initial timeout
			FD_ZERO(&rfds);
			// We do not have to block here
			tv.tv_sec = RPCAP_TIMEOUT_RUNTIME;
			tv.tv_usec = 0;
			
			FD_SET(pars->sockctrl, &rfds);

			retval = select(pars->sockctrl + 1, &rfds, NULL, NULL, &tv);
			if (retval == -1)
			{
				sock_geterror("select(): ", errbuf, PCAP_ERRBUF_SIZE);
				rpcap_senderror(pars->sockctrl, errbuf, PCAP_ERR_NETW, NULL);
				goto end;
			}

			// The timeout has expired
			// So, this was a fake connection. Drop it down
			if (retval == 0)
			{
				SOCK_ASSERT("The RPCAP runtime timeout has expired", 1);
				rpcap_senderror(pars->sockctrl, "The RPCAP runtime timeout has expired", PCAP_ERR_RUNTIMETIMEOUT, NULL);
				goto end;
			}
		}

		if (sock_recv(pars->sockctrl, (char *) &header, sizeof(struct rpcap_header), SOCK_RECEIVEALL_YES, errbuf, PCAP_ERRBUF_SIZE) == -1)
			goto end;

		// Checks if the message is correct
		// In case it is wrong, it discard the data
		retval= rpcap_checkmsg(errbuf, pars->sockctrl, &header,
			RPCAP_MSG_FINDALLIF_REQ,
			RPCAP_MSG_OPEN_REQ,
			RPCAP_MSG_STARTCAP_REQ,
			RPCAP_MSG_UPDATEFILTER_REQ,
			RPCAP_MSG_STATS_REQ,
			RPCAP_MSG_ENDCAP_REQ,
			RPCAP_MSG_SETSAMPLING_REQ,
			RPCAP_MSG_CLOSE,
			RPCAP_MSG_ERROR,
			0);

		switch (retval)
		{
			case -3:	// Unrecoverable network error
				goto end;	// Do nothing; just exit from findalldevs; the error code is already into the errbuf

			case -2:	// The other endpoint send a message that is not allowed here
			{
				rpcap_senderror(pars->sockctrl, "The RPCAP daemon received a message that is not valid", PCAP_ERR_WRONGMSG, errbuf);
			}
			case -1:	// The other endpoint has a version number that is not compatible with our
			{
				rpcap_senderror(pars->sockctrl, "RPCAP version number mismatch", PCAP_ERR_WRONGVER, errbuf);
			}
			break;

			case RPCAP_MSG_FINDALLIF_REQ:
			{
				// Checks that the header does not contain other data; if so, discard it
				if (ntohl(header.plen))
					sock_discard(pars->sockctrl, ntohl(header.plen), errbuf, PCAP_ERRBUF_SIZE);

				if (daemon_findalldevs(pars->sockctrl, errbuf) )
					SOCK_ASSERT(errbuf, 1);

				break;
			};

			case RPCAP_MSG_OPEN_REQ:
			{
				retval= daemon_opensource(pars->sockctrl, source, sizeof(source), ntohl(header.plen), errbuf);

				if (retval == -1)
					SOCK_ASSERT(errbuf, 1);

				break;
			};

			case RPCAP_MSG_SETSAMPLING_REQ:
			{
				retval= daemon_setsampling(pars->sockctrl, &samp_param, ntohl(header.plen), errbuf);

				if (retval == -1)
					SOCK_ASSERT(errbuf, 1);

				break;
			};

			case RPCAP_MSG_STARTCAP_REQ:
			{
				fp= daemon_startcapture(pars->sockctrl, &threaddata, source, pars->isactive, &samp_param, ntohl(header.plen), errbuf);

				if (fp == NULL)
					SOCK_ASSERT(errbuf, 1);

				break;
			};

			case RPCAP_MSG_UPDATEFILTER_REQ:
			{
				if (fp)
				{
					if (daemon_updatefilter(fp, ntohl(header.plen)) )
						SOCK_ASSERT(pcap_geterr(fp->fp), 1);
				}
				else
				{
					rpcap_senderror(pars->sockctrl, "Device not opened. Cannot update filter", PCAP_ERR_UPDATEFILTER, errbuf);
				}

				break;
			};

			case RPCAP_MSG_STATS_REQ:
			{
				// Checks that the header does not contain other data; if so, discard it
				if (ntohl(header.plen))
					sock_discard(pars->sockctrl, ntohl(header.plen), errbuf, PCAP_ERRBUF_SIZE);

				if (fp)
				{
					if (daemon_getstats(fp) )
						SOCK_ASSERT(pcap_geterr(fp->fp), 1);
				}
				else
				{
					SOCK_ASSERT("GetStats: this call should't be allowed here", 1);

					if (daemon_getstatsnopcap(pars->sockctrl, ifdrops, ifrecv, krnldrop, svrcapt, errbuf) )
						SOCK_ASSERT(errbuf, 1);
					// we have to keep compatibility with old applications, which ask for statistics
					// also when the capture has already stopped

//					rpcap_senderror(pars->sockctrl, "Device not opened. Cannot get statistics", PCAP_ERR_GETSTATS, errbuf);
				}

				break;
			};

			case RPCAP_MSG_ENDCAP_REQ:		// The other endpoint close the current capture session
			{
				if (fp)
				{
				struct pcap_stat stats;

					// Save statistics (we can need them in the future)
					if (pcap_stats(fp->fp, &stats) )
					{
						ifdrops= stats.ps_ifdrop;
						ifrecv= stats.ps_recv;
						krnldrop= stats.ps_drop;
						svrcapt= fp->TotCapt;
					}
					else
						ifdrops= ifrecv= krnldrop= svrcapt= 0;

					if ( daemon_endcapture(fp, &threaddata, errbuf) )
						SOCK_ASSERT(errbuf, 1);
					fp= NULL;
				}
				else
				{
					rpcap_senderror(pars->sockctrl, "Device not opened. Cannot close the capture", PCAP_ERR_ENDCAPTURE, errbuf);
				}
				break;
			};

			case RPCAP_MSG_CLOSE:		// The other endpoint close the pcap session
			{
				// signal to the main that the user closed the control connection
				// This is used only in case of active mode
				pars->activeclose= 1;	
				SOCK_ASSERT("The other end system asked to close the connection.", 1);
				goto end;
				break;
			};

			case RPCAP_MSG_ERROR:		// The other endpoint reported an error
			{
				// Do nothing; just exit; the error code is already into the errbuf
				SOCK_ASSERT(errbuf, 1);
				break;
			};

			default:
			{
				SOCK_ASSERT("Internal error.", 1);
				break;
			};
		}
	}

end:
	// The child thread is about to end

	// perform pcap_t cleanup, in case it has not been done
	if (fp)
	{
		if (threaddata)
		{
			pthread_cancel(threaddata);
			threaddata= 0;
		}
		if (fp->rmt_sockdata)
		{
			sock_close(fp->rmt_sockdata, NULL, 0);
			fp->rmt_sockdata= 0;
		}
		daemon_ctx_close(fp);
		fp= NULL;
	}

	// Print message and exit
	SOCK_ASSERT("I'm exiting from the child loop", 1);
	SOCK_ASSERT(errbuf, 1);

	if (!pars->isactive)
	{
		if (pars->sockctrl)
			sock_close(pars->sockctrl, NULL, 0);
		
		free(pars);
#ifdef WIN32
		pthread_exit(0);
#endif
	}
}


/*!
	\brief It checks if the authentication credentials supplied by the user are valid.

	This function is called each time the rpcap daemon starts a new serving thread.
	It reads the authentication message from the network and it checks that the 
	user information are valid.

	\param sockctrl: the socket if of the control connection.
	
	\param nullAuthAllowed: '1' if the NULL authentication is allowed.

	\param errbuf: a user-allocated buffer in which the error message (if one) has to be written.

	\return '0' if everything is fine, '-1' if an unrecoverable error occurred.
	The error message is returned in the 'errbuf' variable.
	'-2' is returned in case the authentication failed or in case of a recoverable error (like
	wrong version). In that case, 'errbuf' keeps the reason of the failure. This provides
	a way to know that the connection does not have to be closed.

	In case the message is a 'CLOSE' or an 'ERROR', it returns -3. The error can be due to a
	connection refusal in active mode, since this host cannot be allowed to connect to the remote
	peer.
*/
int daemon_checkauth(SOCKET sockctrl, int nullAuthAllowed, char *errbuf)
{
struct rpcap_header header;			// RPCAP message general header
int retval;							// generic return value
unsigned int nread;					// number of bytes of the payload read from the socket
struct rpcap_auth auth;				// RPCAP authentication header
char *string1, *string2;			// two strings exchanged by the authentication message
unsigned int plen;					// length of the payload
int retcode;						// the value we have to return to the caller

	if (sock_recv(sockctrl, (char *) &header, sizeof(struct rpcap_header), SOCK_RECEIVEALL_YES, errbuf, PCAP_ERRBUF_SIZE) == -1)
		return -1;

	plen= ntohl(header.plen);

	retval= rpcap_checkmsg(errbuf, sockctrl, &header,
		RPCAP_MSG_AUTH_REQ,
		RPCAP_MSG_CLOSE,
		0);

	if (retval != RPCAP_MSG_AUTH_REQ)
	{
		switch (retval)
		{
			case -3:	// Unrecoverable network error
				return -1;	// Do nothing; just exit; the error code is already into the errbuf

			case -2:	// The other endpoint send a message that is not allowed here
			case -1:	// The other endpoint has a version number that is not compatible with our
				return -2;

			case RPCAP_MSG_CLOSE:
			{
				// Check if all the data has been read; if not, discard the data in excess
				if (ntohl(header.plen) )
				{
					if (sock_discard(sockctrl, ntohl(header.plen), NULL, 0) )
					{
						retcode= -1;
						goto error;
					}
				}		
				return -3;
			};

			case RPCAP_MSG_ERROR:
				return -3;

			default:
			{
				SOCK_ASSERT("Internal error.", 1);
				retcode= -2;
				goto error;
			};
		}
	}

	// If it comes here, it means that we have an authentication request message
	if ( (nread= sock_recv(sockctrl, (char *) &auth, sizeof(struct rpcap_auth), SOCK_RECEIVEALL_YES, errbuf, PCAP_ERRBUF_SIZE)) == -1)
	{
		retcode= -1;
		goto error;
	}

	switch (ntohs(auth.type) )
	{
		case RPCAP_RMTAUTH_NULL:
		{
			if (!nullAuthAllowed)
			{
				snprintf(errbuf, PCAP_ERRBUF_SIZE, "Authentication failed; NULL autentication not permitted.");
				retcode= -2;
				goto error;
			}
			break;
		}

		case RPCAP_RMTAUTH_PWD:
		{
		int len1, len2;

			len1= ntohs(auth.slen1);
			len2= ntohs(auth.slen2);

			string1= (char *) malloc (len1 + 1);
			string2= (char *) malloc (len2 + 1);

			if ( (string1 == NULL) || (string2 == NULL) )
			{
				snprintf(errbuf, PCAP_ERRBUF_SIZE, "malloc() failed: %s", pcap_strerror(errno));
				retcode= -1;
				goto error;
			}

			if ( (nread+= sock_recv(sockctrl, string1, len1, SOCK_RECEIVEALL_YES, errbuf, PCAP_ERRBUF_SIZE)) == -1)
			{
				retcode= -1;
				goto error;
			}
			if ( (nread+= sock_recv(sockctrl, string2, len2, SOCK_RECEIVEALL_YES, errbuf, PCAP_ERRBUF_SIZE)) == -1)
			{
				retcode= -1;
				goto error;
			}

			string1[len1]= 0;
			string2[len2]= 0;

			if (daemon_AuthUserPwd(string1, string2, errbuf) )
			{
				retcode= -2;
				goto error;
			}

			break;
			}

		default:
			snprintf(errbuf, PCAP_ERRBUF_SIZE, "Authentication type not recognized.");
			retcode= -2;
			goto error;
	}


	// Check if all the data has been read; if not, discard the data in excess
	if (nread != plen)
	{
		if (sock_discard(sockctrl, plen - nread, NULL, 0) )
		{
			retcode= -1;
			goto error;
		}
	}

	rpcap_createhdr(&header, RPCAP_MSG_AUTH_REPLY, 0, 0);

	// Send the ok message back
	if ( sock_send(sockctrl, (char *) &header, sizeof (struct rpcap_header), errbuf, PCAP_ERRBUF_SIZE) == -1)
	{
		retcode= -1;
		goto error;
	}

	return 0;

error:
	// Check if all the data has been read; if not, discard the data in excess
	if (nread != plen)
		sock_discard(sockctrl, plen - nread, NULL, 0);

	return retcode;
}



int daemon_AuthUserPwd(char *username, char *password, char *errbuf)
{
#ifdef WIN32
	/*
		Warning: the user which launches the process must have the SE_TCB_NAME right.
		This corresponds to have the "Act as part of the Operating System" turined on
		(administrative tools, local security settings, local policies, user right assignment)
		However, it seems to me that if you run it as a service, this right should be
		provided by default.
	*/
	HANDLE Token;
	if (LogonUser(username, ".", password, LOGON32_LOGON_NETWORK, LOGON32_PROVIDER_DEFAULT, &Token) == 0)
	{
	int error;

		error = GetLastError();
		FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, error, 0, errbuf,
			PCAP_ERRBUF_SIZE, NULL);

		return -1;
	}

	// This call should change the current thread to the selected user.
	// I didn't test it.
	if (ImpersonateLoggedOnUser(Token) == 0)
	{
	int error;

		error = GetLastError();
		FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, error, 0, errbuf,
			PCAP_ERRBUF_SIZE, NULL);

		CloseHandle(Token);
		return -1;
	}

	CloseHandle(Token);
	return 0;

#else
/*	Standard user authentication:
		http://www.unixpapa.com/incnote/passwd.html
	Problem: it is not able to merge the standard pwd file with the shadow one

	Shadow user authentication:
		http://www.tldp.org/HOWTO/Shadow-Password-HOWTO-8.html
	Problem: the program must either (1) run as root, or (2) run as user, but it
	must be owned by root and must be SUID root (chmod u+s rpcapd)
*/

	struct passwd *user;
#ifdef linux
	struct spwd *usersp;
#endif

	// This call is needed to get the uid
	if ((user= getpwnam(username)) == NULL)
	{
		snprintf(errbuf, PCAP_ERRBUF_SIZE, "Authentication failed: no such user");
		return -1;
	}

#ifdef linux
	// This call is needed to get the password; otherwise 'x' is returned
	if ((usersp= getspnam(username)) == NULL)
	{
		snprintf(errbuf, PCAP_ERRBUF_SIZE, "Authentication failed: no such user");
		return -1;
	}
	
	if (strcmp(usersp->sp_pwdp, (char *) crypt(password, usersp->sp_pwdp) ) != 0)
	{
		snprintf(errbuf, PCAP_ERRBUF_SIZE, "Authentication failed: password incorrect");
		return -1;
	}
#endif

#ifdef bsd
	if (strcmp(user->pw_passwd, (char *) crypt(password, user->pw_passwd) ) != 0)
	{
		snprintf(errbuf, PCAP_ERRBUF_SIZE, "Authentication failed: password incorrect");
		return -1;
	}
#endif

	if (setuid(user->pw_uid) )
	{
		snprintf(errbuf, PCAP_ERRBUF_SIZE, "%s", pcap_strerror(errno) );
		return -1;
	}

/*	if (setgid(user->pw_gid) )
	{
		SOCK_ASSERT("setgid failed", 1);
		snprintf(errbuf, PCAP_ERRBUF_SIZE, "%s", pcap_strerror(errno) );
		return -1;
	}
*/
	return 0;

#endif

}



// PORTING WARNING We assume u_int is a 32bit value
int daemon_findalldevs(SOCKET sockctrl, char *errbuf)
{
char sendbuf[RPCAP_NETBUF_SIZE];			// temporary buffer in which data to be sent is buffered
int sendbufidx= 0;							// index which keeps the number of bytes currently buffered
pcap_if_t *alldevs;							// pointer to the heade of the interface chain
pcap_if_t *d;								// temp pointer neede to scan the interface chain
uint16 plen= 0;								// length of the payload of this message
struct pcap_addr *address;					// pcap structure that keeps a network address of an interface
struct rpcap_findalldevs_if *findalldevs_if;// rpcap structure that packet all the data of an interface together
uint16 nif= 0;								// counts the number of interface listed

	// Retrieve the device list
	if (pcap_findalldevs(&alldevs, errbuf) == -1)
	{
		rpcap_senderror(sockctrl, errbuf, PCAP_ERR_FINDALLIF, NULL);
		return -1;
	}

	if (alldevs == NULL)
	{
		rpcap_senderror(sockctrl,
			"No interfaces found! Make sure libpcap/WinPcap is properly installed"
			" and you have the right to access to the remote device.",
			PCAP_ERR_NOREMOTEIF, 
			errbuf);
		return -1;
	}

	// checks the number of interfaces and it computes the total length of the payload
	for (d= alldevs; d != NULL; d= d->next)
	{
		nif++;

		if (d->description)
			plen+= strlen(d->description);
		if (d->name)
			plen+= strlen(d->name);

		plen+= sizeof(struct rpcap_findalldevs_if);

		for (address= d->addresses; address != NULL; address= address->next)
			plen+= ( sizeof(struct sockaddr_storage) * 4);
	}

	// RPCAP findalldevs command
	if ( sock_bufferize(NULL, sizeof(struct rpcap_header), NULL, 
		&sendbufidx, RPCAP_NETBUF_SIZE, SOCKBUF_CHECKONLY, errbuf, PCAP_ERRBUF_SIZE) == -1)
		return -1;

	rpcap_createhdr( (struct rpcap_header *) sendbuf, RPCAP_MSG_FINDALLIF_REPLY, nif, plen);

	// send the interface list
	for (d= alldevs; d != NULL; d= d->next)
	{
	uint16 lname, ldescr;

		findalldevs_if= (struct rpcap_findalldevs_if *) &sendbuf[sendbufidx];

		if ( sock_bufferize(NULL, sizeof(struct rpcap_findalldevs_if), NULL,
			&sendbufidx, RPCAP_NETBUF_SIZE, SOCKBUF_CHECKONLY, errbuf, PCAP_ERRBUF_SIZE) == -1)
			return -1;

		memset(findalldevs_if, 0, sizeof(struct rpcap_findalldevs_if) );

		if (d->description) ldescr= (short) strlen(d->description);
		else ldescr= 0;
		if (d->name) lname= (short) strlen(d->name);
		else lname= 0;

		findalldevs_if->desclen= htons(ldescr);
		findalldevs_if->namelen= htons(lname);
		findalldevs_if->flags= htonl(d->flags);

		for (address= d->addresses; address != NULL; address= address->next)
			findalldevs_if->naddr++;

		findalldevs_if->naddr= htons(findalldevs_if->naddr);

		if (sock_bufferize(d->name, lname, sendbuf, &sendbufidx, 
			RPCAP_NETBUF_SIZE, SOCKBUF_BUFFERIZE, errbuf, PCAP_ERRBUF_SIZE) == -1)
			return -1;

		if (sock_bufferize(d->description, ldescr, sendbuf, &sendbufidx,
			RPCAP_NETBUF_SIZE, SOCKBUF_BUFFERIZE, errbuf, PCAP_ERRBUF_SIZE) == -1)
			return -1;

		// send all addresses
		for (address= d->addresses; address != NULL; address= address->next)
		{
		struct sockaddr_storage *sockaddr;

			sockaddr= (struct sockaddr_storage *) &sendbuf[sendbufidx];
			if (sock_bufferize(NULL, sizeof(struct sockaddr_storage), NULL, 
				&sendbufidx, RPCAP_NETBUF_SIZE, SOCKBUF_CHECKONLY, errbuf, PCAP_ERRBUF_SIZE) == -1)
				return -1;
			daemon_seraddr( (struct sockaddr_storage *) address->addr, sockaddr);

			sockaddr= (struct sockaddr_storage *) &sendbuf[sendbufidx];
			if (sock_bufferize(NULL, sizeof(struct sockaddr_storage), NULL, 
				&sendbufidx, RPCAP_NETBUF_SIZE, SOCKBUF_CHECKONLY, errbuf, PCAP_ERRBUF_SIZE) == -1)
				return -1;
			daemon_seraddr( (struct sockaddr_storage *) address->netmask, sockaddr);

			sockaddr= (struct sockaddr_storage *) &sendbuf[sendbufidx];
			if (sock_bufferize(NULL, sizeof(struct sockaddr_storage), NULL,
				&sendbufidx, RPCAP_NETBUF_SIZE, SOCKBUF_CHECKONLY, errbuf, PCAP_ERRBUF_SIZE) == -1)
				return -1;
			daemon_seraddr( (struct sockaddr_storage *) address->broadaddr, sockaddr);

			sockaddr= (struct sockaddr_storage *) &sendbuf[sendbufidx];
			if (sock_bufferize(NULL, sizeof(struct sockaddr_storage), NULL,
				&sendbufidx, RPCAP_NETBUF_SIZE, SOCKBUF_CHECKONLY, errbuf, PCAP_ERRBUF_SIZE) == -1)
				return -1;
			daemon_seraddr( (struct sockaddr_storage *) address->dstaddr, sockaddr);
		}
	}

	// Send a final command that says "now send it!"
	if (sock_send(sockctrl, sendbuf, sendbufidx, errbuf, PCAP_ERRBUF_SIZE) == -1)
		return -1;

	// We do no longer need the device list. Free it
	pcap_freealldevs(alldevs);

	// everything is fine
	return 0;
}





/*
	\param plen: the length of the current message (needed in order to be able
	to discard excess data in the message, if present)
*/
int daemon_opensource(SOCKET sockctrl, char *source, int srclen, uint32 plen, char *errbuf)
{
pcap_t *fp= NULL;					// pcap_t main variable
unsigned int nread;					// number of bytes of the payload read from the socket
char sendbuf[RPCAP_NETBUF_SIZE];	// temporary buffer in which data to be sent is buffered
int sendbufidx= 0;					// index which keeps the number of bytes currently buffered
struct rpcap_openreply *openreply;	// open reply message


	strcpy(source, PCAP_SRC_IF_STRING);

	if (srclen <= (int) (strlen(PCAP_SRC_IF_STRING) + plen) )
	{
		rpcap_senderror(sockctrl, "Source string too long", PCAP_ERR_OPEN, NULL);
		return -1;
	}

	if ( (nread= sock_recv(sockctrl, &source[strlen(PCAP_SRC_IF_STRING)], plen, SOCK_RECEIVEALL_YES, errbuf, PCAP_ERRBUF_SIZE)) == -1)
		return -1;

	// Check if all the data has been read; if not, discard the data in excess
	if (nread != plen)
		sock_discard(sockctrl, plen - nread, NULL, 0);

	// Puts a '0' to terminate the source string
	source[strlen(PCAP_SRC_IF_STRING) + plen]= 0;

	printf("Opening '%s'\n", source);

	// Open the selected device
	// This is a fake open, since we do that only to get the needed parameters, then we close the device again
	if ( (fp= pcap_open(source, 
			1500 /* fake snaplen */,
			0 /* no promis */, 
			1000 /* fake timeout */,
			NULL /* local device, so no auth */,
			errbuf)) == NULL)
	{
		rpcap_senderror(sockctrl, errbuf, PCAP_ERR_OPEN, NULL);
		return -1;
	}


	// Now, I can send a RPCAP open reply message
	if ( sock_bufferize(NULL, sizeof(struct rpcap_header), NULL, &sendbufidx,
		RPCAP_NETBUF_SIZE, SOCKBUF_CHECKONLY, errbuf, PCAP_ERRBUF_SIZE) == -1)
		goto error;

	rpcap_createhdr( (struct rpcap_header *) sendbuf, RPCAP_MSG_OPEN_REPLY, 0, sizeof(struct rpcap_openreply) );

	openreply= (struct rpcap_openreply *) &sendbuf[sendbufidx];
	
	if ( sock_bufferize(NULL, sizeof(struct rpcap_openreply), NULL, &sendbufidx, 
		RPCAP_NETBUF_SIZE, SOCKBUF_CHECKONLY, errbuf, PCAP_ERRBUF_SIZE) == -1)
		goto error;

	memset(openreply, 0, sizeof(struct rpcap_openreply) );
	openreply->linktype= htonl(pcap_datalink(fp));
	/* XXX: openreply->tzoff= htonl(fp->tzoff); */

	if ( sock_send(sockctrl, sendbuf, sendbufidx, errbuf, PCAP_ERRBUF_SIZE) == -1)
		goto error;

	// I have to close the device again, since it has been opened with wrong parameters
	pcap_close(fp);
	fp= NULL;

	return 0;

error:
	if (fp)
	{
		pcap_close(fp);
		fp= NULL;
	}

	return -1;
}





/*
	\param plen: the length of the current message (needed in order to be able
	to discard excess data in the message, if present)
*/
struct daemon_ctx *daemon_startcapture(SOCKET sockctrl, pthread_t *threaddata, char *source, int active, struct rpcap_sampling *samp_param, uint32 plen, char *errbuf)
{
char portdata[PCAP_BUF_SIZE];		// temp variable needed to derive the data port
char peerhost[PCAP_BUF_SIZE];		// temp variable needed to derive the host name of our peer
struct daemon_ctx *fp= NULL;					// pcap_t main variable
unsigned int nread;					// number of bytes of the payload read from the socket
char sendbuf[RPCAP_NETBUF_SIZE];	// temporary buffer in which data to be sent is buffered
int sendbufidx= 0;					// index which keeps the number of bytes currently buffered

// socket-related variables
SOCKET sockdata= 0;					// socket descriptor of the data connection
struct addrinfo hints;				// temp, needed to open a socket connection
struct addrinfo *addrinfo;			// temp, needed to open a socket connection
struct sockaddr_storage saddr;		// temp, needed to retrieve the network data port chosen on the local machine
socklen_t saddrlen;					// temp, needed to retrieve the network data port chosen on the local machine

pthread_attr_t detachedAttribute;	// temp, needed to set the created thread as detached

// RPCAP-related variables
struct rpcap_startcapreq startcapreq;		// start capture request message
struct rpcap_startcapreply *startcapreply;	// start capture reply message
int serveropen_dp;							// keeps who is going to open the data connection

	addrinfo= NULL;

	if ( (nread= sock_recv(sockctrl, (char *) &startcapreq, sizeof(struct rpcap_startcapreq), SOCK_RECEIVEALL_YES, errbuf, PCAP_ERRBUF_SIZE)) == -1)
		return NULL;

	startcapreq.flags= ntohs(startcapreq.flags);

	// Open the selected device
	if ( (fp= daemon_ctx_open(source,
			ntohl(startcapreq.snaplen),
			(startcapreq.flags & RPCAP_STARTCAPREQ_FLAG_PROMISC) ? PCAP_OPENFLAG_PROMISCUOUS : 0 /* local device, other flags not needed */, 
			ntohl(startcapreq.read_timeout),
			NULL /* local device, so no auth */,
			errbuf)) == NULL)
	{
		rpcap_senderror(sockctrl, errbuf, PCAP_ERR_OPEN, NULL);
		return NULL;
	}

	// Apply sampling parameters
	fp->rmt_samp.method= samp_param->method;
	fp->rmt_samp.value= samp_param->value;

	/*
	We're in active mode if:
	- we're using TCP, and the user wants us to be in active mode
	- we're using UDP
	*/
	serveropen_dp= (startcapreq.flags & RPCAP_STARTCAPREQ_FLAG_SERVEROPEN) || (startcapreq.flags & RPCAP_STARTCAPREQ_FLAG_DGRAM) || active;

	/*
	Gets the sockaddr structure referred to the other peer in the ctrl connection

	We need that because:
	- if we're in passive mode, we need to know the address family we want to use 
	(the same used for the ctrl socket)
	- if we're in active mode, we need to know the network address of the other host 
	we want to connect to
	*/
	saddrlen = sizeof(struct sockaddr_storage);
	if (getpeername(sockctrl, (struct sockaddr *) &saddr, &saddrlen) == -1)
	{
		sock_geterror("getpeername(): ", errbuf, PCAP_ERRBUF_SIZE);
		goto error;
	}

	memset(&hints, 0, sizeof(struct addrinfo) );
	hints.ai_socktype = (startcapreq.flags & RPCAP_STARTCAPREQ_FLAG_DGRAM) ? SOCK_DGRAM : SOCK_STREAM;
	hints.ai_family = saddr.ss_family;

	// Now we have to create a new socket to send packets
	if (serveropen_dp)		// Data connection is opened by the server toward the client
	{
		sprintf(portdata, "%d", ntohs(startcapreq.portdata) );

		// Get the name of the other peer (needed to connect to that specific network address)
		if (getnameinfo( (struct sockaddr *) &saddr, saddrlen, peerhost, 
				sizeof(peerhost), NULL, 0, NI_NUMERICHOST) )
		{
			sock_geterror("getnameinfo(): ", errbuf, PCAP_ERRBUF_SIZE);
			goto error;
		}
		printf("Connecting to udp %s:%s\n", peerhost, portdata);

		if (sock_initaddress(peerhost, portdata, &hints, &addrinfo, errbuf, PCAP_ERRBUF_SIZE) == -1)
			goto error;

		if ( (sockdata= sock_open(addrinfo, SOCKOPEN_CLIENT, 0, errbuf, PCAP_ERRBUF_SIZE)) == -1)
			goto error;
	}
	else		// Data connection is opened by the client toward the server
	{
		hints.ai_flags = AI_PASSIVE;

		// Let's the server socket pick up a free network port for us
		if (sock_initaddress(NULL, "0", &hints, &addrinfo, errbuf, PCAP_ERRBUF_SIZE) == -1)
			goto error;

		if ( (sockdata= sock_open(addrinfo, SOCKOPEN_SERVER, 1 /* max 1 connection in queue */, errbuf, PCAP_ERRBUF_SIZE)) == -1)
			goto error;

		// get the complete sockaddr structure used in the data connection
		saddrlen = sizeof(struct sockaddr_storage);
		if (getsockname(sockdata, (struct sockaddr *) &saddr, &saddrlen) == -1)
		{
			sock_geterror("getsockname(): ", errbuf, PCAP_ERRBUF_SIZE);
			goto error;
		}

		// Get the local port the system picked up
		if (getnameinfo( (struct sockaddr *) &saddr, saddrlen, NULL, 
				0, portdata, sizeof(portdata), NI_NUMERICSERV) )
		{
			sock_geterror("getnameinfo(): ", errbuf, PCAP_ERRBUF_SIZE);
			goto error;
		}
	}

	// addrinfo is no longer used
	freeaddrinfo(addrinfo);
	addrinfo= NULL;

	// save the socket ID for the next calls
	fp->rmt_sockctrl= sockctrl;	// Needed to send an error on the ctrl connection

	// Now I can set the filter
	if ( daemon_unpackapplyfilter(fp, &nread, &plen, errbuf) )
		goto error;


	// Now, I can send a RPCAP start capture reply message
	if ( sock_bufferize(NULL, sizeof(struct rpcap_header), NULL, &sendbufidx,
		RPCAP_NETBUF_SIZE, SOCKBUF_CHECKONLY, errbuf, PCAP_ERRBUF_SIZE) == -1)
		goto error;

	rpcap_createhdr( (struct rpcap_header *) sendbuf, RPCAP_MSG_STARTCAP_REPLY, 0, sizeof(struct rpcap_startcapreply) );

	startcapreply= (struct rpcap_startcapreply *) &sendbuf[sendbufidx];
	
	if ( sock_bufferize(NULL, sizeof(struct rpcap_startcapreply), NULL,
		&sendbufidx, RPCAP_NETBUF_SIZE, SOCKBUF_CHECKONLY, errbuf, PCAP_ERRBUF_SIZE) == -1)
		goto error;

	memset(startcapreply, 0, sizeof(struct rpcap_startcapreply) );
	/* XXX: startcapreply->bufsize= htonl(fp->bufsize); */

	if (!serveropen_dp)
	{
		unsigned short port = (unsigned short)strtoul(portdata,NULL,10);
		startcapreply->portdata= htons(port);
	}

	if ( sock_send(sockctrl, sendbuf, sendbufidx, errbuf, PCAP_ERRBUF_SIZE) == -1)
		goto error;

	if (!serveropen_dp)
	{
	SOCKET socktemp;	// We need another socket, since we're going to accept() a connection

		// Connection creation
		saddrlen = sizeof(struct sockaddr_storage);

		socktemp= accept(sockdata, (struct sockaddr *) &saddr, &saddrlen);
		
		if (socktemp == -1)
		{
			sock_geterror("accept(): ", errbuf, PCAP_ERRBUF_SIZE);
			goto error;
		}

		// Now that I accepted the connection, the server socket is no longer needed
		sock_close(sockdata, errbuf, PCAP_ERRBUF_SIZE);
		sockdata= socktemp;
	}

	fp->rmt_sockdata= sockdata;

	/* GV we need this to create the thread as detached. */
	/* GV otherwise, the thread handle is not destroyed  */
	pthread_attr_init(&detachedAttribute); 
	pthread_attr_setdetachstate(&detachedAttribute, PTHREAD_CREATE_DETACHED);
	
	// Now we have to create a new thread to receive packets
	printf("Starting pcap_dispatch thread\n");
	if ( pthread_create(threaddata, &detachedAttribute, (void *) daemon_thrdatamain, (void *) fp) )
	{
		snprintf(errbuf, PCAP_ERRBUF_SIZE, "Error creating the data thread");
		pthread_attr_destroy(&detachedAttribute);
		goto error;
	}

	pthread_attr_destroy(&detachedAttribute);
	// Check if all the data has been read; if not, discard the data in excess
	if (nread != plen)
		sock_discard(sockctrl, plen - nread, NULL, 0);

	return fp;

error:
	rpcap_senderror(sockctrl, errbuf, PCAP_ERR_STARTCAPTURE, NULL);

	if (addrinfo)
		freeaddrinfo(addrinfo);

	if (threaddata)
		pthread_cancel(*threaddata);

	if (sockdata)
		sock_close(sockdata, NULL, 0);

	// Check if all the data has been read; if not, discard the data in excess
	if (nread != plen)
		sock_discard(sockctrl, plen - nread, NULL, 0);

	if (fp)
	{
		daemon_ctx_close(fp);
		fp= NULL;
	}

	return NULL;
}



int daemon_endcapture(struct daemon_ctx *fp, pthread_t *threaddata, char *errbuf)
{
struct rpcap_header header;
SOCKET sockctrl;

	if (threaddata)
	{
		pthread_cancel(*threaddata);
		threaddata= 0;
	}
	if (fp->rmt_sockdata)
	{
		sock_close(fp->rmt_sockdata, NULL, 0);
		fp->rmt_sockdata= 0;
	}

	sockctrl= fp->rmt_sockctrl;

	daemon_ctx_close(fp);
	fp= NULL;

	rpcap_createhdr( &header, RPCAP_MSG_ENDCAP_REPLY, 0, 0);

	if ( sock_send(sockctrl, (char *) &header, sizeof(struct rpcap_header), errbuf, PCAP_ERRBUF_SIZE) == -1)
		return -1;
	
	return 0;
}



int daemon_unpackapplyfilter(struct daemon_ctx *fp, unsigned int *nread, int *plen, char *errbuf)
{
struct rpcap_filter filter;
struct rpcap_filterbpf_insn insn;
struct bpf_insn *bf_insn;
struct bpf_program bf_prog;
unsigned int i;


	if ( ( *nread+= sock_recv(fp->rmt_sockctrl, (char *) &filter, sizeof(struct rpcap_filter), SOCK_RECEIVEALL_YES, errbuf, PCAP_ERRBUF_SIZE)) == -1)
	{
		// to avoid blocking on the sock_discard()
		*plen= *nread;
		return -1;
	}

	bf_prog.bf_len= ntohl(filter.nitems);

	if (ntohs(filter.filtertype) != RPCAP_UPDATEFILTER_BPF)
	{
		snprintf(errbuf, PCAP_ERRBUF_SIZE, "Only BPF/NPF filters are currently supported");
		return -1;
	}

	bf_insn= (struct bpf_insn *) malloc ( sizeof(struct bpf_insn) * bf_prog.bf_len);
	if (bf_insn == NULL)
	{
		snprintf(errbuf, PCAP_ERRBUF_SIZE, "malloc() failed: %s", pcap_strerror(errno));
		return -1;
	}

	bf_prog.bf_insns= bf_insn;

	for (i= 0; i < bf_prog.bf_len; i++)
	{
		if ( ( *nread+= sock_recv(fp->rmt_sockctrl, (char *) &insn, 
			sizeof(struct rpcap_filterbpf_insn), SOCK_RECEIVEALL_YES, errbuf, PCAP_ERRBUF_SIZE)) == -1)
			return -1;

		bf_insn->code= ntohs(insn.code);
		bf_insn->jf= insn.jf;
		bf_insn->jt= insn.jt;
		bf_insn->k= ntohl(insn.k);

		bf_insn++;
	}

	if (bpf_validate(bf_prog.bf_insns, bf_prog.bf_len) == 0)
	{
		snprintf(errbuf, PCAP_ERRBUF_SIZE, "The filter contains bogus instructions");
		return -1;
	}

	if (pcap_setfilter(fp->fp, &bf_prog) )
	{
		snprintf(errbuf, PCAP_ERRBUF_SIZE, "RPCAP error: %s", pcap_geterr(fp->fp));
		return -1;
    }

	return 0;
}



int daemon_updatefilter(struct daemon_ctx *fp, uint32 plen)
{
struct rpcap_header header;			// keeps the answer to the updatefilter command
unsigned int nread;

	nread= 0;

	if ( daemon_unpackapplyfilter(fp, &nread, &plen, pcap_geterr(fp->fp)) )
		goto error;

	// Check if all the data has been read; if not, discard the data in excess
	if (nread != plen)
	{
		if (sock_discard(fp->rmt_sockctrl, plen - nread, NULL, 0) )
		{
			nread= plen;		// just to avoid to call discard again in the 'error' section
			goto error;
		}
	}

	// A response is needed, otherwise the other host does not know that everything went well
	rpcap_createhdr( &header, RPCAP_MSG_UPDATEFILTER_REPLY, 0, 0);

	if ( sock_send(fp->rmt_sockctrl, (char *) &header, sizeof (struct rpcap_header), pcap_geterr(fp->fp), PCAP_ERRBUF_SIZE) )
		goto error;

	return 0;


error:
	if (nread != plen)
		sock_discard(fp->rmt_sockctrl, plen - nread, NULL, 0);

	rpcap_senderror(fp->rmt_sockctrl, pcap_geterr(fp->fp), PCAP_ERR_UPDATEFILTER, NULL);

	return -1;
}



/*!
	\brief Received the sampling parameters from remote host and it stores in the pcap_t structure.
*/
int daemon_setsampling(SOCKET sockctrl, struct rpcap_sampling *samp_param, int plen, char *errbuf)
{
struct rpcap_header header;
struct rpcap_sampling rpcap_samp;
int nread;					// number of bytes of the payload read from the socket


	if ( ( nread= sock_recv(sockctrl, (char *) &rpcap_samp, sizeof(struct rpcap_sampling), 
			SOCK_RECEIVEALL_YES, errbuf, PCAP_ERRBUF_SIZE)) == -1)
		goto error;


	// Save these settings in the pcap_t 
	samp_param->method= rpcap_samp.method;
	samp_param->value= ntohl(rpcap_samp.value);


	// A response is needed, otherwise the other host does not know that everything went well
	rpcap_createhdr( &header, RPCAP_MSG_SETSAMPLING_REPLY, 0, 0);

	if ( sock_send(sockctrl, (char *) &header, sizeof (struct rpcap_header), errbuf, PCAP_ERRBUF_SIZE) )
		goto error;

	if (nread != plen)
		sock_discard(sockctrl, plen - nread, NULL, 0);

	return 0;

error:
	if (nread != plen)
		sock_discard(sockctrl, plen - nread, NULL, 0);

	rpcap_senderror(sockctrl, errbuf, PCAP_ERR_SETSAMPLING, NULL);

	return -1;
}



int daemon_getstats(struct daemon_ctx *fp)
{
char sendbuf[RPCAP_NETBUF_SIZE];	// temporary buffer in which data to be sent is buffered
int sendbufidx= 0;					// index which keeps the number of bytes currently buffered
struct pcap_stat stats;				// local statistics
struct rpcap_stats *netstats;		// statistics sent on the network

	if ( sock_bufferize(NULL, sizeof(struct rpcap_header), NULL, 
		&sendbufidx, RPCAP_NETBUF_SIZE, SOCKBUF_CHECKONLY, pcap_geterr(fp->fp), PCAP_ERRBUF_SIZE) == -1)
		goto error;

	rpcap_createhdr( (struct rpcap_header *) sendbuf, RPCAP_MSG_STATS_REPLY, 0, (uint16) sizeof(struct rpcap_stats));

	netstats= (struct rpcap_stats *) &sendbuf[sendbufidx];

	if ( sock_bufferize(NULL, sizeof(struct rpcap_stats), NULL,
		&sendbufidx, RPCAP_NETBUF_SIZE, SOCKBUF_CHECKONLY, pcap_geterr(fp->fp), PCAP_ERRBUF_SIZE) == -1)
		goto error;

	if (pcap_stats(fp->fp, &stats) )
		goto error;

	netstats->ifdrop= htonl(stats.ps_ifdrop);
	netstats->ifrecv= htonl(stats.ps_recv);
	netstats->krnldrop= htonl(stats.ps_drop);
	netstats->svrcapt= htonl(fp->svrcapt);

	printf("ifdrop=%u ifrecv=%u krnldrop=%u TotCapt=%u\n",
	       stats.ps_ifdrop, stats.ps_recv, stats.ps_drop, fp->TotCapt);
	printf("    sendring_full=%u (%u sleep) sendring_buf_full=%u (%u sleep)\n",
	       fp->sendring_full, fp->sendring_full_sleep,
	       fp->sendring_buf_full, fp->sendring_buf_full_sleep);
	printf("svrcapt=%u eagain=%u (%u sleep) sleep=%u read_timeouts=%u\n",
	       fp->svrcapt, fp->eagain_pkts, fp->eagain_sleep, fp->sendthr_sleep,
           fp->read_timeouts);

	// Send the packet
	if ( sock_send(fp->rmt_sockctrl, sendbuf, sendbufidx, pcap_geterr(fp->fp), PCAP_ERRBUF_SIZE) == -1)
		goto error;


	return 0;

error:
	rpcap_senderror(fp->rmt_sockctrl, pcap_geterr(fp->fp), PCAP_ERR_GETSTATS, NULL);
	return -1;
}




int daemon_getstatsnopcap(SOCKET sockctrl, unsigned int ifdrops, unsigned int ifrecv, 
						  unsigned int krnldrop, unsigned int svrcapt, char *errbuf)
{
char sendbuf[RPCAP_NETBUF_SIZE];	// temporary buffer in which data to be sent is buffered
int sendbufidx= 0;					// index which keeps the number of bytes currently buffered
struct rpcap_stats *netstats;		// statistics sent on the network

	if ( sock_bufferize(NULL, sizeof(struct rpcap_header), NULL,
		&sendbufidx, RPCAP_NETBUF_SIZE, SOCKBUF_CHECKONLY, errbuf, PCAP_ERRBUF_SIZE) == -1)
		goto error;

	rpcap_createhdr( (struct rpcap_header *) sendbuf, RPCAP_MSG_STATS_REPLY, 0, (uint16) sizeof(struct rpcap_stats));

	netstats= (struct rpcap_stats *) &sendbuf[sendbufidx];

	if ( sock_bufferize(NULL, sizeof(struct rpcap_stats), NULL,
		&sendbufidx, RPCAP_NETBUF_SIZE, SOCKBUF_CHECKONLY, errbuf, PCAP_ERRBUF_SIZE) == -1)
		goto error;

	netstats->ifdrop= htonl(ifdrops);
	netstats->ifrecv= htonl(ifrecv);
	netstats->krnldrop= htonl(krnldrop);
	netstats->svrcapt= htonl(svrcapt);

	// Send the packet
	if ( sock_send(sockctrl, sendbuf, sendbufidx, errbuf, PCAP_ERRBUF_SIZE) == -1)
		goto error;

	return 0;

error:
	rpcap_senderror(sockctrl, errbuf, PCAP_ERR_GETSTATS, NULL);
	return -1;
}

#define RPCAP_NETBUF_MAX_SIZE   65536

#define rmb()   asm volatile("lfence":::"memory")
#define wmb()   asm volatile("sfence":::"memory")

static volatile int daemon_sendthread_done;

static char *daemon_ringbuf;
static unsigned int daemon_ringbuf_len;

struct daemon_pkt_entry {
    int buf_idx;
    int len;
};

static struct daemon_pkt_entry *daemon_ring;
static unsigned int daemon_ring_mask;

struct daemon_ring_ctx {
    char _padding1[64];

    volatile unsigned int head;
    volatile unsigned int head_nbytes;
    unsigned int head_bufidx;
    char _padding2[52]; /* prevents false sharing over cache lines */

    volatile unsigned int tail;
    volatile unsigned int tail_nbytes;
    unsigned int tail_bufidx;
    int tail_signal;
    char _padding3[48];

    pthread_mutex_t send_lock;
    pthread_cond_t send_cv;
};
static struct daemon_ring_ctx daemon_ring_ctx;

static void
daemon_pkt_cb(u_char *usr, const struct pcap_pkthdr *pkt_header, const u_char *pkt_data)
{
    static int largest_caplen;
    struct daemon_ring_ctx *rctx = &daemon_ring_ctx;
    struct rpcap_pkthdr *net_pkt_header;
    struct daemon_ctx *fp = (struct daemon_ctx *)usr;
    struct daemon_pkt_entry *entry;
    unsigned int len;
    unsigned int new_tail;
    unsigned int tail_bufidx;
    unsigned int nbytes;
    int sleep_budget = 100;
    unsigned int caplen = pkt_header->caplen;

 again:
    tail_bufidx = rctx->tail_bufidx;
    len = sizeof(struct rpcap_header) + sizeof(struct rpcap_pkthdr) + caplen;
    nbytes = len;
    if (tail_bufidx + len > daemon_ringbuf_len) {
        /* a pkt can't wrap the end of the buffer, so advance to zero */
        printf("tail wrap, tail_bufidx=%u len=%u\n", tail_bufidx, len);
        nbytes += daemon_ringbuf_len - tail_bufidx;
        tail_bufidx = 0;
    }
    if (rctx->tail_nbytes + nbytes - rctx->head_nbytes > daemon_ringbuf_len) {
        pthread_cond_signal(&rctx->send_cv);
        if (--sleep_budget == 0) {
            fp->sendring_buf_full++;
            return;
        }
        fp->sendring_buf_full_sleep++;
        usleep(1 * 1000);
        goto again;
    }
    new_tail = (rctx->tail + 1) & daemon_ring_mask;
    if (new_tail == rctx->head) {
        pthread_cond_signal(&rctx->send_cv);
        if (--sleep_budget == 0) {
            fp->sendring_full++;
            return;
        }
        fp->sendring_full_sleep++;
        usleep(1 * 1000);
        goto again;
    }
    entry = &daemon_ring[rctx->tail];
    entry->buf_idx = tail_bufidx;
    entry->len = len;

    rpcap_createhdr( (struct rpcap_header *) &daemon_ringbuf[tail_bufidx],
                    RPCAP_MSG_PACKET, 0,
                    (uint16)(sizeof(struct rpcap_pkthdr) + caplen));
    tail_bufidx += sizeof(struct rpcap_header);

    net_pkt_header= (struct rpcap_pkthdr *) &daemon_ringbuf[tail_bufidx];
    tail_bufidx += sizeof(struct rpcap_pkthdr);


    net_pkt_header->caplen= htonl(caplen);
    net_pkt_header->len= htonl(pkt_header->len);
    net_pkt_header->npkt= htonl( ++(fp->TotCapt) );
    net_pkt_header->timestamp_sec= htonl(pkt_header->ts.tv_sec);
    net_pkt_header->timestamp_usec= htonl(pkt_header->ts.tv_usec);

    if (caplen > largest_caplen) {
        largest_caplen = caplen;
        printf("largest_caplen=%d\n", largest_caplen);
    }

    memcpy(&daemon_ringbuf[tail_bufidx], pkt_data, caplen);
    tail_bufidx += caplen;

    rctx->tail_nbytes += nbytes;
    rctx->tail_bufidx = tail_bufidx;
    wmb();
    rctx->tail = new_tail;

    if (--rctx->tail_signal == 0) {
        rctx->tail_signal = 16384;
        pthread_cond_signal(&rctx->send_cv);
    }
    return;
}

static void daemon_sendentry(struct daemon_ctx *fp, char *errbuf,
                             unsigned int bufidx, unsigned int len)
{
    char *buf = &daemon_ringbuf[bufidx];
    int sleep_budget = 10;
 again:
    if ( sock_send(fp->rmt_sockdata, buf, len, errbuf, PCAP_ERRBUF_SIZE) == -1) {
        if (errno == EAGAIN) {
            if (--sleep_budget == 0) {
                fp->eagain_pkts++;
            }
            else {
                fp->eagain_sleep++;
                usleep(1 * 1000);
                goto again;
            }
        }
        else {
            /* XXX: fp->cb_rc = -1; */
        }
    }
    else {
        fp->svrcapt++;
    }
}

void *daemon_sendthread_fn(void *ptr)
{
char errbuf[PCAP_ERRBUF_SIZE + 1];  // error buffer
struct daemon_ctx *fp = ptr;
struct daemon_ring_ctx *rctx = &daemon_ring_ctx;
struct daemon_pkt_entry *entry;
unsigned int idx, endidx;
unsigned int head_bufidx;
unsigned int nbytes;

    // Initialize errbuf
    memset(errbuf, 0, sizeof(errbuf) );

    while (!daemon_sendthread_done) {
        nbytes = 0;
        idx = rctx->head;
        head_bufidx = rctx->head_bufidx;
        endidx = rctx->tail;
        rmb();
        while ((nbytes < 131072) && (idx != endidx)) {
            entry = &daemon_ring[idx];
            if (entry->buf_idx < 0) {
                SOCK_ASSERT("bad ring entry->buf_idx < 0", 1);
                exit(1);
            }
            if (entry->buf_idx != head_bufidx) {
                printf("head wrap entry->buf_idx=%d head_bufidx=%u\n",
                       entry->buf_idx, head_bufidx);
                /* a pkt can't wrap the end of the buffer, so advance to zero */
                if (entry->buf_idx != 0) {
                    SOCK_ASSERT("bad ring entry->buf_idx != 0", 1);
                    exit(1);
                }
                nbytes += daemon_ringbuf_len - head_bufidx;
            }
            if (entry->len < (sizeof(struct rpcap_header) +
                              sizeof(struct rpcap_pkthdr))) {
                SOCK_ASSERT("bad ring entry->len", 1);
                exit(1);
            }
            nbytes += entry->len;
            daemon_sendentry(fp, errbuf, entry->buf_idx, entry->len);
            head_bufidx = entry->buf_idx + entry->len;
            idx = (idx + 1) & daemon_ring_mask;
        }
        rctx->head = idx;
        rctx->head_bufidx = head_bufidx;
        rctx->head_nbytes += nbytes;

        if (idx == rctx->tail) {
            pthread_mutex_lock(&rctx->send_lock);
            if (idx == rctx->tail) {
                struct timeval now;
                struct timespec timeout;
                int rc = gettimeofday(&now, NULL);
                if (rc == 0) {
                    timeout.tv_sec = now.tv_sec + 1;
                    timeout.tv_nsec = now.tv_usec * 1000;
                    pthread_cond_timedwait(&rctx->send_cv, &rctx->send_lock,
                                           &timeout);
                    fp->sendthr_sleep++;
                }
            }
            pthread_mutex_unlock(&rctx->send_lock);
        }
    }
}

void *daemon_thrdatamain(void *ptr)
{
char errbuf[PCAP_ERRBUF_SIZE + 1];	// error buffer
struct daemon_ctx *fp;							// pointer to a 'pcap' structure
int retval;							// general variable used to keep the return value of other functions
struct rpcap_pkthdr *net_pkt_header;// header of the packet
struct pcap_pkthdr *pkt_header;		// pointer to the buffer that contains the header of the current packet
u_char *pkt_data;					// pointer to the buffer that contains the current packet
char *sendbuf;						// temporary buffer in which data to be sent is buffered
int sendbufidx;						// index which keeps the number of bytes currently buffered
int largest_retval = 0;
pthread_t sendthread;
int sendthread_started = 0;


	fp= (struct daemon_ctx *) ptr;


	fp->TotCapt= 0;			// counter which is incremented each time a packet is received
	fp->sendring_full = 0;
	fp->sendring_buf_full = 0;

	// Initialize errbuf
	memset(errbuf, 0, sizeof(errbuf) );
	fp->errbuf = errbuf;

	memset(&daemon_ring_ctx, 0, sizeof(struct daemon_ring_ctx));
	daemon_ring_ctx.tail_signal = 16384;
	pthread_mutex_init(&daemon_ring_ctx.send_lock, NULL);
	pthread_cond_init(&daemon_ring_ctx.send_cv, NULL);
	daemon_sendthread_done = 0;

	if (rpcapd_opt.ringbuf_max_pkt_data < 10000) {
	    rpcapd_opt.ringbuf_max_pkt_data = 64000000;
	}
	printf("ringbuf_max_pkt_data=%d bytes\n", rpcapd_opt.ringbuf_max_pkt_data);
	daemon_ringbuf_len = rpcapd_opt.ringbuf_max_pkt_data;
	daemon_ringbuf = malloc(daemon_ringbuf_len);
	if (daemon_ringbuf == NULL) {
	    snprintf(errbuf, sizeof(errbuf) - 1, "Unable to create ringbuf len=%u",
	             daemon_ringbuf_len);
	    rpcap_senderror(fp->rmt_sockctrl, errbuf, PCAP_ERR_READEX, NULL);
        goto error;
	}

	if (rpcapd_opt.ringbuf_max_pkts < 8) {
	    rpcapd_opt.ringbuf_max_pkts = 65536;
	}
	printf("ringbuf_max_pkts=%d pkts\n", rpcapd_opt.ringbuf_max_pkts);
	daemon_ring = calloc(rpcapd_opt.ringbuf_max_pkts,
	                     sizeof(struct daemon_pkt_entry));
	if (daemon_ring == NULL) {
        snprintf(errbuf, sizeof(errbuf) - 1, "Unable to create ring pkt entries=%u",
                 rpcapd_opt.ringbuf_max_pkts);
        rpcap_senderror(fp->rmt_sockctrl, errbuf, PCAP_ERR_READEX, NULL);
        goto error;
	}
	daemon_ring_mask = rpcapd_opt.ringbuf_max_pkts - 1;
	printf("ring_mask=0x%x\n", daemon_ring_mask);


	// Modify thread params so that it can be killed at any time
	if (pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL) )
		goto error;
	if (pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL) )
		goto error;

	// set the udp outbound length to very large
	printf("setting udp pkt sndbuf to %d bytes\n",
	       rpcapd_opt.ringbuf_max_pkt_data);
#ifdef SO_SNDBUFFORCE
#define DAEMON_SO_SNDBUF    SO_SNDBUFFORCE
#else
#define DAEMON_SO_SNDBUF    SO_SNDBUF
    printf("SO_SNDBUFFORCE does not exist on this kernel, adjust manually:\n")
	printf("  $ sudo -i\n");
	printf("  $ echo 8388608 > /proc/sys/net/core/wmem_max\n");
#endif
	if (setsockopt(fp->rmt_sockdata, SOL_SOCKET, DAEMON_SO_SNDBUF,
	               (char *)&rpcapd_opt.ringbuf_max_pkt_data, sizeof(int)) < 0) {
	    perror("setsockopt(SO_SNDBUF) failed");
	}
	int sndbuf = 0;
	socklen_t sndbuf_len = sizeof(int);
	if (getsockopt(fp->rmt_sockdata, SOL_SOCKET, SO_SNDBUF,
                   (char *)&sndbuf, &sndbuf_len) < 0) {
        perror("getsockopt(SO_SNDBUF) failed");
    }
	printf("udp pkt sndbuf is set to %d bytes\n", sndbuf);
	if (set_non_blocking(fp->rmt_sockdata) < 0) {
	    perror("set_non_blocking(udp sock) failed");
	}

	printf("Starting packet sending thread\n");
    if ( pthread_create(&sendthread, NULL, (void *) daemon_sendthread_fn, (void *) fp) )
    {
        snprintf(errbuf, PCAP_ERRBUF_SIZE, "Error creating the sending thread");
        rpcap_senderror(fp->rmt_sockctrl, errbuf, PCAP_ERR_READEX, NULL);
        goto error;
    }
    sendthread_started = 1;

	// Retrieve the packets
	while ((retval = pcap_dispatch(fp->fp, 1000000, daemon_pkt_cb,
	                               (u_char *)fp)) >= 0)
	{
		if (retval == 0) {	// Read timeout elapsed
		    fp->read_timeouts++;
			continue;
		}
		if (retval > largest_retval) {
		    largest_retval = retval;
		    printf("retval=%d\n", retval);
		}
	}

	if (retval == -1)
	{
		snprintf(errbuf, PCAP_ERRBUF_SIZE, "Error reading the packets: %s", pcap_geterr(fp->fp) );
		rpcap_senderror(fp->rmt_sockctrl, errbuf, PCAP_ERR_READEX, NULL);
		goto error;
	}

error:

	SOCK_ASSERT(errbuf, 1);
 	closesocket(fp->rmt_sockdata);
	fp->rmt_sockdata= 0;

	free(sendbuf);

	daemon_sendthread_done = 1;
	if (sendthread_started) {
	    pthread_join(sendthread, NULL);
	}

    free(daemon_ringbuf);
    daemon_ringbuf = NULL;
    free(daemon_ring);
    daemon_ring = NULL;
	printf("Packet sending thread exiting\n");
	return NULL;
}



/*!
	\brief It serializes a network address.

	It accepts a 'sockaddr_storage' structure as input, and it converts it appropriately into a format
	that can be used to be sent on the network. Basically, it applies all the hton()
	conversion required to the input variable.

	\param sockaddrin: a 'sockaddr_storage' pointer to the variable that has to be
	serialized. This variable can be both a 'sockaddr_in' and 'sockaddr_in6'.

	\param sockaddrout: a 'sockaddr_storage' pointer to the variable that will contain
	the serialized data. This variable has to be allocated by the user.

	\return None

	\warning This function supports only AF_INET and AF_INET6 address families.
*/
void daemon_seraddr(struct sockaddr_storage *sockaddrin, struct sockaddr_storage *sockaddrout)
{
	memset(sockaddrout, 0, sizeof(struct sockaddr_storage) );

	// There can be the case in which the sockaddrin is not available
	if (sockaddrin == NULL) return;

	// Warning: we support only AF_INET and AF_INET6
	if (sockaddrin->ss_family == AF_INET)
	{
	struct sockaddr_in *sockaddr;

		sockaddr= (struct sockaddr_in *) sockaddrin;
		sockaddr->sin_family= htons(sockaddr->sin_family);
		sockaddr->sin_port= htons(sockaddr->sin_port);
		memcpy(sockaddrout, sockaddr, sizeof(struct sockaddr_in) );
	}
	else if (sockaddrin->ss_family == AF_INET6)
	{
	struct sockaddr_in6 *sockaddr;
	
		sockaddr= (struct sockaddr_in6 *) sockaddrin;
		sockaddr->sin6_family= htons(sockaddr->sin6_family);
		sockaddr->sin6_port= htons(sockaddr->sin6_port);
		sockaddr->sin6_flowinfo= htonl(sockaddr->sin6_flowinfo);
		sockaddr->sin6_scope_id= htonl(sockaddr->sin6_scope_id);
		memcpy(sockaddrout, sockaddr, sizeof(struct sockaddr_in6) );
	}
#ifdef AF_PACKET
	else if (sockaddrin->ss_family == AF_PACKET)
	{
	struct sockaddr_ll *sockaddr;

	    sockaddr= (struct sockaddr *)sockaddrin;
	    sockaddr->sll_family = htons(sockaddr->sll_family);
	    memcpy(sockaddrout, sockaddr, sizeof(struct sockaddr_ll));
	}
#endif
	else
	{
	struct sockaddr *sockaddr;
		sockaddr= (struct sockaddr *)sockaddrin;
		sockaddr->sa_family= htons(sockaddr->sa_family);
		memcpy(sockaddrout, sockaddr, sizeof(struct sockaddr) );
	}
}





/*!
	\brief Suspends a pthread for msec milliseconds.

	This function is provided since pthreads do not have a suspend() call.
*/
void pthread_suspend(int msec)
{
#ifdef WIN32
	Sleep(msec);
#else
struct timespec abstime;
struct timeval now;

	pthread_cond_t cond;
	pthread_mutex_t mutex;
	pthread_mutexattr_t attr;

	pthread_mutexattr_init(&attr);
	pthread_mutex_init(&mutex, &attr);
	pthread_mutex_lock(&mutex);

	pthread_cond_init(&cond, NULL);

	gettimeofday(&now, NULL);
	
	abstime.tv_sec = now.tv_sec + msec/1000;
	abstime.tv_nsec = now.tv_usec * 1000 + (msec%1000) * 1000 * 1000;

	pthread_cond_timedwait(&cond, &mutex, &abstime);

	pthread_mutex_destroy(&mutex);
	pthread_cond_destroy(&cond);
#endif
}



