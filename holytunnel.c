#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <netdb.h>

#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <sys/sysinfo.h>
#include <sys/signalfd.h>

#include "holytunnel.h"
#include "util.h"
#include "resolver.h"
#include "config.h"


typedef struct epoll_event Event;


/*
 * Client
 */
enum {
	_CLIENT_STATE_HEADER,
	_CLIENT_STATE_RESOLVER,
	_CLIENT_STATE_CONNECT,
	_CLIENT_STATE_RESPONSE,         /* HTTPS */
	_CLIENT_STATE_FORWARD_HEADER,   /* HTTP  */
	_CLIENT_STATE_FORWARD_ALL,
	_CLIENT_STATE_STOP,
};

enum {
	_CLIENT_TYPE_HTTP,
	_CLIENT_TYPE_HTTPS,
};

typedef struct client Client;
struct client {
	int          type;
	int          state;
	int          src_fd;
	int          trg_fd;
	Event        event;
	Client      *peer;
	HttpRequest  request;
	Url          url;
	size_t       sent;
	size_t       recvd;
	char         buffer[CFG_BUFFER_MAX_SIZE];
};

static const char *_client_state_str(int state);
static const char *_client_type_str(int type);


/*
 * Worker
 */
typedef struct worker {
	unsigned    index;
	atomic_int  is_alive;
	int         event_fd;
	Mempool     clients;
	Resolver   *resolver;
	thrd_t      thread;
} Worker;

static int  _worker_create(Worker *w, Resolver *resolver, unsigned index);
static void _worker_destroy(Worker *w);
static int  _worker_event_loop_thrd(void *worker);
static void _worker_handle_client_state(Worker *w, Client *client);
static int  _worker_client_add(Worker *w, int src_fd, int trg_fd, int state, Client *peer);
static void _worker_client_del(Worker *w, Client *client);
static int  _worker_client_state_header(Worker *w, Client *client);
static int  _worker_client_state_header_parse(Worker *w, Client *client);
static int  _worker_client_state_resolver(Worker *w, Client *client);
static int  _worker_client_state_connect(Worker *w, Client *client);
static int  _worker_client_state_response(Worker *w, Client *client);
static int  _worker_client_state_forward_header(Worker *w, Client *client);
static int  _worker_client_state_forward_all(Worker *w, Client *client);

static int  _worker_client_blocking_send(Worker *w, Client *client);
static void _worker_on_destroy_active_client(void *client, void *udata);


/*
 * Server
 */
typedef struct server {
	volatile int  is_alive;
	int           listen_fd;
	int           signal_fd;
	unsigned      workers_curr;
	unsigned      workers_len;
	Worker       *workers;
	Resolver      resolver;
} Server;

static int  _server_open_signal_fd(Server *s);
static int  _server_open_listen_fd(Server *s, const char lhost[], int lport);
static int  _server_create_workers(Server *s);
static void _server_destroy_workers(Server *s);
static int  _server_event_loop(Server *s);
static void _server_event_handle_listener(Server *s);
static void _server_event_handle_signal(Server *s);


/*********************************************************************************************
 * IMPL                                                                                      *
 *********************************************************************************************/
/*
 * public
 */
int
holytunnel_run(const char lhost[], int lport)
{
	int ret = -1;
	Server server;

	memset(&server, 0, sizeof(server));

	if (_server_open_listen_fd(&server, lhost, lport) < 0)
		return -1;

	if (_server_open_signal_fd(&server) < 0)
		goto out0;

	/* TODO */
	if (resolver_init(&server.resolver, CFG_RESOLVER_DEFAULT, CFG_DOH_ADGUARD) < 0)
		goto out1;

	if (_server_create_workers(&server) < 0)
		goto out2;

	log_info("holytunnel: run: listening on: \"%s:%d\"", lhost, lport);
	ret = _server_event_loop(&server);

	_server_destroy_workers(&server);

out2:
	resolver_deinit(&server.resolver);
out1:
	close(server.signal_fd);
out0:
	close(server.listen_fd);
	return ret;
}


/*
 * private
 */
/*
 * Client
 */
