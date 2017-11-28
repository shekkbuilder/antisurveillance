

/*

Research is everything related to strategy choices.  For example, which sites do we wish to spend the majority
of our bandwidth falsifying connections towards?  It will perform DNS lookups, traceroutes, and BGP analysis to
determine the best IP 4/6 addresses to use for attacks.

For local attacks: NIDs, etc.. It will take the local IP addresses, and find the best ways of attacking
the platform to either hide in other packets, or attempt to force other issues such as Admin believing hacks
are taking place elsewhere.. etc

I'll try to require as little hard coded information as possible.  I'd like everything to work without requiring any updates -- ever.



It took 15 hours at 50 active traceroute queue to complete 53,000 traceroutes (DNS results from top 1mil sites using massdns)
Obviously 53,000 isnt all of them.. ill have to add more timeout interval later...

The data seemed incomplete so I think I finalized the save, and load mechanism.  It can also attempt to retry any missing TTL
entries for targets whenever the queue reaches zero.  It will also randomly disable/enable different queues.  I need to perfect
the system.  I figured sending out more than 50 (maybe 1000) packets a second should mean 1000 responses..

Fact is.. the hops/routers seem to rate limit ICMP, and supposed to block something like 66% in general.  I know
sending more packets per second to close routes (0-3 hops) means they get every single traceroute lookup.

I'll finish and make the RandomizeTTLs first try 8-15 on large amounts of active queue.  I'm pretty sure the targets, and distant
routers/hops will respond therefore if you are sending to a high amount of these then the amount of active traceroutes SHOULD
in fact be increased....

I might just revamp the entire 'queue' system and move it from an active count based to attempting high ttl amounts more, and
going for the LOW ttl amounts later.  It could allow normal until it finds all hosts which are probably the local ISP..
for instance.. trace routing 3000 hosts, and 1000-1500 hit this particular hop then its more than likely going to be uused for most..
the 1500 (near 50%) was what i was seeing for hops 0-3.. so obviously there was some that were ignored by the routes and should be retried
therefore this 1500(or 50%) should be dynamically calculated between the first 3 hops and the initial few thousand lookups
then it can reuse that number to determine which hops are probably the closest to prioritize as last..

so it should get handled after the higher TTL.. so it can set a 'minimum' TTL until the majority of the queue is completed..

it also means that we can auto 'invisibly' add these hops until we get the actual data.. which means we can do more with less...

I hoped to finish today but I should by tommorrow...

btw 15 hours at 3-4kb/sec isnt that serious.. LOL its not as bad as it sounds.. packets are small

for strategies using research:
we wanna construct a context of the strategy we are trying to build
if we cannot find all traceroute, or other information we can then retry, or queue and allow a callback to continue
the strategies whenever the information is complete...

this is also necessary for one of the upcomming attacks

if a single hop is missing, or two.. we can use other results and just fill in the gaps if they share simmilar/near nodes/hops/routers

traceroute_max_active shouild count incoming packets/successful lookups and autoatically adjust itself to get the most packets



The data and information from the automated traceroute can get reused in several different ways.  Reverse DDoS (to detect hacking
even through tor, etc) can use information/routes from this to pick, and calculate the attacks to perform to automate
finding the target


we shouild dns lookup traceroutees to get more IPs (Reverse, and regular..) we can also look for dns responses with multiple ips
need to scan for open dns servers (to get geoip dns automatically)



*/


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <netinet/ip_icmp.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stddef.h> /* For offsetof */
#include "network.h"
#include "antisurveillance.h"
#include "packetbuilding.h"
#include "research.h"
#include "utils.h"


#ifndef offsetof
#define offsetof(type, member) ( (int) & ((type*)0) -> member )
#endif



// find a spider structure by target address
TracerouteSpider *Traceroute_FindByTarget(AS_context *ctx, uint32_t target_ipv4, struct in6_addr *target_ipv6) {
    TracerouteSpider *sptr = ctx->traceroute_spider;

    while (sptr != NULL) {
        if (target_ipv4 && target_ipv4 == sptr->target_ip)
            break;

        if (!target_ipv4 && CompareIPv6Addresses(&sptr->target_ipv6, target_ipv6))
            break;

        sptr = sptr->next;
    }

    return sptr;
}


// find a spider structure by its hop (router) address
TracerouteSpider *Traceroute_FindByHop(AS_context *ctx, uint32_t hop_ipv4, struct in6_addr *hop_ipv6) {
    TracerouteSpider *sptr = ctx->traceroute_spider;

    while (sptr != NULL) {
        if (hop_ipv4 && hop_ipv4 == sptr->hop_ip)
            break;

        if (!hop_ipv4 && CompareIPv6Addresses(&sptr->hop_ipv6, hop_ipv6))
            break;

        sptr = sptr->next;
    }

    return sptr;
    
}

// find a spider structure by its identifier (query identification ID fromm traceroute packets)
TracerouteSpider *Traceroute_FindByIdentifier(AS_context *ctx, uint32_t id, int ttl) {
    TracerouteQueue *qptr = TracerouteQueueFindByIdentifier(ctx, id);

    if (qptr == NULL) return NULL;

    return qptr->responses[ttl];
}


// retry for all missing TTL for a particular traceroute queue..
int Traceroute_Retry(AS_context *ctx, TracerouteQueue *qptr) { //uint32_t identifier) {
    int i = 0;
    int ret = -1;
    int cur_ttl = 0;
    TracerouteSpider *sptr = NULL;
    //TracerouteQueue *qptr = NULL;
    int missing = 0;

    //printf("traceroute_retry\n");
    if (qptr == NULL) {
        //printf("qptr is null? \n");
        return -1;
    }
    
    if (ctx->traceroute_max_retry && (qptr->retry_count > ctx->traceroute_max_retry)) {
        //printf("reached max retry.. max: %d qptr: %d\n", ctx->traceroute_max_retry, qptr->retry_count);
        ret = 0;
        goto end;
    }

    // loop for all TTLs and check if we have a packet from it
    for (i = ctx->traceroute_min_ttl; i < MAX_TTL; i++) {
        //sptr = Traceroute_FindByIdentifier(ctx, qptr->identifier, i);
        sptr = qptr->responses[i];

        // if we reached the target... its completed
        // *** add ipv6
        if (sptr && sptr->target_ip == qptr->target_ip) {
            break;
        }

        // if we cannot find traceroute responses this query, and this TTL
        if (sptr == NULL) {
            if (cur_ttl < MAX_TTL) {
                // we put it back into the list..
                qptr->ttl_list[cur_ttl++] = i;
            }
            missing++;
        }
        
        // if the source IP matches the target.. then it is completed

    }

    if (missing) {
        // set queue structure to start over, and have a max ttl of how many we have left
        qptr->current_ttl = 0;
        qptr->max_ttl = cur_ttl;
        qptr->retry_count++;

        RandomizeTTLs(qptr);

        // its not completed yet..
        qptr->completed = 0;

        //printf("Enabling incomplete traceroute %u [%d total]\n", qptr->target_ip, cur_ttl);
    } else {
        //printf("no missing on %u\n", qptr->target_ip);
    }


    ret = 1;

    end:;
    return ret;
}


// loop and try to find all missing traceroute packets
// we wanna do this when our queue reaches 0
int Traceroute_RetryAll(AS_context *ctx) {
    int i = 0;
    int ret = -1;
    TracerouteQueue *qptr = NULL;

    //printf("retry all\n");

    // loop for all in queue
    qptr = ctx->traceroute_queue;

    while (qptr != NULL) {
        // retry command for this queue.. itll mark to retransmit all missing
        i = Traceroute_Retry(ctx, qptr);

        if (i) {
            //printf("successful retry on %p\n", qptr);
            ret++;
        }

        qptr = qptr->next;
    }

    if (ret) {
        Traceroute_AdjustActiveCount(ctx);
        //printf("%d traceroutes being retried\n", ret);
    }

    return ret;
}

