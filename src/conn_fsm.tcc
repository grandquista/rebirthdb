#ifndef __FSM_TCC__
#define __FSM_TCC__

#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include "utils.hpp"
#include "request_handler/txt_memcached_handler.hpp"
#include "request_handler/bin_memcached_handler.hpp"
#include "request_handler/memcached_handler.hpp"

template<class config_t>
void conn_fsm<config_t>::init_state() {
    this->state = fsm_socket_connected;
    this->rbuf = NULL;
    this->sbuf = NULL;
    this->nrbuf = 0;
    this->corked = false;
}

// This function returns the socket to clean connected state
template<class config_t>
void conn_fsm<config_t>::return_to_socket_connected() {
    if(this->rbuf)
        delete (iobuf_t*)(this->rbuf);
    if(this->sbuf)
        delete (linked_buf_t*)(this->sbuf);
    init_state();
}

// This state represents a connected socket with no outstanding
// operations. Incoming events should be user commands received by the
// socket.
template<class config_t>
typename conn_fsm<config_t>::result_t conn_fsm<config_t>::fill_rbuf(event_t *event) {
    ssize_t sz;
    conn_fsm *state = (conn_fsm*)event->state;
    assert(state == this);

    if(state->rbuf == NULL) {
        state->rbuf = (char *)new iobuf_t();
        state->nrbuf = 0;
    }
    if(state->sbuf == NULL) {
        state->sbuf = new linked_buf_t();
    }

    // TODO: we assume the command will fit comfortably into
    // IO_BUFFER_SIZE. We'll need to implement streaming later.

    sz = io_calls_t::read(state->source,
            state->rbuf + state->nrbuf,
            iobuf_t::size - state->nrbuf);
    if(sz == -1) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            // The machine can't be in
            // fsm_socket_send_incomplete state here,
            // since we break out in these cases. So it's
            // safe to free the buffer.
            //
            //TODO Modify this so that we go into send_incomplete and try to empty our send buffer
            //this is a pretty good first TODO
            if(state->state != conn_fsm::fsm_socket_recv_incomplete && nrbuf == 0)
                return_to_socket_connected();
            else
                state->state = fsm_socket_connected; //we're wating for a socket event
            //break;
        } else if (errno == ENETDOWN) {
            check("Enetdown wtf", sz == -1);
        } else {
            check("Could not read from socket", sz == -1);
        }
    } else if(sz > 0 || nrbuf > 0) {
        state->nrbuf += sz;
        get_cpu_context()->worker->bytes_read += sz;
        if (state->state != fsm_socket_recv_incomplete)
            state->state = fsm_outstanding_data;
    } else {
        if (state->state == fsm_socket_recv_incomplete)
            return fsm_no_data_in_socket;
        else
        return fsm_quit_connection;
        // TODO: what about application-level keepalive?
    }

    return fsm_transition_ok;
}

template<class config_t>
typename conn_fsm<config_t>::result_t conn_fsm<config_t>::do_fsm_btree_incomplete(event_t *event)
{
    if(event->event_type == et_sock) {
        // We're not going to process anything else from the socket
        // until we complete the currently executing command.
    } else if(event->event_type == et_request_complete) {
        send_msg_to_client();
        if(this->state != conn_fsm::fsm_socket_send_incomplete) {
            state = fsm_outstanding_data;
        }
    } else {
        fail("fsm_btree_incomplete: Invalid event");
    }
    
    return fsm_transition_ok;
}

// The socket is ready for sending more information and we were in the
// middle of an incomplete send request.
template<class config_t>
typename conn_fsm<config_t>::result_t conn_fsm<config_t>::do_socket_send_incomplete(event_t *event) {
    // TODO: incomplete send needs to be tested therally. It's not
    // clear how to get the kernel to artifically limit the send
    // buffer.
    if(event->event_type == et_sock) {
        if(event->op == eo_rdwr || event->op == eo_write) {
            send_msg_to_client();
        }
        if(this->state != conn_fsm::fsm_socket_send_incomplete) {
            state = fsm_outstanding_data;
        }
    } else {
        fail("fsm_socket_send_ready: Invalid event");
    }
    return fsm_transition_ok;
}

