/*

todo: put packet types, and pointers to the functions for building them into a structure so its cleaner


*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <netinet/tcp.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <sys/time.h>
#include <pthread.h>
#include "network.h"
#include "antisurveillance.h"
#include "attacks.h"
#include "packetbuilding.h"
#include "adjust.h"
#include "utils.h"

// calculate checksum
unsigned short in_cksum(unsigned short *addr,int len) {
	register int sum = 0;
	u_short answer = 0;
	register u_short *w = addr;
	register int nleft = len;

        /*!
	* Our algorithm is simple, using a 32 bit accumulator (sum), we add
	* sequential 16 bit words to it, and at the end, fold back all the
	* carry bits from the top 16 bits into the lower 16 bits.
	*/
	while (nleft > 1)  {
		sum += *w++;
		nleft -= 2;
	}

	/*! mop up an odd byte, if necessary */
	if (nleft == 1) {
		*(u_char *)(&answer) = *(u_char *)w ;
		sum += answer;
	}

	/*! add back carry outs from top 16 bits to low 16 bits */
	sum = (sum >> 16) + (sum & 0xffff);     /*! add hi 16 to low 16 */
	sum += (sum >> 16);                     /*! add carry */
	answer = ~sum;                          /*! truncate to 16 bits */
	return(answer);
}





/* took some packet forging stuff I found online, and modified it...
   It was better than my wireshark -> C array dumping w memcpy... trying to hack this together as quickly as possible isnt fun :)

   "Generate TCP packets by jve Dated sept 2008"
*/

// Takes build instructions from things like HTTP Session generation, and creates the final network ready
// data buffers which will flow across the Internet
int BuildSingleTCP4Packet(PacketBuildInstructions *iptr) {
    int ret = -1;
    int TCPHSIZE = 20;

    // this is only for ipv4 tcp
    if (iptr->type != PACKET_TYPE_TCP_4) return ret;

    // increase the heaader by the size of the TCP options
    if (iptr->options_size) TCPHSIZE += iptr->options_size;

    // calculate full length of packet.. before we allocate memory for storage
    int final_packet_size = IPHSIZE + TCPHSIZE + iptr->data_size;

    unsigned char *final_packet = (unsigned char *)calloc(1, final_packet_size);
    struct packet *p = (struct packet *)final_packet;

    // ensure the final packet was allocated correctly
    if (final_packet == NULL) return ret;
    
    // IP header below
    p->ip.version 	= 4;
    p->ip.ihl   	= IPHSIZE >> 2;
    p->ip.tos   	= 0;    
    p->ip.frag_off 	= 0x0040;
    p->ip.protocol 	= IPPROTO_TCP;

    // Source, and destination IP addresses
    p->ip.saddr 	= iptr->source_ip;
    p->ip.daddr 	= iptr->destination_ip;

    // These two relate to dynamically changing information.. TTL=OS emulation, header identifier gets incremented..
    // and should be changed every connection that is wrote to the wire
    p->ip.id 	    = htons(iptr->header_identifier);
    p->ip.ttl 	    = iptr->ttl;

    // total length
    p->ip.tot_len   = htons(final_packet_size);
    

    // TCP header below
    // The source, and destination ports in question
    p->tcp.source   = htons(iptr->source_port);
    p->tcp.dest     = htons(iptr->destination_port);

    // The ACK/SEQ relate to variables incremented during normal communications..
    p->tcp.seq      = htonl(iptr->seq);
    p->tcp.ack_seq	= htonl(iptr->ack);

    // The TCP window relates to operating system emulation
    p->tcp.window	= htons(iptr->tcp_window_size);
    
    // syn/ack used the most
    p->tcp.syn  	= (iptr->flags & TCP_FLAG_SYN) ? 1 : 0;
    p->tcp.ack	    = (iptr->flags & TCP_FLAG_ACK) ? 1 : 0;
    p->tcp.psh  	= (iptr->flags & TCP_FLAG_PSH) ? 1 : 0;
    p->tcp.fin  	= (iptr->flags & TCP_FLAG_FIN) ? 1 : 0;
    p->tcp.rst	    = (iptr->flags & TCP_FLAG_RST) ? 1 : 0;

    
    p->tcp.check	= 0;	/*! set to 0 for later computing */
    p->tcp.urg	    = 0;    
    p->tcp.urg_ptr	= 0;
    p->tcp.doff 	= TCPHSIZE >> 2;

    // IP header checksum
    p->ip.check	    = (unsigned short)in_cksum((unsigned short *)&p->ip, IPHSIZE);

    /*
    // Lets make sure we cover all header variables.. just to be sure it wont be computationally possible
    // to filter out all packets....
    __u16 	res1:4 // supposed to be unused..
    __u16 	ece:1
    __u16 	cwr:1
    https://stackoverflow.com/questions/1480548/tcp-flags-present-in-the-header
    The two flags, 'CWR' and 'ECE' are for Explicit Congestion Notification as defined in RFC 3168.
    */

    // TCP header checksum
    if (p->tcp.check == 0) {
        struct pseudo_tcp *p_tcp = NULL;
        char *checkbuf = (char *)calloc(1,sizeof(struct pseudo_tcp) + TCPHSIZE + iptr->data_size);

        if (checkbuf == NULL) return -1;

        p_tcp = (struct pseudo_tcp *)checkbuf;

        p_tcp->saddr 	= p->ip.saddr;
        p_tcp->daddr 	= p->ip.daddr;
        p_tcp->mbz      = 0;
        p_tcp->ptcl 	= IPPROTO_TCP;
        p_tcp->tcpl 	= htons(TCPHSIZE + iptr->data_size);

        memcpy(&p_tcp->tcp, &p->tcp, TCPHSIZE);
        memcpy(checkbuf + sizeof(struct pseudo_tcp), iptr->options, iptr->options_size);
        memcpy(checkbuf + sizeof(struct pseudo_tcp) + iptr->options_size, iptr->data, iptr->data_size);        

        // put the checksum into the correct location inside of the header
        p->tcp.check = (unsigned short)in_cksum((unsigned short *)checkbuf, TCPHSIZE + PSEUDOTCPHSIZE + iptr->data_size + iptr->options_size);

        free(checkbuf);
    }

    // copy the TCP options to the final packet
    if (iptr->options_size)
        memcpy(final_packet + sizeof(struct packet), iptr->options, iptr->options_size);

    // copy the data to the final packet
    if (iptr->data_size)
        memcpy(final_packet + sizeof(struct packet) + iptr->options_size, iptr->data, iptr->data_size);
    

    // put the final packet into the build instruction structure as completed..
    iptr->packet = (char *)final_packet;
    iptr->packet_size = final_packet_size;

    // returning 1 here will mark it as GOOD
    return (ret = 1);
}