// walk all traceroutes with the same 'identifier' in order
int walk_traceroute_hops(AS_context *ctx, TracerouteSpider *sptr, TracerouteSpider *needle, int cur_distance) {
    int s_walk_ttl = 0;
    TracerouteSpider *s_walk_ptr = NULL;
    int ret_distance = 0;
    TracerouteSpider *s_branch_ptr = NULL;
    int bcount = 0;
    int r = 0;

    if (cur_distance > MAX_TTL) return 0;

    printf("walk_traceroute_hops(): cur_distance %d\n", cur_distance);

    // walk for all possible TTLs
    while (s_walk_ttl < MAX_TTL) {
        if ((s_walk_ptr = Traceroute_FindByIdentifier(ctx, sptr->identifier_id, s_walk_ttl)) != NULL) {
            printf("1 s_walk_ptr ttl %d returned %p identifier %u ttl %d\n", s_walk_ttl, s_walk_ptr, sptr->identifier_id,sptr->ttl);

            // if we have a match...
            if (s_walk_ptr == needle) {
                // since ist the same identifier.. we calculate the difference between TTLs
                ret_distance = needle->ttl - sptr->ttl;

                if (ret_distance < 0) ret_distance = abs(ret_distance);

                ret_distance += cur_distance;

                goto end;

            }
        } else {
            printf("culdnt find identifier %X ttl %d\n", sptr->identifier_id, s_walk_ttl);
        }

        s_walk_ttl++;
    }

    s_walk_ttl = 0;
    // walk for all possible TTLs
    while (s_walk_ttl < MAX_TTL) {
        if ((s_walk_ptr = Traceroute_FindByIdentifier(ctx, sptr->identifier_id, s_walk_ttl)) != NULL) {
            printf("2 s_walk_ptr ttl %d returned %p identifier %u ttl %d\n", s_walk_ttl, s_walk_ptr, sptr->identifier_id, sptr->ttl);

            // count branches in this particular identifier's TTL (traceroute noode, and what other traceroutes have simmilar)
            bcount = L_count_offset((LINK *)s_walk_ptr->branches, offsetof(TracerouteSpider, branches));
            printf("branch count: %d\n", bcount);
             
            // if there are branches to transverse
            if (bcount) {
                s_branch_ptr = s_walk_ptr->branches;
                while (s_branch_ptr != NULL) {

                    cur_distance += 1;

                    // recursively look through that branche's entire traceroute hop list
                    r = walk_traceroute_hops(ctx, s_branch_ptr, needle, cur_distance);

                    if (r > 0) {
                        ret_distance = needle->ttl - sptr->ttl;

                        if (ret_distance < 0) ret_distance = abs(ret_distance);

                        ret_distance += cur_distance;

                        // add the distance that was returned fromm that call to walk_traceroute_hops()
                        ret_distance += r;

                        goto end;
                    }

                    cur_distance -= 1;

                    s_branch_ptr = s_branch_ptr->branches;
                }
            }
        } else {
            //printf("culdnt find identifier %X ttl %d\n", sptr->identifier_id, s_walk_ttl);
        }

        s_walk_ttl++;
    }

end:;
    return ret_distance;
}




// this can be handeled recursively because we have max TTL of 30.. its not so bad
int Traceroute_Search(AS_context *ctx, TracerouteSpider *start, TracerouteSpider *looking_for, int distance) {
    TracerouteSpider *startptr = NULL;
    TracerouteSpider *lookingptr = NULL;

    TracerouteSpider *sptr2 = NULL;
    TracerouteSpider *lptr2 = NULL;

    TracerouteSpider *s_walk_ptr = NULL;
    TracerouteSpider *l_walk_ptr = NULL;

    int sdistance = 0;
    int ldistance = 0;

    int s_ttl_walk = 0;
    int l_ttl_walk = 0;

    int cur_distance = distance;
    int ret = 0;
    int ttl_diff = 0;

    // if distance is moore than max ttl.. lets return
    if (distance >= MAX_TTL) return 0;

    // if pointers are NULL for some reason
    if (!start || !looking_for) return 0;

    // ensure we dont go in infinite loop
    //if (start->search_context.second == looking_for) return 0;
    //start->search_context.second = looking_for;

    // dbg msg
    printf("Traceroute_Search: start %p [%u] looking for %p [%u] distance: %d\n",  start, start->hop_ip, looking_for, looking_for->hop_ip, distance);
    
    /*// use context here and use the next list..
    search = start;
    
    // first we search all branches, and perform it recursively as well...
    while (search != NULL) {

        if (search->branches) {
            // increase distance since we are accessing a branch
            cur_distance++;

            // get the first element
            search_branch = search->branches;
        
            // we will loop until its NULL
            while (search_branch != NULL) { */
/*

we should start looking for TARGETs .. not necessarily hops..
hops are just used to connect the various nodes... although if a target is not found we must traceroute it, and hope it matches

if we cant find the target.. its possible it is ignoring ICMP packets... but the routers should have responded
so we can check traceroute queries to look for the target and grab its identifier
(in the future.. all logs should contain the target, as well as the  identifier.. i think i might have it now)
but double checck everywhere..
or log the original queeues as well

and if it doesnt match we need enough IPs in various locations to attempt to fill in the blank
(there should be a way to queue for IP investigations, or modify the IP to adjust as close as possible)
just to ensure we go through the fiber taps which we were targeting
also we need to modify for other reasons thatll be clear soon to pull all 3 types of attacks together :)


192.168.0.1   b      192.168.0.1         
cox nola h           cox nola  h          
cox houston             i            cox houston h                cox houston h
                       i                                                                 cox L

Traceroute_FindByHop
Traceroute_FindByTarget
Traceroute_FindByIdentifier

when target -> identifier -> hop 

hop -> branch (doesnt count as increment in distance.. its the same.. but once it leaves that structure
to spider another identifier (diff ttl hop in same identifier/query) then it increments

branch -> branch doesnt increment

target -> hop increments
hop -> target increments

-----


walks identifier (single traceroute list) looking for match
if it finds it. the distance is the TTL diff between the start, and finding it

if it doesnt match, then it checks all of those agfain but for each hop itll check whether or not it has ther branches (other traceroutes which share hops)
and then itll enumerate over all of their hops looking for the alternative... each time it goes into these banches to verify itll take the TTL diff, and then increase distance

as it goes into each of those as well... and if it finds it.. itll calculate the differencec between getting into that branch (its ttl) and the solution.. then itll have the
other ttl calculation of ttl diff between the start, and the branch it is inside of (before starting a new distancce between that TTL, and its other TTL which matches)


so walk_identifier(start, loooking for, initial_distance) could be run on both sides... 
and when it goes into another banch it can calculate the ttl and change the distance and then recursively look

it wouldnt use 100% recursive since it is looking up each TTL separately instead of following a linked list


another option is to turn the entire thing into arbitrary nodes, or use something like infomap
nodes which doesnt matter what is branches, TTL ,etc..
 (lose the bullshit C++ structure that is giving us issues)

 can hash all traceroute spider structures, and link them together

 prev next
  ___      ___
 |   |    |   |
 |   | oo |   |  
  ---      ---


ok so .. when i was testing.. I was seeing some identifiers (Traceroutes) which had one TTL packet
of 24.. and nothing below... so now im going to log all queries, and allow loading

and this algorithm, or another functin which  detects high amounts of missing
TTL will relaunch the traceroute queries again for those to replace
TTLs which are  missing (especially if >50-60% of traceroute is missing)
this should allow a consitent retry method to fill in the gaps
its essentailly due to rate limiting, etc...

and since this will take place it might be possible to handle more packets quicker
since we will just retry.. ill test a few possibilities to see what works fastest

*/

    // lets find it again by target...
    if ((startptr = Traceroute_FindByTarget(ctx, start->target_ip, NULL)) == NULL) {
        printf("start: couldnt find target.. lets ?? dunno\n");
        // lets try to perform actionss fromm the structuree we were passed
        startptr = looking_for;
    }
    sdistance = 0; // keep track of startptr's search distance
    //s_ttl_walk = 0; // ttl walk is what TTL we are currently checking

    if ((lookingptr = Traceroute_FindByTarget(ctx,looking_for->target_ip, NULL)) == NULL) {
        printf("looking: couldnt find target.. lets ?? dunno\n");
        lookingptr = startptr;
    }
    //ldistance = 0; // keep track of lookingptr's search distance
    //l_ttl_walk = 0; // current ttl we are checking

    printf("will wak %p %p\n", startptr, lookingptr);
    sdistance = walk_traceroute_hops(ctx, startptr, lookingptr, 0);
    printf("sdistance: %d\n", sdistance);
    // loop for each TTL...

/*

    // loop on startptr side
    for (s_ttl_walk = 0; s_ttl_walk < MAX_TTL; s_ttl_walk++) {
        if ((s_walk_ptr = Traceroute_FindByIdentifier(ctx, startptr->identifier, s_walk_ttl)) != NULL) {
            if (s_walk_ptr == lookingptr) {
                // did we find it???
                ttl_diff = s_walk_ptr->ttl - lookingptr->ttl;
                // turn -3 to 3... or -5 to 5.. etc.. we just need the difference
                if (ttl_diff < 0) ttl_diff = abs(ttl_diff);

                // and thats our distance
                return t;
            }
        }
    }

    // loop on looking ptr side...


    // we take both sides and follow the entire receiver heaader until we rewach MAX_TTL..
    // looking for the other side...

    // then we will branch into each TTL (following the similar hops).. every branch we will then follow all of its TTLs
    // increasing distance as we get further and branch into it


    do {

        // look for both sides from each other..
        sptr2 = startptr;
        while (sptr2 != NULL) {

            sptr2 = sptr->next;
        }

        lptr2 = lookingptr;
        while (lptr2 != NULL) {
            lptr2 = lptr2->next;
        }


    } while(1);


*/
/*
                // if the IPv4 address matches.. we found it.. now we have to walk all paths until we literally reach it
                // not just a pointer in the linked list... we have to see how many steps to arrive
                if (search_branch->hop_ip == looking_for->hop_ip) {





                    
                }

                // if not.. then we wanna recursively search this branch.. so increase distance, and  hit this function with this pointer
                //ret = Traceroute_Search(search_branch, looking_for, cur_distance + 1);

                // if it was found..return the distance
                //if (ret) return ret;

                // move to the next branch in this list
                search_branch = search_branch->branches;
            }

            // we decrement hte distance since it wasn't used...
            cur_distance--;
        }

        // movve to the next traceroute response in our main list
        search = search->next;
    }
*/
    return ret;
    // disabling anything under here .. dev'ing maybe rewrite
    /*
    // next we wanna search fromm the  identifiers list (it could be 2 routers away) so distance of 2..
    search = start->identifiers;

    // increase distance so that it is calculated correctly if it finds the needle in this haystack
    cur_distance++;
    // loop until the identifier list is completed..
    while (search != NULL) {

        // does the IPv4 address match?
        if (start->hop_ip == looking_for->hop_ip) {
            // calculate the TTL difference (which tells how many hops away.. which is pretty mcuh the same as branches)
            ttl_diff = start->ttl - looking_for->ttl;

            // if its <0 it means the start->ttl was alrady lower.. lets just get the absolute integer( turn negative to positive)
            if (ttl_diff < 0) ttl_diff = abs(ttl_diff);

            // if we have a value then add the current distance to it
            if (ttl_diff) {
                // set ret so that it returns it to the calling function
                ret = ttl_diff + cur_distance;
                break;
            }

            // otherwise we wanna go into this identifiers branch

            // *** this was recursively doing an inf loop.. will fix soon
            search_branch = NULL; // search->branches;

            // we will loop until all the branches have been checked
            while (search_branch != NULL) {
                // we wanna check branches of that traceroute (identifier) we are checking    

                // increase the distance, and call this function again to recursively use the same algorithm
                ret = Traceroute_Search(search_branch, looking_for, cur_distance+1);

                    // if it was found..
                if (ret) return ret;

                // move to the  next branch in this identifier list
                search_branch = search_branch->branches;
            }
        }

        // move to the next hop in this traceroute (identifier) list
        search = search->identifiers;
    }

    // decrement the distance (just to keep things clean)
    cur_distance--;

    return ret;*/
}


