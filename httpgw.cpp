/*
 *  httpgw.cpp
 *  gateway for serving swift content via HTTP, libevent2 based.
 *
 *  Created by Victor Grishchenko, Arno Bakker
 *  Copyright 2010-2012 TECHNISCHE UNIVERSITEIT DELFT. All rights reserved.
 *
 */
#include "swift.h"
#include <event2/http.h>
#include <event2/bufferevent.h>
#include <sstream>

// Set to true for limited debug output instead of dprintf() full blast.
#define  http_debug	false

using namespace swift;

static uint32_t HTTPGW_VOD_PROGRESS_STEP_BYTES = (256*1024); // configurable
// For best performance make bigger than HTTPGW_PROGRESS_STEP_BYTES
#define HTTPGW_VOD_MAX_WRITE_BYTES	(512*1024)


#define HTTPGW_LIVE_PROGRESS_STEP_BYTES	(16*1024)
// For best performance make bigger than HTTPGW_PROGRESS_STEP_BYTES
#define HTTPGW_LIVE_MAX_WRITE_BYTES	(32*1024)


// Let swift report download progress every 2^layer * chunksize bytes (so 0 = report
// every chunk). Note: for LIVE this cannot be reliably used to force a
// prebuffer size, as this gets called when a subtree of size X has been
// downloaded. If the hook-in point is in the middle of such a subtree, the
// call won't happen until the next full subtree has been downloaded, i.e.
// after ~1.5 times the prebuf has been downloaded. See HTTPGW_MIN_PREBUF_BYTES
//
#define HTTPGW_FIRST_PROGRESS_BYTE_INTERVAL_AS_LAYER     0    // must be 0

// Arno: libevent2 has a liberal understanding of socket writability,
// that may result in tens of megabytes being cached in memory. Limit that
// amount at app level.
#define HTTPGW_MAX_OUTBUF_BYTES		(2*1024*1024)

// Arno: Minimum amout of content to download before replying to HTTP
static uint32_t HTTPGW_MIN_PREBUF_BYTES  = (256*1024); // configurable

const char *url_query_keys[] = { "v", "cp", "hf", "ca", "ld", "ia", "cs", "cl", "cd", "bt", "mt" };
#define NUM_URL_QUERY_KEYS 	11



#define HTTPGW_MAX_REQUEST 128

struct http_gw_t {
    int      id;	 // request id
    // request
    struct evhttp_request *sinkevreq;   // libevent HTTP request
    struct event          *sinkevwrite; // libevent HTTP socket writable event
    // what
    int      td;	 // transfer being served
    std::string mfspecname; // (optional) name from multi-file spec
    int64_t  rangefirst; // (optional) First byte wanted in HTTP GET Range request or -1 (relative to any mfspecname)
    int64_t  rangelast;  // (optional) Last byte wanted in HTTP GET Range request (also 99 for 100 byte interval) or -1
    // reply
    int      replycode;  // HTTP status code to return
    std::string xcontentdur;   // (optional) duration of content in seconds, -1 for live (for Firefox HTML5)
    std::string mimetype;      // MIME type to return
    bool     replied;    // Whether or not a reply has been sent on the HTTP request
    // reply progress
    uint64_t offset;	 // current offset of request into content address space (bytes)
    uint64_t tosend;	 // number of bytes still to send, or inf for live
    uint64_t startoff;   // MULTIFILE: starting offset of desired file in content address space, or live hook-in point
    uint64_t endoff;     // MULTIFILE: ending offset (careful, for an e.g. 100 byte interval this is 99)
    bool     closing;	 // Whether we are finishing the HTTP connection
    bool     foundH264NALU; // Raw H.264 live streaming: Whether a NALU has been found.

} http_requests[HTTPGW_MAX_REQUEST];


int http_gw_reqs_open = 0;
int http_gw_reqs_count = 0;

struct evhttp *http_gw_event; 	            // libevent HTTP request received event
struct evhttp_bound_socket *http_gw_handle; // libevent HTTP server socket handle
popt_cont_int_prot_t httpgw_cipm=POPT_CONT_INT_PROT_MERKLE;
uint64_t httpgw_livesource_disc_wnd=POPT_LIVE_DISC_WND_ALL; // Default live discard window. Copy of cmdline param
uint32_t httpgw_chunk_size = SWIFT_DEFAULT_CHUNK_SIZE;  // Default chunk size. Copy of cmdline param
double *httpgw_maxspeed = NULL;                         // Default speed limits. Copy of cmdline param
std::string httpgw_storage_dir="";			// Default storage location for swarm downloads
Address httpgw_bindaddr;				// Address of HTTP server

// Arno, 2010-11-30: for SwarmPlayer 3000 backend autoquit when no HTTP req is received
bool sawhttpconn = false;


http_gw_t *HttpGwFindRequestByEV(struct evhttp_request *evreq) {
    for (int httpc=0; httpc<http_gw_reqs_open; httpc++) {
        if (http_requests[httpc].sinkevreq==evreq)
            return &http_requests[httpc];
    }
    return NULL;
}

http_gw_t *HttpGwFindRequestByTD(int td) {
    for (int httpc=0; httpc<http_gw_reqs_open; httpc++) {
        if (http_requests[httpc].td==td) {
            return &http_requests[httpc];
        }
    }
    return NULL;
}

http_gw_t *HttpGwFindRequestBySwarmID(SwarmID &swarmid) {
    int td = swift::Find(swarmid);
    if (td < 0)
        return NULL;
    return HttpGwFindRequestByTD(td);
}