static const char *
_client_state_str(int state)
{
	switch (state) {
	case _CLIENT_STATE_HEADER: return "header";
	case _CLIENT_STATE_RESOLVER: return "resolver";
	case _CLIENT_STATE_CONNECT: return "connect";
	case _CLIENT_STATE_RESPONSE: return "response";
	case _CLIENT_STATE_FORWARD_HEADER: return "forward header";
	case _CLIENT_STATE_FORWARD_ALL: return "forward all";
	}

	return "unknown";
}


static const char *
_client_type_str(int type)
{
	switch (type) {
	case _CLIENT_TYPE_HTTP: return "http";
	case _CLIENT_TYPE_HTTPS: return "https";
	}

	return "unknown";
}


/*
 * Worker
 */
static int
_worker_create(Worker *w, Resolver *resolver, unsigned index)
{
	atomic_store(&w->is_alive, 0);

	const int efd = epoll_create1(0);
	if (efd < 0) {
		log_err(errno, "holytunnel: _worker_create[%u]: epoll_create1", index);
		return -1;
	}

	if (mempool_init(&w->clients, sizeof(Client), CFG_CLIENT_MIN_SIZE) < 0) {
		log_err(ENOMEM, "holytunnel: _worker_create[%u]: mempool_init", index);
		goto err0;
	}

	w->index = index;
	w->event_fd = efd;
	if (thrd_create(&w->thread, _worker_event_loop_thrd, w) != thrd_success) {
		log_err(0, "holytunnel: _worker_create[%u]: thrd_create: failed", index);
		goto err1;
	}

	w->resolver = resolver;
	return 0;

err1:
	mempool_deinit(&w->clients, NULL, NULL);
err0:
	close(efd);
	return -1;
}


static void
_worker_destroy(Worker *w)
{
	log_debug("holytunnel: _worker_destroy: [%u:%p]", w->index, (void *)w);
	atomic_store(&w->is_alive, 0);
	thrd_join(w->thread, NULL);

	close(w->event_fd);
	mempool_deinit(&w->clients, _worker_on_destroy_active_client, w);
}


static int
_worker_event_loop_thrd(void *worker)
{
	int ret = -1;
	Worker *const w = (Worker *)worker;
	Event events[CFG_EVENT_SIZE];
	const int efd = w->event_fd;


	atomic_store(&w->is_alive, 1);
	while (atomic_load_explicit(&w->is_alive, memory_order_relaxed)) {
		const int count = epoll_wait(efd, events, CFG_EVENT_SIZE, CFG_EVENT_TIMEOUT);
		if (count < 0) {
			if (errno == EINTR)
				break;

			log_err(errno, "holytunnel: _worker_event_loop_thrd[%u]: epoll_wait", w->index);
			goto out0;
		}

		for (int i = 0; i < count; i++)
			_worker_handle_client_state(w, (Client *)events[i].data.ptr);
	}

	ret = 0;

out0:
	atomic_store(&w->is_alive, 0);
	return ret;
}


static void
_worker_handle_client_state(Worker *w, Client *client)
{
	log_debug("holytunnel: _worker_handle_client_state[%u]: %p: state: %s", w->index, (void *)client,
		  _client_state_str(client->state));

	int state = client->state;
	switch (state) {
	case _CLIENT_STATE_HEADER:
		state = _worker_client_state_header(w, client);
		break;
	case _CLIENT_STATE_RESOLVER:
		state = _worker_client_state_resolver(w, client);
		break;
	case _CLIENT_STATE_CONNECT:
		state = _worker_client_state_connect(w, client);
		break;
	case _CLIENT_STATE_RESPONSE:
		state = _worker_client_state_response(w, client);
		break;
	case _CLIENT_STATE_FORWARD_HEADER:
		state = _worker_client_state_forward_header(w, client);
		break;
	case _CLIENT_STATE_FORWARD_ALL:
		state = _worker_client_state_forward_all(w, client);
		break;
	}

	if (state == _CLIENT_STATE_STOP)
		_worker_client_del(w, client);

	client->state = state;
}