// traceroutes are necessary to ensure a single nonde running this code can affect all mass surveillance programs worldwide
// it allows us to ensure we cover all places we expect them to be.. in the world today: if we expect it to be there.. then it
// probably is (for mass surveillance programs)
//https://www.information.dk/udland/2014/06/nsa-third-party-partners-tap-the-internet-backbone-in-global-surveillance-program
// we want to go through asa many routes as possible which means we are innjecting information into each surveillance tap along the way
// the other strategy will be using two nodes running this code which will be on diff parts of the world so we ca ensure eah side of the packets
// get processed correctly.. in the begininng (before they modify) it wont matter.. later once they attempt to filter out, and procecss
// it might matter but it'll make the entire job/technology that much more difficult
int Traceroute_Compare(AS_context *ctx, TracerouteSpider *first, TracerouteSpider *second) {
    int ret = 0;
    TracerouteSpider *srch_main = NULL;
    TracerouteSpider *srch_branch = NULL;
    int distance = 0;

    // make sure both were passed correctly
    if (!first || !second) return -1;

    // if they are the same..
    if (first->hop_ip == second->hop_ip) return 1;

    // we wanna call this function Traceroute_Search to find the distance  of the two spider parameters passed
    distance = Traceroute_Search(ctx,first, second, 0);

    // print distance to screen
    printf("distance: %d\n", distance);

    // prepare to return it..
    ret = distance;

    end:;

    return ret;
}





int TracerouteQueueFindByIdentifier(AS_context *ctx, uint32_t identifier) {
    TracerouteQueue *qptr = ctx->traceroute_queue;
    while (qptr != NULL) {

        if (qptr->identifier == identifier) break;

        qptr = qptr->next;
    }

    return qptr;
}


// link with other traceroute structures of the same queue (same target/scan)
int Spider_IdentifyTogether(AS_context *ctx, TracerouteSpider *sptr) {
    TracerouteSpider *srch = ctx->traceroute_spider;
    int ret = 0;
    int a, b;
    TracerouteQueue *qptr = TracerouteQueueFindByIdentifier(ctx, sptr->identifier_id);

    if (qptr == NULL) return -1;

    // lets give the original queue a direct pointer to every TTL responses regarding its lookup
    // its much easier than having it in a linked list.. especially for later analysis
    qptr->responses[sptr->ttl] = sptr;

    ret = 1;    

    return ret;
}


// randomize TTLs.. for adding a new traceroutee queue, and loading data files with missing TTLs we wish to retry
void RandomizeTTLs(TracerouteQueue *tptr) {
    int i = 0, n = 0, ttl = 0;

    // only randomize if more than 5
    if (tptr->max_ttl <= 5) return;

    // randomize TTLs between 0 and 15 (so each hop doesnt get all 50 at once.. higher chance of scanning  probability of success)    
    for (i = 0; i < (tptr->max_ttl / 2); i++) {
        // array randomization
        // pick which 0-15 we will exchange the current one with
        n = rand()%tptr->max_ttl;
        // use 'ttl' as temp variable to hold that TTL we want to swap
        ttl = tptr->ttl_list[n];

        // swap it with this current enumeration by the i for loop
        tptr->ttl_list[n] = tptr->ttl_list[i];

        // move from swapped variable to complete the exchange
        tptr->ttl_list[i] = ttl;
    }
}

// take the TTL list, and remove completed, or found to decrease the 'max_ttl' variable so later we can randomly
// enable/disable queues so we can scan a lot more (randomly as well)
void ConsolidateTTL(TracerouteQueue *qptr) {
    int ttl_list[MAX_TTL+1];
    int i = 0;
    int cur = 0;

    // loop and remove all completed ttls...
    while (i < qptr->max_ttl) {
        if (qptr->ttl_list[i] != 0)
            ttl_list[cur++] = qptr->ttl_list[i];
        i++;
    }

    // copy the ones we found back into the queue structure
    i = 0;
    while (i < cur) {
        qptr->ttl_list[i] = ttl_list[i];
        i++;
    }

    // set current to now so it can start
    qptr->max_ttl = cur;
    qptr->current_ttl = 0;
    qptr->ts_activity = 0;

    // done..
}

// lets randomly disable all queues, and enable thousands of others...
// this is so we dont perform lookups immediately for every traceroute target..
int Traceroute_AdjustActiveCount(AS_context *ctx) {
    int ret = 0;
    int disabled = 0;
    int count = 0;
    int r = 0;
    TracerouteQueue *qptr = ctx->traceroute_queue;

    if (!qptr) return -1;

    count = L_count((LINK *)qptr);

    // disable ALL currently enabled queues.. counting the amount
    while (qptr != NULL) {
        // if its enabled, and not completed.. lets disable
        if (qptr->enabled && !qptr->completed) {
            qptr->enabled = 0;
            disabled++;
        }
        qptr = qptr->next;
    }

    // now enable the same amount of random queues
    // ret is used here so we dont go over max amt of queue allowed..
    // this allowedw this function to get reused during modifying traffic parameters
    while (disabled && (ret < ctx->traceroute_max_active)) {

        // pick a random queue
        r = rand()%count;

        // find it..
        qptr = ctx->traceroute_queue;
        while (r-- && qptr) { qptr = qptr->next; }

        // some issues that shouldnt even take place..
        if (!qptr) continue;

        if (!qptr->completed) {
            qptr->enabled = 1;
            disabled--;
            ret++;
        }

        // we will do that same loop until we have enough enabled..
    }
    
    return ret;
}