void HttpGwCloseConnection (http_gw_t* req) {
    dprintf("%s @%i http get: cleanup evreq %p\n",tintstr(),req->id, req->sinkevreq);

    struct evhttp_connection *evconn = evhttp_request_get_connection(req->sinkevreq);

    req->closing = true;

    if (!req->replied)
        evhttp_request_free(req->sinkevreq);
    else if (req->offset > req->startoff)
        evhttp_send_reply_end(req->sinkevreq); //WARNING: calls HttpGwLibeventCloseCallback
    req->sinkevreq = NULL;

    if (req->sinkevwrite != NULL)
    {
        event_free(req->sinkevwrite);
        req->sinkevwrite = NULL;
    }

    // Note: for some reason calling conn_free here prevents the last chunks
    // to be sent to the requester?
    //    evhttp_connection_free(evconn); // WARNING: calls HttpGwLibeventCloseCallback

    // Current close policy: checkpoint and DO NOT close transfer, keep on
    // seeding forever. More sophisticated clients should use CMD GW and issue
    // REMOVE.
    swift::Checkpoint(req->td);

    // Arno, 2012-05-04: MULTIFILE: once the selected file has been downloaded
    // swift will download all content that comes afterwards too. Poor man's
    // fix to avoid this: seek to end of content when HTTP done. VOD PiecePicker
    // will then no download anything. Better would be to seek to end when
    // swift partial download is done, not the serving via HTTP.
    //
    swift::Seek(req->td,swift::Size(req->td)-1,SEEK_CUR);

    // Arno, 2013-05-24: Leave swarm for LIVE.
    if (req->xcontentdur == "-1")
	swift::Close(req->td);

    *req = http_requests[--http_gw_reqs_open];
}


void HttpGwLibeventCloseCallback(struct evhttp_connection *evconn, void *evreqvoid) {
    // Called by libevent on connection close, either when the other side closes
    // or when we close (because we call evhttp_connection_free()). To prevent
    // doing cleanup twice, we see if there is a http_gw_req that has the
    // passed evreqvoid as sinkevreq. If so, clean up, if not, ignore.
    // I.e. evhttp_request * is used as sort of request ID
    //
    fprintf(stderr,"HttpGwLibeventCloseCallback: called\n");
    http_gw_t * req = HttpGwFindRequestByEV((struct evhttp_request *)evreqvoid);
    if (req == NULL)
        dprintf("%s @-1 http closecb: conn already closed\n",tintstr() );
    else {
        dprintf("%s @%i http closecb\n",tintstr(),req->id);
        if (req->closing)
            dprintf("%s @%i http closecb: already closing\n",tintstr(), req->id);
        else
            HttpGwCloseConnection(req);
    }
}