static int
_worker_client_add(Worker *w, int src_fd, int trg_fd, int state, Client *peer)
{
	log_debug("holytunnel: _worker_client_add[%u]: new client: fd: %d", w->index, src_fd);

	Client *const client = mempool_alloc(&w->clients);
	if (client == NULL) {
		log_err(ENOMEM, "holytunnel: _worker_client_add[%u]: mempool_alloc", w->index);
		return -1;
	}

	log_debug("holytunnel: _worker_client_add[%u]: new client: %p", w->index, (void *)client);

	client->event.events = EPOLLIN;
	client->event.data.ptr = client;
	if (epoll_ctl(w->event_fd, EPOLL_CTL_ADD, src_fd, &client->event) < 0) {
		log_err(ENOMEM, "holytunnel: _worker_client_add[%u]: epoll_ctl: add", w->index);
		mempool_free(&w->clients, client);
		return -1;
	}

	client->type = _CLIENT_TYPE_HTTP;
	client->state = state;
	client->src_fd = src_fd;
	client->trg_fd = trg_fd;
	client->url.host = NULL;
	client->url.port = NULL;
	client->sent = 0;
	client->recvd = 0;
	client->peer = peer;
	return 0;
}


static void
_worker_client_del(Worker *w, Client *client)
{
	log_debug("holytunnel: _worker_client_del[%u]: client: %p", w->index, (void *)client);

	Client *const peer = client->peer;
	if (peer != NULL) {
		peer->event.events = EPOLLIN | EPOLLOUT;
		if (epoll_ctl(w->event_fd, EPOLL_CTL_MOD, peer->src_fd, &peer->event) < 0) {
			log_err(errno, "holytunnel: _worker_client_del[%u]: epoll_ctl: mod: peer", w->index);

			/* TODO */
			abort();
		}

		peer->state = _CLIENT_STATE_STOP;
		peer->peer = NULL;
	}

	if (epoll_ctl(w->event_fd, EPOLL_CTL_DEL, client->src_fd, &client->event) < 0) {
		log_err(errno, "holytunnel: _worker_client_del[%u]: epoll_ctl: del", w->index);

		/* TODO */
		abort();
	}

	close(client->src_fd);
	url_free(&client->url);
	mempool_free(&w->clients, client);
}


static int
_worker_client_state_header(Worker *w, Client *client)
{
	return _CLIENT_STATE_STOP;
}


static int
_worker_client_state_header_parse(Worker *w, Client *client)
{
	return _CLIENT_STATE_STOP;
}


static int
_worker_client_state_resolver(Worker *w, Client *client)
{
	return _CLIENT_STATE_STOP;
}


static int
_worker_client_state_connect(Worker *w, Client *client)
{
	return _CLIENT_STATE_STOP;
}


static int
_worker_client_state_response(Worker *w, Client *client)
{
	return _CLIENT_STATE_STOP;
}


static int
_worker_client_state_forward_header(Worker *w, Client *client)
{
	return _CLIENT_STATE_STOP;
}


static int
_worker_client_state_forward_all(Worker *w, Client *client)
{
	return _CLIENT_STATE_STOP;
}


static void
_worker_on_destroy_active_client(void *client, void *udata)
{
	Worker *const w = (Worker *)udata;
	Client *const c = (Client *)client;
	log_debug("holytunnel: _worker_on_destroy_active_client[%u]: [%p: %d]", w->index, client,
		  c->src_fd);

	close(c->src_fd);
	url_free(&c->url);
}


/*
 * Server
 */
static int
_server_open_signal_fd(Server *s)
{
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGQUIT);
	sigaddset(&mask, SIGHUP);

	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
		log_err(errno, "holytunnel: _server_open_signal_fd: sigprocmask");
		return -1;
	}

	const int fd = signalfd(-1, &mask, 0);
	if (fd < 0) {
		log_err(errno, "holytunnel: _server_open_signal_fd: signalfd");
		return -1;
	}

	s->signal_fd = fd;
	return 0;
}


static int
_server_open_listen_fd(Server *s, const char lhost[], int lport)
{
	const struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons((in_port_t)lport),
		.sin_addr.s_addr = inet_addr(lhost),
	};

	const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
	if (fd < 0) {
		log_err(errno, "holytunnel: _server_open_listen_fd: socket");
		return -1;
	}

	const int y = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &y, sizeof(y)) < 0) {
		log_err(errno, "holytunnel: _server_open_listen_fd: setsockopt: SO_REUSEADDR");
		goto err0;
	}

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		log_err(errno, "holytunnel: _server_open_listen_fd: bind");
		goto err0;
	}

	if (listen(fd, 32) < 0) {
		log_err(errno, "holytunnel: _server_open_listen_fd: listen");
		goto err0;
	}

	s->listen_fd = fd;
	return 0;