// Analyze a traceroute response again  st the current queue and build the spider web of traceroutes
// for further strategies
int TracerouteAnalyzeSingleResponse(AS_context *ctx, TracerouteResponse *rptr) {
    int ret = 0;
    TracerouteQueue *qptr = ctx->traceroute_queue;
    TracerouteSpider *sptr = NULL, *snew = NULL, *slast = NULL;
    struct in_addr src;
    int i = 0;
    int left = 0;

    //printf("Traceroute Analyze Single responsse %p\n", rptr);

    // if the pointer was NULL.. lets just return with 0 (no error...)
    if (rptr == NULL) return ret;

    qptr = TracerouteQueueFindByIdentifier(ctx, rptr->identifier);
    
    // we had a match.. lets link it in the spider web
    if (qptr != NULL) {
        // if this hop responding matches an actual target in the queue... then that traceroute is completed
        if ((rptr->hop_ip && rptr->hop_ip == qptr->target_ip) ||
            (!rptr->hop_ip && !qptr->target_ip && CompareIPv6Addresses(&rptr->hop_ipv6, &qptr->target_ipv6))) {

                //printf("------------------\nTraceroute completed %p [%u %u]\n-------------------\n", qptr, rptr->hop, qptr->target);
                // normal non randomized traceroute TTLs we just mark it as completed
                //qptr->completed = 1;

                //  If we are doing TTLs in random order rather than incremental.. then lets enumerate over all of the ttls for this queue
                for (i = 0; i < qptr->max_ttl; i++) {
                    // if a TTL is higher than the current then it should be disqualified..
                    if (qptr->ttl_list[i] >= rptr->ttl) qptr->ttl_list[i] = 0;
                    // and while we are at it.. lets count how  many is left.. (TTLs to send packets for)
                    if (qptr->ttl_list[i] != 0) left++;
                }

                // if all TTLs were already used.. then its completed
                if (!left) qptr->completed = 1;
        }

        // allocate space for us to append this response to our interal spider web for analysis
        if ((snew = (TracerouteSpider *)calloc(1, sizeof(TracerouteSpider))) != NULL) {
            // we need to know which TTL later (for spider web analysis)
            snew->ttl = rptr->ttl;

            // ensure we log identifier so we can connect all for target easily
            snew->identifier_id = qptr->identifier;

            // take note of the hops address (whether its ipv4, or 6)
            snew->hop_ip = rptr->hop_ip;
            CopyIPv6Address(&snew->hop_ipv6, &rptr->hop_ipv6);

            // take note of the target ip address
            snew->target_ip = qptr->target_ip;
            CopyIPv6Address(&qptr->target_ipv6, &rptr->target_ipv6);

            // in case later we wanna access the original queue which created the entry
            snew->queue = qptr;
            
            // link into list containing all..
            L_link_ordered_offset((LINK **)&ctx->traceroute_spider, (LINK *)snew, offsetof(TracerouteSpider, next));


            //Traceroute_Insert(ctx, snew);
            /*
            // now lets link into 'hops' list.. all these variations are required for final strategy
            if ((sptr = Spider_Find(ctx, snew->hop_ip, &snew->hop_ipv6)) != NULL) {
                //printf("--------------\nFound Spider %p [%u] branches %d\n", sptr->hop, snew->hop, branch_count(sptr->branches));

                // we found it as a spider.. so we can add it as a BRANCH to a hop (the router which responded is already listed)
                L_link_ordered_offset((LINK **)&sptr->branches, (LINK *)snew, offsetof(TracerouteSpider, branches));
            } else {
                // we couldnt find this hop/router so we add it as new
                L_link_ordered_offset((LINK **)&ctx->traceroute_spider_hops, (LINK *)snew, offsetof(TracerouteSpider, hops_list));
            }

            // link with other from same traceroute queue (by its identifier ID)...
            // this is another dimension of the strategy.. required .. branches of a single hop wasnt enough
            Spider_IdentifyTogether(ctx, snew);
*/

            Traceroute_Insert(ctx, snew);

            Spider_IdentifyTogether(ctx, snew);

            // log for watchdog to adjust traffic speed
            Traceroute_Watchdog_Add(ctx);

            ret = 1;
        }
    }

    end:;
    return ret;
}



int Traceroute_Insert(AS_context *ctx, TracerouteSpider *snew) {
    int ret = -1;
    int i = 0;
    TracerouteSpider *sptr = NULL, *snext = NULL, *slast = NULL;
    TracerouteSpider *search = NULL;

    /*
    int a = 0;
    int b =  0;
    int jtable = 0, jtable_enum = 0;
    TracerouteSpider *jptr = NULL;

    b = (snew->hop_ip & 0x0000ff00) >> 8;
    a = (snew->hop_ip & 0x000000ff);


    jtable = (a * (256*2));
    jtable += b;

    jptr = (TracerouteSpider *)ctx->jump_table[jtable];

    if (jptr != NULL) 
        sptr = jptr;
    else
        sptr = ctx->traceroute_spider_hops;
*/
    sptr = ctx->traceroute_spider_hops;

    // check if hop exist...
    if (sptr == NULL) {
        ctx->traceroute_spider_hops = snew;
    } else {
        while (sptr != NULL) {

            i = IPv4_compare(sptr->hop_ip, snew->hop_ip);

            //printf("IPv4_compare: %u %u -> %d [%p %p]\n", sptr->hop_ip, snew->hop_ip, i, sptr, snew);

            //printf("slast %p last->next %p sptr %p, sptr->next %p\n", slast, slast ? slast->next : "null", sptr, sptr ? sptr->next : "null");

            if (i == 0) {
                // add as branch (already here)
                //L_link_ordered_offset((LINK **)&sptr->branches, (LINK *)snew, offsetof(TracerouteSpider, branches));
                snew->branches = sptr->branches;
                sptr->branches = snew;
                //printf("Added branch\n");
                break;
            } else if (i == 1) {
                if (slast != NULL) {
                    snew->hops_list = slast->hops_list;
                    slast->hops_list = snew;
                } else {
                    if (ctx->traceroute_spider_hops == sptr) {
                        snew->hops_list = ctx->traceroute_spider_hops;
                        ctx->traceroute_spider_hops = snew;
                    }
                }

                break;
            } else if (i == -1) {
                if (ctx->traceroute_spider_hops == sptr) {
                    snew->hops_list = ctx->traceroute_spider_hops;

                    ctx->traceroute_spider_hops = snew;

                } else {

                    if (slast != NULL) {
                        snew->hops_list = slast->hops_list;
                        slast->hops_list = snew;
                    }

                }
                break;
            }

            slast = sptr;
            sptr = sptr->next;
        }
    }

    ret = 1;

    // lets setup jump table if its less than the one for this...
    // since its in order itll help find..

    end:;
    return ret;
}



// Queue an address for traceroute analysis/research
int Traceroute_Queue(AS_context *ctx, uint32_t target, struct in6_addr *targetv6) {
    TracerouteQueue *tptr = NULL;
    int ret = -1;
    int i = 0;
    int n = 0;
    int ttl = 0;
    struct in_addr addr;

    addr.s_addr = target;
    //printf("\nTraceroute Queue %u: %s\n", target, inet_ntoa(addr));

    // allocate memory for this new traceroute target we wish to add into the system
    if ((tptr = (TracerouteQueue *)calloc(1, sizeof(TracerouteQueue))) == NULL) goto end;

    // which IP are we performing traceroutes on
    tptr->target_ip = target;


    // if its an ipv6 addres pasased.. lets copy it (this function will verify its not NULL)
    CopyIPv6Address(&tptr->target_ipv6, targetv6);

    // we start at ttl 1.. itll inncrement to that when processing
    tptr->current_ttl = 0;

    // create a random identifier to find this packet when it comes from many hops
    tptr->identifier = rand()%0xFFFFFFFF;

    // later we wish to allow this to be set by scripting, or this function
    // for example: if we wish to find close routes later to share... we can set to max = 5-6
    // and share with p2p nodes when mixing/matching sides of the taps (when they decide to secure them more)
    tptr->max_ttl = MAX_TTL;

    // current timestamp stating it was added at this time
    tptr->ts = time(0);

    // add to traceroute queue...
    //L_link_ordered_offset((LINK **)&ctx->traceroute_queue, (LINK *)tptr, offsetof(TracerouteQueue, next));
    tptr->next = ctx->traceroute_queue;
    ctx->traceroute_queue = tptr;

    // enable default TTL list
    for (i = ctx->traceroute_min_ttl; i < MAX_TTL; i++) tptr->ttl_list[i] = (i - ctx->traceroute_min_ttl);

    // randomize those TTLs
    RandomizeTTLs(tptr);

    end:;
    return ret;
}