void HttpGwWrite(int td) {
    //
    // Write to HTTP socket.
    //
    http_gw_t* req = HttpGwFindRequestByTD(td);
    if (req == NULL) {
        print_error("httpgw: MayWrite: can't find req for transfer");
        return;
    }

    // When writing first data, send reply header
    //if (req->offset == req->startoff) {
    if (!req->replied) {
        // Not just for chunked encoding, see libevent2's http.c

	dprintf("%s @%d http reply 2: %d\n",tintstr(),req->id, req->replycode );

        evhttp_send_reply_start(req->sinkevreq, req->replycode, "OK");
        req->replied = true;
    }

    // SEEKTODO: stop downloading when file complete

    // Update endoff as size becomes less fuzzy
    if (swift::Size(req->td) < req->endoff)
        req->endoff = swift::Size(req->td)-1;

    // How much can we write?
    uint64_t relcomplete = swift::SeqComplete(req->td,req->startoff);
    if (relcomplete > req->endoff)
        relcomplete = req->endoff+1-req->startoff;
    int64_t avail = relcomplete-(req->offset-req->startoff);

    dprintf("%s @%d http write: avail %lld relcomp %llu offset %llu start %llu end %llu tosend %llu\n",tintstr(),req->id, avail, relcomplete, req->offset, req->startoff, req->endoff, req->tosend );

    struct evhttp_connection *evconn = evhttp_request_get_connection(req->sinkevreq);
    struct bufferevent* buffy = evhttp_connection_get_bufferevent(evconn);
    struct evbuffer *outbuf = bufferevent_get_output(buffy);

    // Arno: If sufficient data to write (avoid small increments) and out buffer
    // not filled then write to socket. Unfortunately, libevent2 has a liberal
    // understanding of socket writability that may result in tens of megabytes
    // being cached in memory. Limit that amount at app level.
    //
    if (avail > 0 && evbuffer_get_length(outbuf) < HTTPGW_MAX_OUTBUF_BYTES)
    {
        int64_t max_write_bytes = 0;
        if (swift::ttype(req->td) == FILE_TRANSFER)
            max_write_bytes = HTTPGW_VOD_MAX_WRITE_BYTES;
        else
            max_write_bytes = HTTPGW_LIVE_MAX_WRITE_BYTES;

        // Allocate buffer to read into. TODO: let swift::Read accept evb
        char *buf = (char *)malloc(max_write_bytes);

        uint64_t tosend = std::min(max_write_bytes,avail);
        size_t rd = swift::Read(req->td,buf,tosend,req->offset);
        if (rd<0) {
            print_error("httpgw: MayWrite: error pread");
            HttpGwCloseConnection(req);
            free(buf);
            return;
        }

        // Construct evbuffer and send incrementally
        struct evbuffer *evb = evbuffer_new();
        int ret = 0;

        // ARNO LIVE raw H264 hack
        if (req->mimetype == "video/h264")
        {
	    if (req->offset == req->startoff)
	    {
		// Arno, 2012-10-24: When tuning into a live stream of raw H.264
		// you must
		// 1. Replay Sequence Picture Set (SPS) and Picture Parameter Set (PPS)
		// 2. Find first NALU in video stream (starts with 00 00 00 01 and next bit is 0
		// 3. Write that first NALU
		//
		// PROBLEM is that SPS and PPS contain info on video size, frame rate,
		// and stuff, so is stream specific. The hardcoded values here are
		// for H.264 640x480 15 fps 500000 bits/s obtained via Spydroid.
		//

		const unsigned char h264sps[] = { 0x00, 0x00, 0x00, 0x01, 0x27, 0x42, 0x80, 0x29, 0x8D, 0x95, 0x01, 0x40, 0x7B, 0x20 };
		const unsigned char h264pps[] = { 0x00, 0x00, 0x00, 0x01, 0x28, 0xDE, 0x09, 0x88 };

		dprintf("%s @%i http write: adding H.264 SPS and PPS\n",tintstr(),req->id );

		ret = evbuffer_add(evb,h264sps,sizeof(h264sps));
		if (ret < 0)
		    print_error("httpgw: MayWrite: error evbuffer_add H.264 SPS");
		ret = evbuffer_add(evb,h264pps,sizeof(h264pps));
		if (ret < 0)
		    print_error("httpgw: MayWrite: error evbuffer_add H.264 PPS");
	    }
        }
	else
	    req->foundH264NALU = true; // Other MIME type

        // Find first H.264 NALU
        size_t naluoffset = 0;
        if (!req->foundH264NALU && rd >= 5)
        {
            for (int i=0; i<rd-5; i++)
            {
        	// Find startcode before NALU
        	if (buf[i] == '\x00' && buf[i+1] == '\x00' && buf[i+2] == '\x00' && buf[i+3] == '\x01')
        	{
        	    char naluhead = buf[i+4];
        	    if ((naluhead & 0x80) == 0)
        	    {
        		// Found NALU
        		// http://mailman.videolan.org/pipermail/x264-devel/2007-February/002681.html
        		naluoffset = i;
        		req->foundH264NALU = true;
        		dprintf("%s @%i http write: Found H.264 NALU at %d\n",tintstr(),req->id, naluoffset );
        		break;
        	    }
        	}
            }
        }

        // Live tuned-in or VOD:
        if (req->foundH264NALU)
        {
	    // Arno, 2012-10-24: LIVE Don't change rd here, as that should be a multiple of chunks
	    ret = evbuffer_add(evb,buf+naluoffset,rd-naluoffset);
	    if (ret < 0) {
		print_error("httpgw: MayWrite: error evbuffer_add");
		evbuffer_free(evb);
		HttpGwCloseConnection(req);
		free(buf);
		return;
	    }
        }

        if (evbuffer_get_length(evb) > 0)
	    evhttp_send_reply_chunk(req->sinkevreq, evb);

        evbuffer_free(evb);
        free(buf);

        int wn = rd;
        dprintf("%s @%i http write: sent %db\n",tintstr(),req->id,wn);
        if (http_debug)
            fprintf(stderr,"httpgw: %s @%i http write: sent %db\n",tintstr(),req->id,wn);

        req->offset += wn;
        req->tosend -= wn;

        // PPPLUG
        swift::Seek(req->td,req->offset,SEEK_CUR);
    }

    // Arno, 2010-11-30: tosend is set to fuzzy len, so need extra/other test.
    if (req->tosend==0 || req->offset == req->endoff+1) {
        // Done; wait for outbuffer to empty
        dprintf("%s @%i http write: done, wait for buffer empty\n",tintstr(),req->id);
        if (evbuffer_get_length(outbuf) == 0) {
            dprintf("%s @%i http write: final done\n",tintstr(),req->id );
            HttpGwCloseConnection(req);
        }
    }
    else {
        // wait for data
        dprintf("%s @%i http write: waiting for data\n",tintstr(),req->id);
    }
}

void HttpGwSubscribeToWrite(http_gw_t * req);

void HttpGwLibeventMayWriteCallback(evutil_socket_t fd, short events, void *evreqvoid )
{
    //
    // HTTP socket is ready to be written to.
    //
    http_gw_t * req = HttpGwFindRequestByEV((struct evhttp_request *)evreqvoid);
    if (req == NULL)
        return;

    HttpGwWrite(req->td);

    if (swift::ttype(req->td) == FILE_TRANSFER) {

        if (swift::Complete(req->td)+HTTPGW_VOD_MAX_WRITE_BYTES >= swift::Size(req->td)) {

            // We don't get progress callback for last chunk < chunk size, nor
            // when all data is already on disk. In that case, just keep on
            // subscribing to HTTP socket writability until all data is sent.
            //
            if (req->sinkevreq != NULL) // Conn closed
                HttpGwSubscribeToWrite(req);
        }
    }
}


void HttpGwSubscribeToWrite(http_gw_t * req) {
    //
    // Subscribing to writability of the socket requires libevent2 >= 2.0.17
    // (or our backported version)
    //
    struct evhttp_connection *evconn = evhttp_request_get_connection(req->sinkevreq);
    struct event_base *evbase =    evhttp_connection_get_base(evconn);
    struct bufferevent* evbufev = evhttp_connection_get_bufferevent(evconn);

    if (req->sinkevwrite != NULL)
        event_free(req->sinkevwrite); // does event_del()

    req->sinkevwrite = event_new(evbase,bufferevent_getfd(evbufev),EV_WRITE,HttpGwLibeventMayWriteCallback,req->sinkevreq);
    struct timeval t;
    t.tv_sec = 10;
    int ret = event_add(req->sinkevwrite,&t);
    //fprintf(stderr,"httpgw: HttpGwSubscribeToWrite: added event\n");
}