template<class config_t>
typename conn_fsm<config_t>::result_t conn_fsm<config_t>::do_fsm_outstanding_req(event_t *event) {
    //We've processed a request but there are still outstanding requests in our rbuf
    conn_fsm *state = (conn_fsm*)event->state;
    assert(state == this);
    if (nrbuf == 0) {
        state->state = fsm_socket_recv_incomplete;
        return fsm_transition_ok;
    }

    typename req_handler_t::parse_result_t handler_res =
        req_handler->parse_request(event);
    switch(handler_res) {
        case req_handler_t::op_malformed:
            // Command wasn't processed correctly, send error
            // Error should already be placed in buffer by parser
            send_msg_to_client();
            state->state = fsm_outstanding_data;
            break;
        case req_handler_t::op_partial_packet:
            // The data is incomplete, keep trying to read in
            // the current read loop
            state->state = conn_fsm::fsm_socket_recv_incomplete;
            break;
        case req_handler_t::op_req_shutdown:
            // Shutdown has been initiated
            return fsm_shutdown_server;
        case req_handler_t::op_req_quit:
            // The connection has been closed
            return fsm_quit_connection;
        case req_handler_t::op_req_complex:
            // Ain't nothing we can do now - the operations
            // have been distributed accross CPUs. We can just
            // sit back and wait until they come back.
            state->state = fsm_btree_incomplete;
            return fsm_transition_ok;
            break;
        case req_handler_t::op_req_parallelizable:
            state->state = fsm_outstanding_data;
            return fsm_transition_ok;
            break;
        case req_handler_t::op_req_send_now:
            send_msg_to_client();
            state->state = fsm_outstanding_data;
            return fsm_transition_ok;
        default:
            fail("Unknown request parse result");
    }
    return fsm_transition_ok;
}

// Switch on the current state and call the appropriate transition
// function.
template<class config_t>
typename conn_fsm<config_t>::result_t conn_fsm<config_t>::do_transition(event_t *event) {
    // TODO: Using parent_pool member variable within state
    // transitions might cause cache line alignment issues. Can we
    // eliminate it (perhaps by giving each thread its own private
    // copy of the necessary data)?
    result_t res;

    switch(state) {
        case fsm_socket_connected:
        case fsm_socket_recv_incomplete:
            res = fill_rbuf(event);
            break;
        case fsm_socket_send_incomplete:
            res = do_socket_send_incomplete(event);
            break;
        case fsm_btree_incomplete:
            res = do_fsm_btree_incomplete(event);
            break;
        case fsm_outstanding_data:
            res = fsm_transition_ok;
            break;
        default:
            res = fsm_invalid;
            fail("Invalid state");
    }
    if (state == fsm_outstanding_data && res != fsm_quit_connection && res != fsm_shutdown_server) {
        if (nrbuf == 0) {
            //fill up the buffer
            event->event_type = et_sock;
            res = fill_rbuf(event);
        }
        if (state != fsm_outstanding_data)
            return res;
        //there's still data in our rbuf, deal with it
        //this is awkward, but we need to make sure that we loop here until we
        //actually create a btree request
        do {
#ifdef MEMCACHED_STRICT
            bool was_corked = corked;
#endif
            res = do_fsm_outstanding_req(event);
            if (res == fsm_shutdown_server || res == fsm_quit_connection) {
                return_to_socket_connected();
                return res;
            }

            if (state == fsm_socket_recv_incomplete) {
                event->event_type = et_sock;
                res = fill_rbuf(event);
                
                if (res == fsm_no_data_in_socket)
                    return fsm_transition_ok;
            }

#ifdef MEMCACHED_STRICT
            if (was_corked && !corked)
                send_msg_to_client();
#endif
        } while (state == fsm_socket_recv_incomplete || state == fsm_outstanding_data);
    }

    return res;
}

template<class config_t>
conn_fsm<config_t>::conn_fsm(resource_t _source, event_queue_t *_event_queue)
    : source(_source), req_handler(NULL), event_queue(_event_queue)
{
    req_handler = new memcached_handler_t<config_t>(event_queue, this);
    init_state();
}

template<class config_t>
conn_fsm<config_t>::~conn_fsm() {
    close(source);
    delete req_handler;
    if(this->rbuf) {
        delete (iobuf_t*)(this->rbuf);
    }
    if(this->sbuf) {
        delete (this->sbuf);
    }
}

// Send a message to the client. The message should be contained
// within sbuf (nbuf should be the full size). If state has been
// switched to fsm_socket_send_incomplete, then buf must not be freed
// after the return of this function.
template<class config_t>
void conn_fsm<config_t>::send_msg_to_client() {
    // Either number of bytes already sent should be zero, or we
    // should be in the middle of an incomplete send.
    //assert(this->snbuf == 0 || this->state == conn_fsm::fsm_socket_send_incomplete); TODO equivalent thing for seperate buffers

    if (!this->corked) {
        int res = sbuf->send(this->source);
        if (sbuf->gc_me)
            sbuf = sbuf->garbage_collect();

        switch (res) {
            case linked_buf_t::linked_buf_outstanding:
                this->state = conn_fsm::fsm_socket_send_incomplete;
                break;
            case linked_buf_t::linked_buf_empty:
                this->state = conn_fsm::fsm_outstanding_data;
                break;
        }
    }
}

template<class config_t>
void conn_fsm<config_t>::consume(unsigned int bytes) {
    memmove(this->rbuf, this->rbuf + bytes, this->nrbuf - bytes);
    this->nrbuf -= bytes;
}

#endif // __FSM_TCC__