//https://tools.ietf.org/html/rfc1323
// Incomplete but within 1 day it should emulate Linux, Windows, and Mac...
// we need access to the attack structure due to the timestampp generator having a response portion from the opposide sides packets
// *** finish building tcp options data for the header
int PacketTCP4BuildOptions(AS_attacks *aptr, PacketBuildInstructions *iptr) {
    // need to see what kind of packet by the flags....
    // then determine which options are necessaray...
    // low packet id (fromm 0 being syn connection) would require the tcp window size, etc

    // options are here static.. i need to begin to generate the timestamp because that can be used by surveillance platforms
    // to attempt to weed out fabricated connections ;) i disabled it to grab this raw array
    unsigned char options[12] = {0x02, 0x04, 0x05, 0xb4, 0x01, 0x01, 0x04, 0x02, 0x01, 0x03,0x03, 0x07};
    // this is preparing for when we have dynamic options...
    char *current_options = NULL;

    int current_options_size = 12;

    // verify that we should even generate the options.. if not return 1 (no error)
    if (!(iptr->flags & TCP_OPTIONS))
        return 1;
    /*
    if (iptr->flags & TCP_OPTIONS_TIMESTAMP) {
        current_options_size += 8;
        // generate new options.. into current_options[_size]
    }*/


    current_options = (char *)calloc(1, current_options_size);
    if (current_options == NULL) return -1;

    PtrFree(&iptr->options);

    // *** generate options using flags.. timestamp+window size
    // until we generate using flags...
    memcpy(current_options, options, 12);

    iptr->options_size = current_options_size;
    iptr->options = current_options;

    return 1;
}