err0:
	close(fd);
	return -1;
}


static int
_server_create_workers(Server *s)
{
	const int nprocs = get_nprocs();
	assert(nprocs > 0);

	const unsigned _nprocs = (unsigned)nprocs;
	Worker *const workers = malloc(sizeof(Worker) * _nprocs);
	if (workers == NULL) {
		log_err(errno, "holytunnel: _server_create_workers: malloc: workers");
		return -1;
	}

	unsigned i = 0;
	for (; i < _nprocs; i++) {
		if (_worker_create(&workers[i], &s->resolver, i) < 0)
			goto err0;
	}

	s->workers_curr = 0;
	s->workers_len = _nprocs;
	s->workers = workers;

	/* TODO:
	 * - Carefully wait worker threads.
	 * - Ignore dead (error) worker thread, at least there is 1 worker thread alive.
	 */
	log_debug("holytunnel: _server_create_workers: nprocs: %u", _nprocs);
	for (unsigned j = 0; j < i;) {
		if (atomic_load(&s->workers[j].is_alive)) {
			log_debug("holytunnel: _server_create_workers: wait: [%u:%p]: OK", j, &s->workers[j]);
			j++;
		}

		usleep(10000);
	}

	log_debug("holytunnel: _server_create_workers: OK");
	return 0;

err0:
	while (i--)
		_worker_destroy(&workers[i]);

	free(workers);
	return -1;
}



static void
_server_destroy_workers(Server *s)
{
	for (unsigned i = 0; i < s->workers_len; i++)
		_worker_destroy(&s->workers[i]);

	free(s->workers);
	s->workers = NULL;
}


static int
_server_event_loop(Server *s)
{
	int ret = -1;
	const int lfd = s->listen_fd;
	const int sfd = s->signal_fd;
	struct pollfd pfds[2] = {
		{ .events = POLLIN, .fd = lfd },
		{ .events = POLLIN, .fd = sfd },
	};

	s->is_alive = 1;
	while (s->is_alive) {
		const int count = poll(pfds, 2, -1);
		if (count < 0) {
			if (errno == EINTR)
				break;

			log_err(errno, "holytunnel: _server_event_loop: poll");
			goto out0;
		}

		short int rv = pfds[0].revents;
		if (rv & (POLLERR | POLLHUP)) {
			log_err(0, "holytunnel: _server_event_loop: POLLERR/POLLHUP: listen fd");
			goto out0;
		}

		if (rv & POLLIN)
			_server_event_handle_listener(s);

		rv = pfds[1].revents;
		if (rv & (POLLERR | POLLHUP)) {
			log_err(0, "holytunnel: _server_event_loop: POLLERR/POLLHUP: signal fd");
			goto out0;
		}

		if (rv & POLLIN)
			_server_event_handle_signal(s);
	}

	ret = 0;

out0:
	s->is_alive = 0;
	return ret;
}


static void
_server_event_handle_listener(Server *s)
{
	const int fd = accept(s->listen_fd, NULL, NULL);
	if (fd < 0) {
		log_err(errno, "holytunnel: _server_event_handle_signal: accept");
		return;
	}

	const unsigned curr = s->workers_curr;
	Worker *const worker = &s->workers[curr];
	if (_worker_client_add(worker, fd, -1, _CLIENT_STATE_HEADER, NULL) < 0) {
		/* TODO: use another worker thread */
		close(fd);
		return;
	}

	s->workers_curr = (curr + 1) % s->workers_len;
}


static void
_server_event_handle_signal(Server *s)
{
	struct signalfd_siginfo siginfo;
	if (read(s->signal_fd, &siginfo, sizeof(siginfo)) <= 0) {
		log_err(errno, "holytunnel: _server_event_handle_signal: read");
		return;
	}

	switch (siginfo.ssi_signo) {
	case SIGHUP:
		break;
	case SIGINT:
	case SIGQUIT:
		s->is_alive = 0;
		putchar('\n');
		log_info("holytunnel: _server_event_handle_signal[%u]: interrupted", siginfo.ssi_signo);
		break;
	default:
		log_err(errno, "holytunnel: _server_event_handle_signal: invalid signal");
		abort();
	}
}