void HttpGwSwiftPlayingProgressCallback (int td, bin_t bin) {

    // Ready to play or playing, and subsequent HTTPGW_PROGRESS_STEP_BYTES
    // available. So subscribe to a callback when HTTP socket becomes writable
    // to write it out.

    dprintf("%s T%i http play progress\n",tintstr(),td);
    if (http_debug)
	fprintf(stderr,"httpgw: %s T%i http play progress %s\n",tintstr(),td, bin.str().c_str() );

    http_gw_t* req = HttpGwFindRequestByTD(td);
    if (req == NULL)
        return;

    if (req->sinkevreq == NULL) // Conn closed
        return;

    // LIVE
    if (req->xcontentdur == "-1")
    {
	// Check if we re-hooked-in
	uint64_t hookinoff = swift::GetHookinOffset(td);
	if (hookinoff > req->startoff) // Sort of safety catch, only forward
	{
	    req->startoff = hookinoff;
	    req->offset = hookinoff;

	    fprintf(stderr,"httpgw: Live: Re-hook-in at %llu\n", hookinoff );
	    dprintf("%s @%i http play: Re-hook-in at %llu\n",tintstr(),req->id, hookinoff );
	}
    }

    // Arno, 2011-12-20: We have new data to send, wait for HTTP socket writability
    HttpGwSubscribeToWrite(req);
}


void HttpGwSwiftPrebufferProgressCallback (int td, bin_t bin) {
    //
    // Prebuffering, and subsequent bytes of content downloaded.
    //
    // If sufficient prebuffer, next step is to subscribe to a callback for
    // writing out the reply and body.
    //
    dprintf("%s T%i http prebuf progress\n",tintstr(),td);
    if (http_debug)
	fprintf(stderr,"httpgw: %s T%i http prebuf progress %s\n",tintstr(),td, bin.str().c_str() );

    http_gw_t* req = HttpGwFindRequestByTD(td);
    if (req == NULL)
    {
        dprintf("%s T%i http prebuf progress: req not found\n",tintstr(),td);
        return;
    }

    // ARNOSMPTODO: bitrate-dependent prebuffering?

    dprintf("%s T%i http prebuf progress: endoff startoff %llu endoff %llu\n",tintstr(),td, req->startoff, req->endoff);

    int64_t wantsize = std::min(req->endoff+1-req->startoff,(uint64_t)HTTPGW_MIN_PREBUF_BYTES);

    dprintf("%s T%i http prebuf progress: want %lld got %lld\n",tintstr(),td, wantsize, swift::SeqComplete(req->td,req->startoff) );
    if (http_debug)
	fprintf(stderr,"httpgw: %s T%i http prebuf progress: want %lld got %lld\n",tintstr(),td, wantsize, swift::SeqComplete(req->td,req->startoff) );

    if (swift::SeqComplete(req->td,req->startoff) < wantsize)
    {
        // wait for more data
        return;

    }

    // First HTTPGW_MIN_PREBUF_BYTES bytes of request received.
    swift::RemoveProgressCallback(td,&HttpGwSwiftPrebufferProgressCallback);

    int stepbytes = 0;
    if (swift::ttype(td) == FILE_TRANSFER)
        stepbytes = HTTPGW_VOD_PROGRESS_STEP_BYTES;
    else
        stepbytes = HTTPGW_LIVE_PROGRESS_STEP_BYTES;
    int progresslayer = bytes2layer(stepbytes,swift::ChunkSize(td));

    swift::AddProgressCallback(td,&HttpGwSwiftPlayingProgressCallback,progresslayer);

    //
    // We have sufficient data now, see if HTTP socket is writable so we can
    // write the HTTP reply and first part of body.
    //
    HttpGwSwiftPlayingProgressCallback(td,bin_t(0,0));
}




bool HttpGwParseContentRangeHeader(http_gw_t *req,uint64_t filesize)
{
    struct evkeyvalq *reqheaders =    evhttp_request_get_input_headers(req->sinkevreq);
    struct evkeyvalq *repheaders = evhttp_request_get_output_headers(req->sinkevreq);
    const char *contentrangecstr =evhttp_find_header(reqheaders,"Range");

    if (contentrangecstr == NULL) {
        req->rangefirst = -1;
        req->rangelast = -1;
        req->replycode = 200;
        return true;
    }
    std::string range = contentrangecstr;

    // Handle RANGE query
    bool bad = false;
    int idx = range.find("=");
    if (idx == std::string::npos)
        return false;
    std::string seek = range.substr(idx+1);

    dprintf("%s @%i http get: range request spec %s\n",tintstr(),req->id, seek.c_str() );

    if (seek.find(",") != std::string::npos) {
        // - Range header contains set, not supported at the moment
        bad = true;
    } else     {
        // Determine first and last bytes of requested range
        idx = seek.find("-");

        dprintf("%s @%i http get: range request idx %d\n", tintstr(),req->id, idx );

        if (idx == std::string::npos)
            return false;
        if (idx == 0) {
            // -444 format
            req->rangefirst = -1;
        } else {
            std::istringstream(seek.substr(0,idx)) >> req->rangefirst;
        }


        dprintf("%s @%i http get: range request first %s %lld\n", tintstr(),req->id, seek.substr(0,idx).c_str(), req->rangefirst );

        if (idx == seek.length()-1)
            req->rangelast = -1;
        else {
            // 444- format
            std::istringstream(seek.substr(idx+1)) >> req->rangelast;
        }

        dprintf("%s @%i http get: range request last %s %lld\n", tintstr(),req->id, seek.substr(idx+1).c_str(), req->rangelast );

        // Check sanity of range request
        if (filesize == -1) {
            // - No length (live)
            bad = true;
        }
        else if (req->rangefirst == -1 && req->rangelast == -1) {
            // - Invalid input
            bad = true;
        }
        else if (req->rangefirst >= (int64_t)filesize) {
            bad = true;
        }
        else if (req->rangelast >= (int64_t)filesize) {
            if (req->rangefirst == -1)     {
                // If the entity is shorter than the specified
                // suffix-length, the entire entity-body is used.
                req->rangelast = filesize-1;
            }
            else
                bad = true;
        }
    }

    if (bad) {
        // Send 416 - Requested Range not satisfiable
        std::ostringstream cross;
        if (filesize == -1)
            cross << "bytes */*";
        else
            cross << "bytes */" << filesize;
        evhttp_add_header(repheaders, "Content-Range", cross.str().c_str() );

        evhttp_send_error(req->sinkevreq,416,"Malformed range specification");
	req->replied = true;

        dprintf("%s @%i http get: ERROR 416 invalid range %lld-%lld\n",tintstr(),req->id,req->rangefirst,req->rangelast );
        return false;
    }

    // Convert wildcards into actual values
    if (req->rangefirst != -1 && req->rangelast == -1) {
        // "100-" : byte 100 and further
        req->rangelast = filesize - 1;
    }
    else if (req->rangefirst == -1 && req->rangelast != -1) {
        // "-100" = last 100 bytes
        req->rangefirst = filesize - req->rangelast;
        req->rangelast = filesize - 1;
    }

    // Generate header
    std::ostringstream cross;
    cross << "bytes " << req->rangefirst << "-" << req->rangelast << "/" << filesize;
    evhttp_add_header(repheaders, "Content-Range", cross.str().c_str() );

    // Reply is sent when content is avail
    req->replycode = 206;

    dprintf("%s @%i http get: valid range %lld-%lld\n",tintstr(),req->id,req->rangefirst,req->rangelast );

    return true;
}