// When we initialize using Traceroute_Init() it added a filter for ICMP, and set this function
// as the receiver for any packets seen on the wire thats ICMP
int Traceroute_Incoming(AS_context *ctx, PacketBuildInstructions *iptr) {
    int ret = -1;
    struct in_addr cnv;
    TracerouteResponse *rptr = NULL;
    TraceroutePacketData *pdata = NULL;

    // when we extract the identifier from the packet.. put it here..
    uint32_t identifier = 0;
    // ttl has to be extracted as well (possibly from the identifier)
    int ttl = 0;

    //printf("incoming\n");

    if (iptr->source_ip && (iptr->source_ip == ctx->my_addr_ipv4)) {
        //printf("ipv4 Getting our own packets.. probably loopback\n");
        return 0;
    }

    if (!iptr->source_ip && CompareIPv6Addresses(&ctx->my_addr_ipv6, &iptr->source_ipv6)) {
        //printf("ipv6 Getting our own packets.. probably loopback\n");
        return 0;
    }
        
    // data isnt big enough to contain the identifier
    if (iptr->data_size < sizeof(TraceroutePacketData)) goto end;

    // the responding hops may have encapsulated the original ICMP within its own.. i'll turn the 28 into a sizeof() calculation
    // ***
    if (iptr->data_size > sizeof(TraceroutePacketData) && ((iptr->data_size >= sizeof(TraceroutePacketData) + 28)))
        pdata = (TraceroutePacketData *)(iptr->data + 28);//(sizeof(struct iphdr) + sizeof(struct icmphdr)));
    else
        pdata = (TraceroutePacketData *)iptr->data;

    /*
    printf("Got packet from network! data size %d\n", iptr->data_size);
    //printf("\n\n---------------------------\nTraceroute Incoming\n");

    cnv.s_addr = iptr->source_ip;
    printf("SRC: %s %u\n", inet_ntoa(cnv), iptr->source_ip);

    cnv.s_addr = iptr->destination_ip;
    printf("DST: %s %u\n", inet_ntoa(cnv), iptr->destination_ip);
    */

    // the packet has the TTL, and the identifier (to find the original target information)
    ttl = pdata->ttl;
    identifier = pdata->identifier;

    // this function is mainly to process quickly.. so we will fill another structure so that it can get processed
    // later again with calculations directly regarding its query

    // allocate a new structure for traceroute analysis functions to deal with it later
    if ((rptr = (TracerouteResponse *)calloc(1, sizeof(TracerouteResponse))) == NULL) goto end;
    rptr->identifier = identifier;
    rptr->ttl = ttl;
    
    // copy over IP parameters
    rptr->hop_ip = iptr->source_ip;
    CopyIPv6Address(&rptr->hop_ipv6, &iptr->source_ipv6);

    // maybe lock a mutex here (have 2... one for incoming from socket, then moving from that list here)
    L_link_ordered_offset((LINK **)&ctx->traceroute_responses, (LINK *)rptr, offsetof(TracerouteResponse, next));

    // thats about it for the other function to determine the original target, and throw it into the spider web
    ret = 1;

    end:;

    // iptr gets freed in the calling function
    return ret;
}


static int ccount = 0;


// iterate through all current queued traceroutes handling whatever circumstances have surfaced for them individually
// todo: allow doing random TTLS (starting at 5+) for 10x... most of the end hosts or hops will respond like this
// we can prob accomplish much more
int Traceroute_Perform(AS_context *ctx) {
    TracerouteQueue *tptr = ctx->traceroute_queue;
    TracerouteResponse *rptr = ctx->traceroute_responses, *rnext = NULL;
    struct icmphdr icmp;
    PacketBuildInstructions *iptr = NULL;
    AttackOutgoingQueue *optr = NULL;
    int i = 0, n = 0;
    TraceroutePacketData *pdata = NULL;
    TracerouteSpider *sptr = NULL;
    int tcount  = 0;
    int ret = 0;
    // timestamp required for various states of traceroute functionality
    int ts = time(0);

    // if the list is empty.. then we are done here
    if (tptr == NULL) goto end;

    // zero icmp header since its in the stack
    memset(&icmp, 0, sizeof(struct icmphdr));


    printf("Traceroute_Perform: Queue %d [completed %d] max: %d\n", L_count((LINK *)tptr), Traceroute_Count(ctx, 1, 1), ctx->traceroute_max_active);

    // loop until we run out of elements
    while (tptr != NULL) {        
        // if we have reached max ttl then mark this as completed.. otherwise it could be marked completed if we saw a hop which equals the target
        if (tptr->current_ttl >= tptr->max_ttl) {
            tptr->completed = 1;
        }

        if (!tptr->completed && tptr->enabled) {
            // lets increase the TTL by this number (every 1 second right now)
            if ((ts - tptr->ts_activity) > 1) {
                tptr->ts_activity = time(0);

                // increase TTL in case this one is rate limiting ICMP, firewalled, or whatever.. move to the next
                tptr->current_ttl++;

                // in case we have more in a row for this queue that are 0 (because it responded fromm a higher ttl already)
                // and we just need its lower hops...
                // this is mainly when we are randomizing the TTL so our closer routes arent getting all 50 at once..                
                while ((tptr->current_ttl < tptr->max_ttl) && tptr->ttl_list[tptr->current_ttl] == 0) {
                    // disqualify this ttl before we move to the next..
                    // this is for easily enabling/disabling large amounts of queues so we can
                    // attempt to scan more simultaneously (and retry easily for largee amounts)

                    // its already 0 (in the while &&)
                    //tptr->ttl_list[tptr->current_ttl] = 0;

                    tptr->current_ttl++;
                }

                // if the current TTL isnt disqualified already
                if (tptr->ttl_list[tptr->current_ttl] != 0) {
                    // prepare the ICMP header for the traceroute
                    icmp.type = ICMP_ECHO;
                    icmp.code = 0;
                    icmp.un.echo.sequence = tptr->identifier;
                    icmp.un.echo.id = tptr->identifier + tptr->ttl_list[tptr->current_ttl];

                    // create instruction packet for the ICMP(4/6) packet building functions
                    if ((iptr = (PacketBuildInstructions *)calloc(1, sizeof(PacketBuildInstructions))) != NULL) {

                        // this is the current TTL for this target
                        iptr->ttl = tptr->ttl_list[tptr->current_ttl];
                        
                        // determine if this is an IPv4/6 so it uses the correct packet building function
                        if (tptr->target_ip != 0) {
                            iptr->type = PACKET_TYPE_ICMP_4;
                            iptr->destination_ip = tptr->target_ip;
                            iptr->source_ip = ctx->my_addr_ipv4;
                        } else {
                            iptr->type = PACKET_TYPE_ICMP_6;
                            // destination is the target
                            CopyIPv6Address(&iptr->destination_ipv6, &tptr->target_ipv6);
                            // source is our ip address
                            CopyIPv6Address(&iptr->source_ipv6, &ctx->my_addr_ipv6);
                        }

                        // copy ICMP parameters into this instruction packet as a complete structure
                        memcpy(&iptr->icmp, &icmp, sizeof(struct icmphdr));

                        // set size to the traceroute packet data structure's size...
                        iptr->data_size = sizeof(TraceroutePacketData);

                        if ((iptr->data = (char *)calloc(1, iptr->data_size)) != NULL) {
                            pdata = (TraceroutePacketData *)iptr->data;

                            // lets include a little message since we are performing a lot..
                            // if ever on a botnet, or worm.. disable this obviously
                            strcpy(&pdata->msg, "performing traceroute research");
                            
                            // set the identifiers so we know which traceroute queue the responses relates to
                            pdata->identifier = tptr->identifier;
                            pdata->ttl = iptr->ttl;
                        }

                        // lets build a packet from the instructions we just designed for either ipv4, or ipv6
                        // for either ipv4, or ipv6
                        if (iptr->type & PACKET_TYPE_ICMP_6)
                            i = BuildSingleICMP6Packet(iptr);
                        else if (iptr->type & PACKET_TYPE_ICMP_4)
                            i = BuildSingleICMP4Packet(iptr);

                        // if the packet building was successful
                        if (i == 1) {
                            // allocate a structure for the outgoing packet to get wrote to the network interface
                            if ((optr = (AttackOutgoingQueue *)calloc(1, sizeof(AttackOutgoingQueue))) != NULL) {
                                // we need to pass it the final packet which was built for the wire
                                optr->buf = iptr->packet;
                                optr->type = iptr->type;
                                optr->size = iptr->packet_size;

                                // remove the pointer from the instruction structure so it doesnt get freed in this function
                                iptr->packet = NULL;
                                iptr->packet_size = 0;

                                // the outgoing structure needs some other information
                                optr->dest_ip = iptr->destination_ip;
                                optr->ctx = ctx;

                                // if we try to lock mutex to add the newest queue.. and it fails.. lets try to pthread off..
                                if (AttackQueueAdd(ctx, optr, 1) == 0) {
                                    // create a thread to add it to the network outgoing queue.. (brings it from 4minutes to 1minute) using a pthreaded outgoing flusher
                                    if (pthread_create(&optr->thread, NULL, AS_queue_threaded, (void *)optr) != 0) {
                                        // if we for some reason cannot pthread (prob memory).. lets do it blocking
                                        AttackQueueAdd(ctx, optr, 0);
                                    }
                                }
                            }
                        }
                        // dont need this anymore.. (we removed the data pointer from it so lets just clear everyting else)
                        PacketBuildInstructionsFree(&iptr);
                    }
                }
            }
        }

        tptr = tptr->next;
    }

    // now process all queued responses we have from incoming network traffic.. it was captured on a thread specifically for reading packets
    rptr = ctx->traceroute_responses;

    // loop until all responsses have been analyzed
    while (rptr != NULL) {
        // call this function which will take care of the response, and build the traceroute spider for strategies
        TracerouteAnalyzeSingleResponse(ctx, rptr);

        // get pointer to next so we have it after freeing
        rnext = rptr->next;

        // free this response structure..
        free(rptr);

        // move to next
        rptr = rnext;
    }

    // we cleared the list so ensure the context is updated
    ctx->traceroute_responses = NULL;

    // *** we will log to the disk every 20 calls (for dev/debugging)
    // moved to every 40 because the randomly disabling and enabling should be more than MAX_TTL at least
    if ((ccount++ % 40)==0) {
        Spider_Save(ctx);

        // lets randomly enable or disable queues.... 20% of the time we reach this..
        if ((rand()%100) < 20)
            Traceroute_AdjustActiveCount(ctx);
    }

    // count how many traceroutes are in queue and active
    tcount = Traceroute_Count(ctx, 0, 0);

    // if the amount of active is lower than our max, then we will activate some other ones
    if (tcount < ctx->traceroute_max_active) {
        
        // how many to ativate?
        tcount = ctx->traceroute_max_active - tcount;
    
        // start on the  linked list...enumerating each
        tptr = ctx->traceroute_queue;
        while (tptr != NULL) {
            // ensure this one isnt completed, and isnt already enabled..
            if (!tptr->completed && !tptr->enabled) {
                // if we already activated enough then we are done
                if (!tcount) break;

                // activate this particular traceroute target
                tptr->enabled = 1;

                // decrease the coutner
                tcount--;
            }
            // move to the next target
            tptr = tptr->next;
        }
    }

    // do we adjust max active?
    Traceroute_Watchdog(ctx);

    end:;

    return ret;
}



