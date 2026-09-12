/*

 Copyright (c) 2015-2026 unfacd works

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU Affero General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU Affero General Public License for more details.

 You should have received a copy of the GNU Affero General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

 */

#ifndef UFLIB_NET_SOCKET_TYPE_H
#define UFLIB_NET_SOCKET_TYPE_H

#include <uflib/adt/adt_queue.h>
#include <uflib/net/socket_message_type.h>

#define NO_SOCKETS    0
#define QUEUE_EMPTY 0

#ifndef MAXHOSTLEN
# define MAXHOSTLEN  165
#endif

#define SOCK_MAIN_LISTENER		1
#define SOCK_LISTENINGTELNET 	2
#define SOCK_CONNECTEDTELNET 	3
#define SOCK_PIPEREADER				4
#define SOCK_PIPEWRITER				5
#define SOCK_IPC             	6
#define SOCK_IPCCLIENT       	7
#define SOCK_SOCKSCOUNT      	8
#define SOCK_TCP							9
#define SOCK_UDP							10
#define SOCK_FD								11
#define SOCK_UNIX							12
#define SOCK_UFSRVQUEUEPUB		13
#define SOCK_UFSRVQUEUESUB		14
#define SOCK_COMMAND_CONSOLE	15
#define SOCK_INPROGRESS_CONN	16//for blocking connect()
#define SOCK_SIGNALFD      	  17

#define SOCKOPT_IP4 						(0x1<<1)
#define SOCKOPT_IP6 						(0x1<<2)
#define SOCKOPT_REUSEADDRE 			(0x1<<3)
#define SOCKOPT_BLOCKING 				(0x1<<4)
#define SOCKOPT_LINGER 					(0x1<<5)
#define SOCKOPT_REUSEPORT 			(0x1<<6)

#define SOCKOPT_IS_SET(x, y)		((x)->opts&y)
#define SOCKOPT_SET(x,y)				((x)->opts|=y)
#define SOCKOPT_UNSET(x,y)			((x)->opts&=~y)

//used where'eve call_flags are processed
#define SOCKMSG_DONTDECODE 									(0x1<<1)//b64 encoding
#define SOCKMSG_DONTENCODE 									(0x1<<2)//b64 encoing
#define SOCKMSG_READBUFFER 									(0x1<<3)
#define SOCKMSG_READSOCKET									(0x1<<4)
#define SOCKMSG_DONTWSFRAME									(0x1<<5)//no WS framing
#define SOCKMSG_COMPRESS										(0x1<<6)
#define SOCKMSG_ENQUEUE											(0x1<<7)//add message to applicable queue
#define SOCKMSG_ENCODED											(0x1<<8)
#define SOCKMSG_WSFRAMED										(0x1<<9)
#define SOCKMSG_ENCRYPTED										(0x1<<10)
#define SOCKMSG_DONTOWNSESNLOCK							(0x1<<11)//reading socketmessage, but donw own session lock
#define SOCKMSG_CONSOLIDATE_INSESSION				(0x1<<12)//where to consolidate Sessions' transient incoming SocketMessage
#define SOCKMSG_CONSOLIDATE_INSESSIONQUEUE	(0x1<<13)//where to consolidate Sessions' incoming SocketMessage queue
#define SOCKMSG_LOCK_SOCKMSGQUEUE						(0x1<<14)//lock Session 's SocketMessage queue
#define SOCKMSG_KEEPMSGQUEUE_LOCKED					(0x1<<15)//when reading into the message queue leave the lock on
#define SOCKMSG_READQUEUE										(0x1<<16)//when reading from message bugger, source it from the incoming queue
#define SOCKMSG__NEXT__											(0x1<<17)//

 struct Socket {
		int 			sock;

		unsigned 	opts,
							type,
							blocksz;

		size_t 		trbytes,//cummulative transfered
							rcbytes;//cumulative received

		long 			hport, //remote port
		  				port;	//local port

		char 			address[MAXHOSTLEN], //normally localhost
		  				haddress[MAXHOSTLEN];//remote address

//		ProtocolHeaderWebsocket protocol_header;

		SocketMessage socket_msg;//incoming buffer store
		SocketMessage socket_msg_out;//outgoing buffer store
	};
 typedef struct Socket Socket;

#define SOCKETTOINDEX(x) ((Index *)(x)->index)
#define QUEUE_NOT_EMPTY(x) ((x)->nMsg>=1)

#define SOCKET_OPTIONS(x)	(x)->opts
#define SOCKET_TYPE(x)		(x)->type
#define SOCKET_blocksz(x)	(x)->blocksz

#endif /* UFLIB_NET_SOCKET_TYPE_H */