void HttpGwFirstProgressCallback (int td, bin_t bin) {
    //
    // First bytes of content downloaded (first in absolute sense)
    // We can now determine if a request for a file inside a multi-file
    // swarm is valid, and calculate the absolute offsets of the requested
    // content, taking into account HTTP Range: headers. Or return error.
    //
    // If valid, next step is to subscribe a new callback for prebuffering.
    //
    dprintf("%s T%i http first progress\n",tintstr(),td);
    if (http_debug)
	fprintf(stderr,"httpgw: %s T%i http first progress\n",tintstr(),td);

    // Need the first chunk
    if (swift::SeqComplete(td) == 0)
    {
        dprintf("%s T%i http first: not enough seqcomp\n",tintstr(),td );
        return;
    }

    http_gw_t* req = HttpGwFindRequestByTD(td);
    if (req == NULL)
    {
        dprintf("%s T%i http first: req not found\n",tintstr(),td);
        return;
    }

    // Protection against spurious callback
    if (req->tosend > 0)
    {
        dprintf("%s @%i http first: already set tosend\n",tintstr(),req->id);
        return;
    }

    // MULTIFILE
    // Is storage ready?
    Storage *storage = swift::GetStorage(td);
    if (storage == NULL)
    {
        dprintf("%s @%i http first: not storage object?\n",tintstr(),req->id);
        return;
    }
    if (!storage->IsReady())
    {
	dprintf("%s 2%i http first: Storage not ready, wait\n",tintstr(),req->id);
	return; // wait for some more data
    }

    /*
     * Good to go: Calculate info needed for header of HTTP reply, return
     * error if it doesn't make sense
     */
    uint64_t filesize = 0;
    if (req->mfspecname != "")
    {
        // MULTIFILE
        // Find out size of selected file
        storage_files_t sfs = storage->GetStorageFiles();
        storage_files_t::iterator iter;
        bool found = false;
        for (iter = sfs.begin(); iter < sfs.end(); iter++)
        {
            StorageFile *sf = *iter;
            if (sf->GetSpecPathName() == req->mfspecname)
            {
                found = true;
                req->startoff = sf->GetStart();
                req->endoff = sf->GetEnd();
                filesize = sf->GetSize();
                break;
            }
        }
        if (!found) {
            evhttp_send_error(req->sinkevreq,404,"Individual file not found in multi-file content.");
	    req->replied = true;
	    dprintf("%s @%i http get: ERROR 404 file %s not found in multi-file\n",tintstr(),req->id,req->mfspecname.c_str() );
            return;
        }
    }
    else
    {
        // Single file
        req->startoff = 0;
        req->endoff = swift::Size(td)-1;
        filesize = swift::Size(td);
    }

    // Handle HTTP GET Range request, i.e. additional offset within content
    // or file. Sets some headers or sends HTTP error.
    if (req->xcontentdur == "-1") //LIVE
    {
	uint64_t hookinoff = swift::GetHookinOffset(td);
	req->startoff = hookinoff;
        req->rangefirst = -1;
        req->replycode = 200;

	fprintf(stderr,"httpgw: Live: hook-in at %llu\n", hookinoff );
	dprintf("%s @%i http first: hook-in at %llu\n",tintstr(),req->id, hookinoff );
    }
    else if (!HttpGwParseContentRangeHeader(req,filesize))
        return;

    if (req->rangefirst != -1)
    {
        // Range request
        // Arno, 2012-06-15: Oops, use startoff before mod.
        req->endoff = req->startoff + req->rangelast;
        req->startoff += req->rangefirst;
        req->tosend = req->rangelast+1-req->rangefirst;
    }
    else
    {
        req->tosend = filesize;
    }
    req->offset = req->startoff;

    // SEEKTODO: concurrent requests to same resource
    if (req->startoff != 0 && req->xcontentdur != "-1")
    {
        // Seek to multifile/range start
        int ret = swift::Seek(req->td,req->startoff,SEEK_SET);
        if (ret < 0) {
            evhttp_send_error(req->sinkevreq,500,"Internal error: Cannot seek to file start in range request or multi-file content.");
            req->replied = true;
            dprintf("%s @%i http get: ERROR 500 cannot seek to %llu\n",tintstr(),req->id, req->startoff);
            return;
        }
    }

    // Prepare rest of headers. Not actually sent till HttpGwWrite
    // calls evhttp_send_reply_start()
    //
    struct evkeyvalq *reqheaders = evhttp_request_get_output_headers(req->sinkevreq);
    //evhttp_add_header(reqheaders, "Connection", "keep-alive" );
    evhttp_add_header(reqheaders, "Connection", "close" );
    evhttp_add_header(reqheaders, "Content-Type", req->mimetype.c_str() );
    if (req->xcontentdur != "-1")
    {
        if (req->xcontentdur.length() > 0)
            evhttp_add_header(reqheaders, "X-Content-Duration", req->xcontentdur.c_str() );

        // Convert size to string
        std::ostringstream closs;
        closs << req->tosend;
        evhttp_add_header(reqheaders, "Content-Length", closs.str().c_str() );
    }
    else
    {
    	// LIVE
	// Uses chunked encoding, configured by not setting a Content-Length header
        evhttp_add_header(reqheaders, "Accept-Ranges", "none" );
    }

    dprintf("%s @%i http first: headers set, tosend %lli\n",tintstr(),req->id,req->tosend);


    // Reconfigure callbacks for prebuffering
    swift::RemoveProgressCallback(td,&HttpGwFirstProgressCallback);

    int stepbytes = 0;
    if (swift::ttype(td) == FILE_TRANSFER)
        stepbytes = HTTPGW_VOD_PROGRESS_STEP_BYTES;
    else
        stepbytes = HTTPGW_LIVE_PROGRESS_STEP_BYTES;
    int progresslayer = bytes2layer(stepbytes,swift::ChunkSize(td));
    swift::AddProgressCallback(td,&HttpGwSwiftPrebufferProgressCallback,progresslayer);

    // We have some data now, see if sufficient prebuffer to go play
    // (happens when some content already on disk, or fast DL
    //
    HttpGwSwiftPrebufferProgressCallback(td,bin_t(0,0));
}