// dump traceroute data to disk.. printing a little information..
// just here temporarily.. 
int Spider_Save(AS_context *ctx) {
    TracerouteSpider *sptr = NULL;
    int count = 0;
    FILE *fd = NULL;
    FILE *fd2 = NULL;
    char fname[32];
    TracerouteSpider *bptr = NULL;
    char Ahop[16], Atarget[16];
    struct in_addr conv;
    TracerouteQueue *qptr = NULL;

    // open file for writing traceroute queues...
    sprintf(fname, "traceroute_queue.txt", "w");
    fd = fopen(fname, "w");

    // filename for debug data
    sprintf(fname, "traceroute.txt", "w");
    fd2 = fopen(fname, "w");
    //fd2 = NULL; // disabling it by settinng to NULL

    // dump all traceroute queues and their identifiers
    qptr = ctx->traceroute_queue;
    while (qptr != NULL) {

        if (fd) {
            conv.s_addr = qptr->target_ip;            
            strcpy((char *)&Atarget, inet_ntoa(conv));

            fprintf(fd, "QUEUE,%s,%u,%d,%d,%d,%d,%d\n", Atarget,
                qptr->identifier, qptr->retry_count, qptr->completed,
                qptr->enabled, qptr->ts_activity, qptr->ts);

        }

        qptr = qptr->next;
    }

    // enumerate spider and list information
    sptr = ctx->traceroute_spider_hops;
    while (sptr != NULL) {
        // if the output file is open then lets write some data
        if (fd2) {
            // we wanna turn the target, and hop IP from long to ascii
            conv.s_addr = sptr->hop_ip;
            strcpy((char *)&Ahop, inet_ntoa(conv));
            conv.s_addr = sptr->target_ip;
            strcpy((char *)&Atarget, inet_ntoa(conv));
            // and finally format & write it to the output file
            fprintf(fd2, "HOP,%s,%s,%u,%d\n", Ahop, Atarget, sptr->identifier_id, sptr->ttl);
        }

        // this message is for debugging/development.. how many branches are in this hop (similar)
        count = L_count_offset((LINK *)sptr->branches, offsetof(TracerouteSpider, branches));
        //count = branch_count(sptr->branches);
        // we want only if its more than 10 to show to the sccreen because a lot are small numbers (1-10)
        if (count > 10) {
            printf("spider hop %u branches %p [count %d] next %p\n", sptr->hop_ip, sptr->branches, count, sptr->hops_list);
        }

        // if this particular hop has other branches (relations to other traceroute queries/targets)
        // then we wanna log those to the file as well
        bptr = sptr->branches;

        // loop for each branch
        while (bptr != NULL) {
            // if the file is open
            if (fd2) {
                // convert long ips to ascii
                conv.s_addr = bptr->hop_ip;
                strcpy((char *)&Ahop, inet_ntoa(conv));
                conv.s_addr = bptr->target_ip;
                strcpy((char *)&Atarget, inet_ntoa(conv));
                // and format & write the data to the file
                fprintf(fd2, "BRANCH,%s,%s,%u,%d\n", Ahop, Atarget, sptr->identifier_id, sptr->ttl);
            }

            // move to next in branch list
            bptr = bptr->branches;
        }

        // move to next in hop list (routers which have resppoonded to traceroute queries)
        sptr = sptr->hops_list;

        //printf("L2 %p\n", sptr);
    }

    // how many traceroute hops do we have? (unique.. dont count branches)
    // *** fix this.. we neeed an L_count() for _offset() because this will count the total fromm the first element
    printf("Traceroute Spider count: %d\n", L_count_offset((LINK *)ctx->traceroute_spider_hops, offsetof(TracerouteSpider, hops_list)));
    printf("Traceroute total count: %d\n", L_count((LINK *)ctx->traceroute_spider));

    // close file if it was open
    if (fd) fclose(fd);
    if (fd2) fclose(fd2);

    return 0;
}


int IPv4_compare(uint32_t comp, uint32_t ipv4) {
    struct  in_addr addr;
    char Aip[16];
    int a=0,b=0,c=0,d=0;
    int a2=0,b2=0,c2=0,d2=0;

    d = (comp & 0xff000000) >> 24;
    c = (comp & 0x00ff0000) >> 16;
    b = (comp & 0x0000ff00) >> 8;
    a = (comp & 0x000000ff);

    d2 = (ipv4 & 0xff000000) >> 24;
    c2 = (ipv4 & 0x00ff0000) >> 16;
    b2 = (ipv4 & 0x0000ff00) >> 8;
    a2 = (ipv4 & 0x000000ff);

    if (a2 < a) return -1;
    if (a2 > a) return 1;

    if (b2 < b) return -1;
    if (b2 > b) return 1;

    if (c2 < c) return -1;
    if (c2 > c) return 1;

    if (d2 < d) return -1;
    if (d2 > d) return 1;

    if (d2 == d) return 0;
}


// lets see if we have a hop already in the spider.. then we just add this as a branch of it
// *** verify IPv6 (change arguments.. use IP_prepare)
TracerouteSpider *Spider_Find(AS_context *ctx, uint32_t hop, struct in6_addr *hopv6) {
    TracerouteSpider *sptr = ctx->traceroute_spider_hops;
    // enumerate through spider list tryinig to find a hop match...
    // next we may want to add looking for targets.. but in general
    // we will wanna analyze more hop informatin about a target ip
    // as its hops come back.. so im not sure if its required yet
    while (sptr != NULL) {
        //printf("hop %u sptr %u\n", hop, sptr->hop);
        if (hop && sptr->hop_ip == hop) {
                break;
        } else {
            //if (!hop && CompareIPv6Addresses(&sptr->hopv6, hopv6))  break;
        }

        sptr = sptr->hops_list;
    }

    return sptr;
}