// This function takes the linked list of build instructions, and loops to build out each packet
// preparing it to be wrote to the Internet.
// This will return on failure.  It is meant to generate packets for an entire sessions instructions.  If you need
// a packet build without some context (such as a connection) then perform it using that types function.
void BuildTCP4Packets(AS_attacks *aptr) {
    PacketBuildInstructions *ptr = aptr->packet_build_instructions;
    PacketInfo *qptr = NULL;

    if (ptr == NULL) {
        aptr->completed = 1;
        return;
    }

    while (ptr != NULL) {
        // Build the options, single packet, and verify it worked out alright.
        if (ptr->type == PACKET_TYPE_TCP_4) {
            // Build a sessions TCP packets
            if ((PacketTCP4BuildOptions(aptr, ptr) != 1) || (BuildSingleTCP4Packet(ptr) != 1) ||
                    (ptr->packet == NULL) || (ptr->packet_size <= 0)) {
                // Mark for deletion otherwise
                aptr->completed = 1;

                return;
            }
        }

        // everything went well...
        ptr->ok = 1;

        ptr = ptr->next;
    }

    // All packets were successful.. lets move them to a different structure..
    // PacketInfo is the structure used to put into the outgoing network buffer..
    // this mightt be possible to remove.. but i wanted to give some room for additional
    // protocols later.. so i decided to keep for now...
    ptr = aptr->packet_build_instructions;

    while (ptr != NULL) {
        if ((qptr = (PacketInfo *)calloc(1, sizeof(PacketInfo))) == NULL) {
            // Allocation issue.. mark completed
            aptr->completed = 1;
            return;
        }

        qptr->type = PACKET_TYPE_TCP_4;
        qptr->buf = ptr->packet;
        qptr->size = ptr->packet_size;
        // These are required for sending the packet out on the raw socket.. so lets pass it
        qptr->dest_ip = ptr->destination_ip;
        qptr->dest_port = ptr->destination_port;

        // This should really only matter later.. once they begin 'attempting' to process out
        // false packets.. Doubtfully going to work in any capacity.
        qptr->wait_time = 0;

        // so we dont double free.. lets just keep in the new structure..
        // again i might remove this later... but wanted some room for other upgrades
        // i dont wish to discuss yet ;)
        ptr->packet = NULL;
        ptr->packet_size = 0;

        // link FIFO into the attack structure
        L_link_ordered((LINK **)&aptr->packets, (LINK *)qptr);

        ptr = ptr->next;
    }

    return;
}



// free all packets within an attack structure
void PacketsFree(PacketInfo **packets) {
    PacketInfo *ptr = NULL, *pnext = NULL;

    if (packets == NULL) return;

    ptr = *packets;

    // verify there are packets there to begin with
    if (ptr == NULL) return;

    // free all packets
    while (ptr != NULL) {
        // once AS_queue() executes on this.. it moves the pointer over
        // so it wont need to be freed from here (itll happen when outgoing buffer flushes)
        PtrFree(&ptr->buf);

        // keep track of the next, then free the current..
        pnext = ptr->next;

        // free this specific structure element
        free(ptr);

        // now use that pointer to move forward..
        ptr = pnext;
    }

    // no more packets left... so lets ensure it doesn't get double freed
    *packets = NULL;

    return;
}