bool swift::ParseURI(std::string uri,parseduri_t &map)
{
    //
    // Arno, 2013-09-11: New Format:
    //   tswift://tracker:port/swarmid-in-hex/filename?k=v&k=v
    // where the server part, filename may be optional. Defined keys
    // (see PPSP protocol options)
    //    v = Version
    //    cp = Content Integrity Protection Method
    //    hf = Merkle Tree Hash Function
    //	  ca = Chunk Addressing Method
    //    ld = Live Discard Window (hint, normally per peer)
    //
    // Additional:
    //    cs = Chunk Size
    //    cl = Content Length
    //    cd = Content Duration (in seconds)
    //    bt = BT tracker URL
    //	  mt = MIME type
    //
    // Note that Live Signature Algorithm is part of the Swarm ID.
    //
    // Returns map with "scheme", "server", "path", "swarmidhex",
    // "filename", and an entry for each key,value pair in the query part.

    struct evhttp_uri *evu = evhttp_uri_parse(uri.c_str());
    if (evu == NULL)
	return false;

    std::string scheme="";
    const char *schemecstr = evhttp_uri_get_scheme(evu);
    if (schemecstr != NULL)
        scheme = schemecstr;

    std::ostringstream oss;
    const char *hostcstr = evhttp_uri_get_host(evu);
    if (hostcstr != NULL)
    {
        oss << hostcstr;
        oss << ":" << evhttp_uri_get_port(evu);
    }

    std::string path = evhttp_uri_get_path(evu);
    std::string swarmidhexstr="";
    std::string filename="";
    int sidx = path.find("/",1);
    if (sidx == std::string::npos)
	swarmidhexstr = path.substr(1,path.length());
    else
    {
	// multi-file
	swarmidhexstr = path.substr(1,sidx-1);
	filename = path.substr(sidx+1,path.length()-sidx);
    }

    // Put in map
    map.insert(stringpair("scheme", scheme ));
    map.insert(stringpair("server",oss.str() ));
    map.insert(stringpair("path", path ));
    // Derivatives
    map.insert(stringpair("swarmidhex",swarmidhexstr));
    map.insert(stringpair("filename",filename));

    // Query, if present
    struct evkeyvalq qheaders;
    const char *querycstr = evhttp_uri_get_query(evu);
    if (querycstr == NULL)
    {   
        for (int i=0; i<NUM_URL_QUERY_KEYS; i++)
        {
	    const char *valcstr = "";
	    map.insert(stringpair(std::string(url_query_keys[i]),std::string(valcstr) ));
        }
    }
    else
    {
        int ret = evhttp_parse_query_str(querycstr,&qheaders);
        if (ret < 0)
        {
	    evhttp_uri_free(evu);
	    return false;
        }

        // Query: TODO multiple occurrences of same key
        for (int i=0; i<NUM_URL_QUERY_KEYS; i++)
        {
	    const char *valcstr = evhttp_find_header(&qheaders,url_query_keys[i]);
	    if (valcstr == NULL)
	        valcstr = "";

            fprintf(stderr,"httpgw: ParseURI key %s val %s\n", url_query_keys[i], valcstr );

	    map.insert(stringpair(std::string(url_query_keys[i]),std::string(valcstr) ));
        }

        // Syntax check bt URL, if any
        std::string bttrackerurl = map["bt"];
        if (bttrackerurl != "")
        {
            // Handle possibly escaped "http:..."
            char *decoded = evhttp_uridecode(bttrackerurl.c_str(),false,NULL);
            std::string unesctrackerurl = decoded;
            free(decoded);

            struct evhttp_uri *evu2 = evhttp_uri_parse(unesctrackerurl.c_str());
            if (evu == NULL)
            {
        	evhttp_uri_free(evu);
        	return false;
            }
        }
    }

    evhttp_uri_free(evu);

    return true;
}