// load data from a file.. this is for development.. so I can use the python interactive debugger, and write various C code
// for the algorithms required to determine the best IP addresses for manipulation of the mass surveillance networks
int Spider_Load(AS_context *ctx, char *filename) {
    FILE *fd = NULL, *fd2 = NULL;
    char buf[1024];
    char *sptr = NULL;
    char type[16], hop[16],target[16];
    int ttl = 0;
    uint32_t identifier = 0;
    int ts =0, enabled = 0, activity = 0, completed = 0, retry = 0;
    int i = 0;
    int n = 0;
    TracerouteSpider *Sptr = NULL;
    TracerouteSpider *snew = NULL;
    TracerouteSpider *slast = NULL, *Blast = NULL;
    TracerouteQueue *qnew = NULL;
    char fname[32];

    // traceroute responses (spider)
    sprintf(fname, "%s.txt", filename);
    // open ascii format file
    if ((fd = fopen(fname, "r")) == NULL) goto end;

    // traceroute queue
    sprintf(fname, "%s_queue.txt", filename);
    // open ascii format file
    if ((fd2 = fopen(fname, "r")) == NULL) goto end;



    // read all lines
    while (fgets(buf,1024,fd2)) {
        i = 0;
        // if we have \r or \n in the buffer (line) we just read then lets set it to NULL
        if ((sptr = strchr(buf, '\r')) != NULL) *sptr = 0;
        if ((sptr = strchr(buf, '\n')) != NULL) *sptr = 0;

        // change all , (like csv) to " " spaces for sscanf()
        n = strlen(buf);
        while (i < n) {
            if (buf[i] == ',') buf[i] = ' ';
            i++;
        }

        // grab entries
        sscanf(buf, "%s %s %"SCNu32" %d %d %d %d %d", &type, &target, &identifier,
         &retry, &completed, &enabled, &activity, &ts);

        if ((qnew = (TracerouteQueue *)calloc(1, sizeof(TracerouteQueue))) == NULL) break;

        // set parameters from data file
        qnew->completed = completed;
        qnew->retry_count = retry;
        qnew->ts = ts;
        qnew->ts_activity = activity;

        // we wanna control enabled here when we look for all TTL hops
        qnew->enabled = 0;//enabled;

        qnew->target_ip = inet_addr(target);
        qnew->identifier = identifier;

        // set all TTLs in the list to their values
        // ***
        // turn randomizing into its own function later and set here..
        //for (n = 0; n < MAX_TTL; n++) qnew->ttl_list[n] = n;
        //qnew->max_ttl = MAX_TTL;


        qnew->next = ctx->traceroute_queue;
        ctx->traceroute_queue = qnew;
    }

    // first we load the traceroute responses.. so we can use the list for re-enabling ones which were  in progress
    while (fgets(buf,1024,fd)) {
        i = 0;
        // if we have \r or \n in the buffer (line) we just read then lets set it to NULL
        if ((sptr = strchr(buf, '\r')) != NULL) *sptr = 0;
        if ((sptr = strchr(buf, '\n')) != NULL) *sptr = 0;

        // change all , (like csv) to " " spaces for sscanf()
        n = strlen(buf);
        while (i < n) {
            if (buf[i] == ',') buf[i] = ' ';
            i++;
        }

        // grab entries
        sscanf(buf, "%s %s %s %"SCNu32" %d", &type, &hop, &target, &identifier, &ttl);

        //printf("type: %s\nhop %s\ntarget %s\nident %X\nttl %d\n", type, hop,target, identifier, ttl);

        // allocate structure for storing this entry into the traceroute spider
        if ((snew = (TracerouteSpider *)calloc(1, sizeof(TracerouteSpider))) == NULL) break;

        // set various information we have read fromm the file into the new structure
        snew->hop_ip = inet_addr(hop);
        snew->target_ip = inet_addr(target);
        snew->ttl = ttl;
        snew->identifier_id = identifier;

        // add to main linked list.. (where every entry goes)
        // use last so its faster..
        if (slast == NULL) {
            L_link_ordered_offset((LINK **)&ctx->traceroute_spider, (LINK *)snew, offsetof(TracerouteSpider, next));
            slast = snew;
        } else {
            slast->next = snew;
            slast = snew;
        }

        Traceroute_Insert(ctx, snew);
        Spider_IdentifyTogether(ctx, snew);

        // before we fgets() again lets clear the buffer
        // *** had a weird bug with sscanf.. im pretty sure this is useless but it began working duringn 4-5 changes..
        // ill rewrite this entire format binary soon anyways.
        memset(buf,0,1024);
    }



    //printf("calling Traceroute_RetryAll to deal with loaded data\n");
    Traceroute_RetryAll(ctx);

    end:;

    if (fd) fclose(fd);
    if (fd2) fclose(fd2);

    return 1;
}


//http://www.binarytides.com/get-local-ip-c-linux/
uint32_t get_local_ipv4() {
    const char* google_dns_server = "8.8.8.8";
    int dns_port = 53;
    uint32_t ret = 0;
    struct sockaddr_in serv;     
    int sock = 0;
    
    //return inet_addr("192.168.0.100");
    
    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) return 0;
     
    memset( &serv, 0, sizeof(serv) );
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = inet_addr( google_dns_server );
    serv.sin_port = htons( dns_port );
 
    int err = connect( sock , (const struct sockaddr*) &serv , sizeof(serv) );
     
    struct sockaddr_in name;
    socklen_t namelen = sizeof(name);
    err = getsockname(sock, (struct sockaddr*) &name, &namelen);
         
    ret = name.sin_addr.s_addr;

    close(sock);
     
    return ret;
}

// fromm //http://www.binarytides.com/get-local-ip-c-linux/ as above..
// but modified for ipv6
void get_local_ipv6(struct in6_addr *dst) {
    const char* google_dns_server = "2001:4860:4860::8888";
    int dns_port = 53;
    uint32_t ret = 0;

    struct sockaddr_in6 serv;

    int sock = socket ( AF_INET6, SOCK_DGRAM, 0);
     if (sock < 0) return 0;

    memset( &serv, 0, sizeof(serv) );
    serv.sin6_family = AF_INET6;
    inet_pton(AF_INET6, google_dns_server, &serv.sin6_addr);
    serv.sin6_port = htons( dns_port );

    int err = connect( sock , (const struct sockaddr*) &serv , sizeof(serv) );

    struct sockaddr_in6 name;
    socklen_t namelen = sizeof(name);
    err = getsockname(sock, (struct sockaddr*) &name, &namelen);

    memcpy(dst, &name.sin6_addr, sizeof(struct in6_addr));

    close(sock);

}


// initialize traceroute research subsystem
// this has to prepare the incoming packet filter, and structure so we get iniformation from the wire
int Traceroute_Init(AS_context *ctx) {
    NetworkAnalysisFunctions *nptr = NULL;
    FilterInformation *flt = NULL;
    int ret = -1;

    flt = (FilterInformation *)calloc(1, sizeof(FilterInformation));
    nptr = (NetworkAnalysisFunctions *)calloc(1, sizeof(NetworkAnalysisFunctions));

    if (!flt || !nptr) goto end;

    // lets just ensure we obtain all ICMP packet information since we'll have consistent ipv4/ipv6 requests happening
    FilterPrepare(flt, FILTER_PACKET_ICMP, 0);

    // prepare structure used by the network engine to ensure w get the packets we are looking for
    nptr->incoming_function = &Traceroute_Incoming;
    nptr->flt = flt;

    // insert so the network functionality will begin calling our function for these paackets
    nptr->next = ctx->IncomingPacketFunctions;
    ctx->IncomingPacketFunctions = nptr;
    
    // get our own ip addresses for packet building
    ctx->my_addr_ipv4 = get_local_ipv4();
    get_local_ipv6(&ctx->my_addr_ipv6);

    // max of 20 retries for traceroute
    // it retries when all queues are completed..
    // the data is extremely important to ensure attacks are successful
    // especially if we wanna do the most damage from a single node
    // distributed? good luck.
    ctx->traceroute_max_retry = 20;
    // start at ttl 8 (need to change this dynamically from script) doing it manually for research
    ctx->traceroute_min_ttl = 0;

    // how many active at all times? we set it here so we can addjust it in watchdog later...
    ctx->traceroute_max_active = 50;

    ret = 1;

    end:;

    // free structures if they were not used for whatever reasons
    if (ret != 1) {
        PtrFree(&flt);
        PtrFree(&nptr);
    }

    return ret;
}