// This is one of the main logic functions.  It handles sessions which are to be replayed many times, along with the timing 
// logic, and it calls other functions to queue into the network outgoing queue
void PacketQueue(AS_context *ctx, AS_attacks *aptr) {
    PacketInfo *pkt = NULL;
    struct timeval tv;
    struct timeval time_diff;

    gettimeofday(&tv, NULL);

    // if this thread is paused waiting for some other thread, or process to complete a task
    if (aptr->paused == 1) return;

    // if its already finished.. lets just move forward
    if (aptr->completed) return;

    // onoe of these two cases are correct fromm the calling function
    if (aptr->current_packet != NULL)
        pkt = aptr->current_packet;
    else {
        // we do have to reprocess these packets fromm packet #1?
        if (aptr->count == 0) {
            // Free all packets (it will put a NULL at its pointer location afterwards)
            PacketsFree(&aptr->packets);

            // If there was anything in current_packet then its freed already from the function above
            aptr->current_packet = NULL;
            
            aptr->completed = 1;

            return;
        }

        // lets start it over..
        pkt = aptr->packets;        
    }

    if (pkt == NULL) {
        // error shouldnt be here...
        aptr->completed = 1;

        return;
    }

    // is it the first packet?
    if (pkt == aptr->packets) {
        // if we are about to replay this attack again from the first packet due to a repeat count.. then
        // verify enough time has elapsed to match our repeat interval (IN seconds)
        timeval_subtract(&time_diff, &aptr->ts, &tv);
        if (time_diff.tv_usec < aptr->repeat_interval)
            // we are on the first packet and it has NOT been long enough...
            return;

        // derement the count..
        //aptr->count--;

        // aptr->ts is only set if it was already used once..
        if (aptr->ts.tv_sec) {
            // free older packets since we will rebuild
            PacketsFree(&aptr->packets);
            // we wanna start fresh..
            aptr->current_packet = NULL;

            // we have some adjustments to make (source port, identifiers, etc)
            PacketAdjustments(aptr);

            // we must refresh this pointer after that..
            pkt = aptr->packets;
        }

        // If its marked as completed for any reason, then we are done.
        if (aptr->completed) return;
    } else {
        // Is it too soon to send this packet? (we check its milliseconds)
        timeval_subtract(&time_diff, &aptr->ts, &tv);

        if (time_diff.tv_usec < pkt->wait_time) {
            return;
        } 
    }

    // Queue this packet into the outgoing queue for the network wire
    AS_queue(ctx, aptr, pkt);

    // We set this pointer to the next packet for next iteration of AS_perform()
    aptr->current_packet = pkt->next;

    // time couldnt have changed that much.. lets just copy it over
    memcpy(&aptr->ts, &tv, sizeof(struct timeval)); 

    return;
}



int BuildSingleUDP4Packet(PacketBuildInstructions *iptr) {
    int ret = -1;

    // this is only for ipv4 tcp
    if (iptr->type != PACKET_TYPE_UDP_4) return ret;

    // calculate full length of packet.. before we allocate memory for storage
    int final_packet_size = sizeof(struct iphdr) + sizeof(struct udphdr) + iptr->data_size;
    unsigned char *final_packet = (unsigned char *)calloc(1, final_packet_size);
    struct packetudp4 *p = (struct packetudp4 *)final_packet;

    // ensure the final packet was allocated correctly
    if (final_packet == NULL) return ret;

    // IP header below
    p->ip.version       = 4;
    p->ip.ihl   	    = IPHSIZE >> 2;
    p->ip.tos   	    = 0;    
    p->ip.frag_off 	    = 0;
    p->ip.protocol 	    = IPPROTO_UDP;
    p->ip.tot_len       = htons(final_packet_size);

    //p->udp

}



int BuildSingleICMP4Packet(PacketBuildInstructions *iptr) {
    int ret = -1;

    // this is only for ipv4 tcp
    if (iptr->type != PACKET_TYPE_ICMP_4) return ret;

    /*
     //ip header
    struct iphdr *ip = (struct iphdr *) packet;
    struct icmphdr *icmp = (struct icmphdr *) (packet + sizeof (struct iphdr));
     
    //zero out the packet buffer
    memset (packet, 0, packet_size);
 
    ip->version = 4;
    ip->ihl = 5;
    ip->tos = 0;
    ip->tot_len = htons (packet_size);
    ip->id = rand ();
    ip->frag_off = 0;
    ip->ttl = 255;
    ip->protocol = IPPROTO_ICMP;
    ip->saddr = saddr;
    ip->daddr = daddr;
    //ip->check = in_cksum ((u16 *) ip, sizeof (struct iphdr));
 
    icmp->type = ICMP_ECHO;
    icmp->code = 0;
    icmp->un.echo.sequence = rand();
    icmp->un.echo.id = rand();
    //checksum
    icmp->checksum = 0;
    */
}

/*
coming soon:
int BuildSingleTCP6Packet(PacketBuildInstructions *iptr);
int BuildSingleUDP6Packet(PacketBuildInstructions *iptr);
int BuildSingleICMP6Packet(PacketBuildInstructions *iptr);


need a sniffer which can use filters to find particular sessions
should have heuristics to find somme gzip http, dns, etc.. 

so the tool can get executed, and automatically populate & initiate
*/