void HttpGwNewRequestCallback (struct evhttp_request *evreq, void *arg) {

    dprintf("%s @%i http get: new request\n",tintstr(),http_gw_reqs_count+1);

    if (evhttp_request_get_command(evreq) != EVHTTP_REQ_GET) {
        return;
    }
    sawhttpconn = true;

    // 1. Get swift URI
    std::string uri = evhttp_request_get_uri(evreq);
    struct evkeyvalq *reqheaders = evhttp_request_get_input_headers(evreq);

    dprintf("%s @%i http get: new request %s\n",tintstr(),http_gw_reqs_count+1,uri.c_str() );

    // Arno, 2012-04-19: libevent adds "Connection: keep-alive" to reply headers
    // if there is one in the request headers, even if a different Connection
    // reply header has already been set. And we don't do persistent conns here.
    //
    evhttp_remove_header(reqheaders,"Connection"); // Remove Connection: keep-alive

    // 2. Parse swift URI
    if (uri.length() <= 1)     {
        evhttp_send_error(evreq,400,"Path must be root hash in hex, 40 bytes.");
        dprintf("%s @%i http get: ERROR 400 Path must be root hash in hex\n",tintstr(),0 );
        return;
    }
    parseduri_t puri;
    if (!swift::ParseURI(uri,puri))
    {
        evhttp_send_error(evreq,400,"Path format is /swarmid-in-hex/filename?k1=v1&k2=v2");
        dprintf("%s @%i http get: ERROR 400 Path format violation\n",tintstr(),0 );
        return;
    }
    std::string swarmidhexstr = "", mfstr="", durstr="", chunksizestr = "", iastr="", bttrackerurl="", urlmimetype="";
    swarmidhexstr = puri["swarmidhex"];
    mfstr = puri["filename"];
    durstr = puri["cd"];
    chunksizestr = puri["cs"];
    iastr = puri["ia"];
    bttrackerurl = puri["bt"];
    urlmimetype = puri["mt"];

    // Handle MIME
    std::string mimetype = "video/mp2t";
    if (urlmimetype != "")
    {
	mimetype = urlmimetype;
    }

    uint32_t chunksize=httpgw_chunk_size; // default externally configured
    if (chunksizestr.length() > 0)
        std::istringstream(chunksizestr) >> chunksize;

    // BT tracker via URL query param
    std::string trackerurl = "";
    if (bttrackerurl != "")
    {
        // Handle possibly escaped "http:..."
        char *decoded = evhttp_uridecode(bttrackerurl.c_str(),false,NULL);
        trackerurl = decoded;
        free(decoded);
    }

    dprintf("%s @%i http get: demands %s mf %s dur %s track %s mime %s\n",tintstr(),http_gw_reqs_open+1,swarmidhexstr.c_str(),mfstr.c_str(),durstr.c_str(), trackerurl.c_str(), mimetype.c_str() );

    // Handle LIVE
    if (swarmidhexstr.length() > Sha1Hash::SIZE*2)
	durstr = "-1";

    Address srcaddr(iastr.c_str());
    if (iastr != "")
    {
	if (srcaddr == Address())
	{
	    evhttp_send_error(evreq,400,"Bad injector address in query.");
	    dprintf("%s @%i http get: ERROR 400 Bad injector address in query\n",tintstr(),0 );
	    return;
	}
    }

    // 3. Check for concurrent requests, currently not supported.
    SwarmID swarm_id = SwarmID(swarmidhexstr);
    http_gw_t *existreq = HttpGwFindRequestBySwarmID(swarm_id);
    if (existreq != NULL)
    {
        evhttp_send_error(evreq,409,"Conflict: server does not support concurrent requests to same swarm.");
        dprintf("%s @%i http get: ERROR 409 No concurrent requests to same swarm\n",tintstr(),0 );
        return;
    }

    // ANDROID
    std::string filename = "";
    if (httpgw_storage_dir == "") {
    	filename = swarmidhexstr;
    }
    else {
    	filename = httpgw_storage_dir;
    	filename += swarmidhexstr;
    }

    // 4. Initiate transfer, activating FileTransfer if needed
    bool activate=true;
    int td = swift::Find(swarm_id,activate);
    if (td == -1) {
        // LIVE
        if (durstr != "-1") {
            td = swift::Open(filename,swarm_id,trackerurl,false,httpgw_cipm,false,activate,chunksize);
        }
        else {
            td = swift::LiveOpen(filename,swarm_id,trackerurl,srcaddr,httpgw_cipm,httpgw_livesource_disc_wnd,chunksize);
        }

        // Arno, 2011-12-20: Only on new transfers, otherwise assume that CMD GW
        // controls speed
        swift::SetMaxSpeed(td,DDIR_DOWNLOAD,httpgw_maxspeed[DDIR_DOWNLOAD]);
        swift::SetMaxSpeed(td,DDIR_UPLOAD,httpgw_maxspeed[DDIR_UPLOAD]);
    }

    // 5. Record request
    http_gw_t* req = http_requests + http_gw_reqs_open++;
    req->id = ++http_gw_reqs_count;
    req->sinkevreq = evreq;

    // Replace % escaped chars to 8-bit values as part of the UTF-8 encoding
    char *decodedmf = evhttp_uridecode(mfstr.c_str(), 0, NULL);
    req->mfspecname = std::string(decodedmf);
    free(decodedmf);
    req->xcontentdur = durstr;
    req->mimetype = mimetype;
    req->offset = 0;
    req->tosend = 0;
    req->td = td;
    req->sinkevwrite = NULL;
    req->closing = false;
    req->replied = false;
    req->startoff = 0;
    req->endoff = 0;
    req->foundH264NALU = false;

    fprintf(stderr,"httpgw: Opened %s dur %s\n",swarmidhexstr.c_str(), durstr.c_str() );

    // We need delayed replying, so take ownership.
    // See http://code.google.com/p/libevent-longpolling/source/browse/trunk/main.c
    // Careful: libevent docs are broken. It doesn't say that evhttp_send_reply_send
    // actually calls evhttp_request_free, i.e. releases ownership for you.
    //
    evhttp_request_own(evreq);

    // Register callback for connection close
    struct evhttp_connection *evconn = evhttp_request_get_connection(req->sinkevreq);
    evhttp_connection_set_closecb(evconn,HttpGwLibeventCloseCallback,req->sinkevreq);

    if (swift::SeqComplete(td) > 0) {
        /*
         * Arno, 2011-10-17: Swift ProgressCallbacks are only called when
         * the data is downloaded, not when it is already on disk. So we need
         * to handle the situation where all or part of the data is already
         * on disk.
         */
        HttpGwFirstProgressCallback(td,bin_t(0,0));
    } else {
        swift::AddProgressCallback(td,&HttpGwFirstProgressCallback,HTTPGW_FIRST_PROGRESS_BYTE_INTERVAL_AS_LAYER);
    }
}