// count the amount of non completed (active) traceroutes in queue
int Traceroute_Count(AS_context *ctx, int return_completed, int count_disabled) {
    TracerouteQueue *qptr = ctx->traceroute_queue;
    int ret = 0;
    //int pass = 0;

    // loop until we enuerate the entire list of queued traceroute activities
    while (qptr != NULL) {

        // ** rewrite this logic... to deall w all flags correctly.. I kept adding them instantly..
        // not logically..

        // check if they are completed.. if not we increase the counter
        if (!return_completed && !qptr->completed) {
            if (count_disabled && !qptr->enabled) ret++;

            if (!count_disabled && qptr->enabled) ret++;
        }

        if (return_completed && qptr->completed) {
            ret++;
        }

        qptr = qptr->next;
    }

    return ret;
}


// find a traceroute structure by address.. and maybe check ->target as well (traceroute queue IP as well as hops)
TracerouteSpider *Traceroute_Find(AS_context *ctx, uint32_t address, struct  in6_addr *addressv6, int check_targets) {
    TracerouteSpider *ret = NULL;
    TracerouteSpider *sptr = ctx->traceroute_spider;
    struct in_addr src;

    while (sptr != NULL) {

        // for turning long IP to ascii for dbg msg
        //src.s_addr = sptr->hop_ip;
        //printf("FIND checking against IP: %s\n", inet_ntoa(src));

        // ***maybe create an address structuure which can hold IPv4, and 6 and uses an integer so we dont just check if ipv4 doesnt exist..
        if (address && sptr->hop_ip == address) {
            //printf("Checking %u against %u", address, sptr->hop);
            break;
        }
        if (!address && CompareIPv6Addresses(addressv6, &sptr->hop_ipv6)) {
            break;
        }

        // if it passed a flag to check targets.. then we arent only checking routers (hops)
        // we will match with anything relating to that target
        if (check_targets && address && sptr->target_ip == address)
            break;

        if (check_targets && !address && CompareIPv6Addresses(addressv6, &sptr->target_ipv6))
            break;

        // move to the next in the list to check for the address
        sptr = sptr->next;
    }

    return sptr;

}

// we call this to let the watchdog know that some incoming traceroute was successful
void Traceroute_Watchdog_Add(AS_context *ctx) {
    int i = 0;
    int ts = time(0);

    // which part of array to use? we loop back around if over max with %1024
    i = ctx->Traceroute_Traffic_Watchdog.HistoricRawCurrent % (1024*10);

    // parameters
    ctx->Traceroute_Traffic_Watchdog.HistoricDataRaw[i].ts = ts;
    ctx->Traceroute_Traffic_Watchdog.HistoricDataRaw[i].count=1;
    ctx->Traceroute_Traffic_Watchdog.HistoricDataRaw[i].max_setting = ctx->traceroute_max_active;

    // thats all.. simple.. increase counter
    ctx->Traceroute_Traffic_Watchdog.HistoricRawCurrent++;
}




// monitors historic amount of queries that we get so we can adjust
// the active amount of traceroutes we are performing automatically for
// highest speed possible
int Traceroute_Watchdog(AS_context *ctx) {
    int ret = 0;
    int i = 0;
    TraceroutePerformaceHistory *hptr = &ctx->Traceroute_Traffic_Watchdog;
    int total = 0;
    int interval_seconds = 10;
    int ts = time(0); // current time for calculations

    int historic_count = 0;
    int interval_sum = 0;
    int interval_max = 0;
    int prior_ts = 0;
    int which = 0;
    int up = 0, down = 0;
    int total_historic_to_use = 4;
    CountElement *cptr[total_historic_to_use+1];
    
    float perc_change;
    int historic_avg_increase = 0;


    i = Traceroute_Count(ctx, 0, 0);

    if (i == 0) return 0;

    // if there arent any entries then there is nothing to do
    if (hptr->HistoricRawCurrent == 0) {
        //printf("no historic\n");
        return 0;
    }

    for (i = 0; i < (1024*10); i++) {
        if ((ts - hptr->HistoricDataRaw[i].ts) > interval_seconds) {
            interval_sum += hptr->HistoricDataRaw[i].count;
            interval_max = hptr->HistoricDataRaw[i].max_setting;
        }
    }

    // interval sum contains the amount within the last X seconds at any given moment
    // now we would like to know between a specific time period

    // on the second entry we know when the first waas calculated
    // so we can automatically choose that minute:second and start all of our interval
    // counts using that as a reference... but how to set the first?

    if (hptr->HistoricCurrent) {
        prior_ts = hptr->HistoricDataCalculated[ctx->Traceroute_Traffic_Watchdog.HistoricCurrent - 1].ts;
    }

    // prior_ts would be 0 here.. so it will trigger on the first...
    if ((ts - prior_ts) < interval_seconds) {
        //printf("not time ts %d prior %d .. interval %d [%d]\n", ts, prior_ts, interval_seconds, (ts-prior_ts));
        return 0;
    }

    // lets log...
    hptr->HistoricDataCalculated[hptr->HistoricCurrent].count = interval_sum;
    hptr->HistoricDataCalculated[hptr->HistoricCurrent].ts = ts;
    hptr->HistoricDataCalculated[hptr->HistoricCurrent].max_setting = ctx->traceroute_max_active;

    //printf("count: %d ts: %d max %d\n", interval_sum, ts, ctx->traceroute_max_active);

    // increase historic counter.. so we can keep track of more
    

    if (hptr->HistoricCurrent == (1024*10))
        hptr->HistoricCurrent = 0;

    // now we need to use the data we have to determine we wish to dynamically modify the max traceroute queue
    // we want at least 3 to attempt to modifhiy...
    if (hptr->HistoricCurrent >= total_historic_to_use) {

        if ((ts - ctx->watchdog_ts) < (60*2)) {
            return 0;
        }

        // get pointer to the last 3
        for (i = 0; i < total_historic_to_use; i++) {
            cptr[i] = (CountElement *)&ctx->Traceroute_Traffic_Watchdog.HistoricDataCalculated[ctx->Traceroute_Traffic_Watchdog.HistoricCurrent - (i+1)];

            // wait until they are all on same speed... (minute and a half at current setting)
            if (cptr[i]->max_setting != hptr->HistoricDataCalculated[hptr->HistoricCurrent - 1].max_setting) {
                //printf("dont have %d of same max\n", total_historic_to_use);
                goto end;
            }

            //printf("cptr[i].count = [%d, %d]\n", i, cptr[i]->count);
            //printf("Against %d\n", hptr->HistoricDataCalculated[hptr->HistoricCurrent].count);

            // lets get percentage change...
            perc_change = (float)((float)hptr->HistoricDataCalculated[hptr->HistoricCurrent].count / (float)cptr[i]->count);
            perc_change *= 100;

            //historic[i] /= 
            if (cptr[i]->count < hptr->HistoricDataCalculated[hptr->HistoricCurrent].count) {
                if (perc_change > 105) up++;
            } else {
                if (perc_change < 90) down++;
            }
        }


        if (up > down) {
            ctx->traceroute_max_active += 300;
        } else
        if (up < down) {
            ctx->traceroute_max_active -= 50;

        }   
        
        /*     if (good >= (total_historic_to_use/2)) ctx->traceroute_max_active += 300;
        //if (good == 1) ctx->traceroute_max_active += 50;
        else if (good > (total_historic_to_use/4)) ctx->traceroute_max_active += ((5+rand()%5) - rand()%10);
        
        else if (good <= (total_historic_to_use/4)) ctx->traceroute_max_active -= 50;
        */

        if (ctx->traceroute_max_active < 50) ctx->traceroute_max_active = 50;

        if (ctx->traceroute_max_active > 10000) ctx->traceroute_max_active = 10000;


        ctx->watchdog_ts = ts;

        //printf("up %d down %d and ret is %d\n", up, down, ret);

        ret = 1;

        if (ret == 1)
            // adjust active queue using the new setting
            Traceroute_AdjustActiveCount(ctx);
    }

    end:;
    hptr->HistoricCurrent++;
    return ret;
}

int TracerouteResetRetryCount(AS_context *ctx) {
    int ret = 0;
    TracerouteQueue *qptr = ctx->traceroute_queue;

    while (qptr != NULL) {
        if (qptr->retry_count) qptr->retry_count=0;

        ret++;

        qptr = qptr->next;
    }

    return ret;
}