bool InstallHTTPGateway( struct event_base *evbase,Address bindaddr, popt_cont_int_prot_t cipm, uint64_t disc_wnd, uint32_t chunk_size, double *maxspeed, std::string storage_dir, int32_t vod_step, int32_t min_prebuf)
{
    // Arno, 2011-10-04: From libevent's http-server.c example

    // Arno, 2012-10-16: Made configurable for ANDROID
    httpgw_storage_dir = storage_dir;
    if (vod_step != -1)
	HTTPGW_VOD_PROGRESS_STEP_BYTES = vod_step;
    if (min_prebuf != -1)
	HTTPGW_MIN_PREBUF_BYTES = min_prebuf;

    /* Create a new evhttp object to handle requests. */
    http_gw_event = evhttp_new(evbase);
    if (!http_gw_event) {
        print_error("httpgw: evhttp_new failed");
        return false;
    }

    /* Install callback for all requests */
    evhttp_set_gencb(http_gw_event, HttpGwNewRequestCallback, NULL);

    /* Now we tell the evhttp what port to listen on */
    http_gw_handle = evhttp_bind_socket_with_handle(http_gw_event, bindaddr.ipstr().c_str(), bindaddr.port());
    if (!http_gw_handle) {
        print_error("httpgw: evhttp_bind_socket_with_handle failed");
        return false;
    }

    httpgw_cipm=cipm;
    httpgw_livesource_disc_wnd=disc_wnd;
    httpgw_chunk_size = chunk_size;
    // Arno, 2012-11-1: Make copy.
    httpgw_maxspeed = new double[2];
    for (int d=0; d<2; d++)
	httpgw_maxspeed[d] = maxspeed[d];
    httpgw_bindaddr = bindaddr;
    return true;
}


/*
 * ANDROID
 * Return progress info in "x/y" format. This is returned to the Java Activity
 * which uses it to update the progress bar. Currently x is not the number of
 * bytes downloaded, but the number of bytes written to the HTTP connection.
 */
std::string HttpGwGetProgressString(SwarmID &swarmid)
{
    std::stringstream rets;
    //rets << "httpgw: ";

    int td = swift::Find(swarmid);
    if (td==-1)
	rets << "0/0";
    else
    {
	http_gw_t* req = HttpGwFindRequestByTD(td);
	if (req == NULL)
	    rets << "0/0";
	else
	{
	    //rets << swift::SeqComplete(td);
	    rets << req->offset;
	    rets << "/";
	    rets << swift::Size(req->td);
	}
    }

    return rets.str();
}

// ANDROID
// Arno: dummy place holder
std::string HttpGwStatsGetSpeedCallback(SwarmID &swarmid)
{
    int dspeed = 0, uspeed = 0;
    uint32_t nleech=0,nseed=0;
    int statsgw_last_down=0, statsgw_last_up=0;

    int td = swift::Find(swarmid);
    if (td !=-1)
    {
	dspeed = (int)(swift::GetCurrentSpeed(td,DDIR_DOWNLOAD)/1024.0);
	uspeed = (int)(swift::GetCurrentSpeed(td,DDIR_UPLOAD)/1024.0);
	nleech = swift::GetNumLeechers(td);
	nseed = swift::GetNumSeeders(td);

	http_gw_t* req = HttpGwFindRequestByTD(td);
	if (req != NULL)
	    statsgw_last_down = req->offset;
	statsgw_last_up = swift::SeqComplete(td);
    }
    std::stringstream ss;
    ss << dspeed;
    ss << "/";
    ss << uspeed;
    ss << "/";
    ss << nleech;
    ss << "/";
    ss << nseed;
    ss << "/";
    ss << statsgw_last_down;
    ss << "/";
    ss << statsgw_last_up;

    return ss.str();
}



/** For SwarmPlayer 3000's HTTP failover. We should exit if swift isn't
 * delivering such that the extension can start talking HTTP to the backup.
 */
bool HTTPIsSending()
{
    if (http_gw_reqs_open > 0)
    {
	int td = http_requests[http_gw_reqs_open-1].td;
	fprintf(stderr,"httpgw: upload %lf\n",swift::GetCurrentSpeed(td,DDIR_UPLOAD)/1024.0);
	fprintf(stderr,"httpgw: dwload %lf\n",swift::GetCurrentSpeed(td,DDIR_DOWNLOAD)/1024.0);
    }
    return true;
}
