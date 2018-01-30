#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <semaphore.h>

#include "channels.h"

// declare global variables
#define PORT0 8000
#define PORT1 8001
#define PORT2 8002
#define PORT3 8003
#define PORT4 8004
#define PORT5 8005
#define PORT6 8006
#define PORT7 8007
#define CHANNELS 8
#define POLL_SIZE 24


int socket_fds[CHANNELS];
int client_fds[CHANNELS];
int new_fds[CHANNELS];
struct sockaddr_in serv_addr[CHANNELS];
struct sockaddr_in cli_addr[CHANNELS];
int ports[CHANNELS] = {PORT0, PORT1, PORT2, PORT3, PORT4, PORT5, PORT6, PORT7};
socklen_t clilen[CHANNELS];
struct pollfd fds[POLL_SIZE];
int isMessage[CHANNELS];

int on = 1;
int n;


/*
 * Locking mechanisms.
 */

pthread_mutex_t send_mutexes[CHANNELS];
pthread_mutex_t recv_mutexes[CHANNELS];

pthread_cond_t send_cond[CHANNELS];
pthread_cond_t recv_cond[CHANNELS];



/*
 * Set up.
 */
 
int ch_setup() {
	// must be called exactly once before using any channels
	// how is it not safe to use library? think of scenarios
	// return 0 on success or < 0 on error
    
    // if this was called previously, then the fds[0] would not be NULL
    if(fds[0].fd != 0) {
        printf("set up has been called previously\n");
        return -1;
    }
    int error, i;
    for(i = 0; i < CHANNELS; i++) {
        socket_fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        if(socket_fds[i] < 0) {
            printf("error opening server socket %d\n", i);
            return -1;
        }
    }

    for(i = 0; i < CHANNELS; i++) {
        client_fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        if(client_fds[i] < 0) {
            printf("error opening client socket %d\n", i);
            return -1;
        }
    }

    // set socket to be reusable
    for(i = 0; i < CHANNELS; i++) {
        error = setsockopt(socket_fds[i], SOL_SOCKET, SO_REUSEADDR,
                           (char *) &on, sizeof(on));
        if(error < 0) {
            printf("error on setting socket %d to reusable\n", i);
            return -1;
        }
    }

    for(i = 0; i < CHANNELS; i++) {
        bzero((char *) &serv_addr[i], sizeof(serv_addr[i]));
        serv_addr[i].sin_family = AF_INET;
        serv_addr[i].sin_addr.s_addr = INADDR_ANY;
        serv_addr[i].sin_port = htons(ports[i]);
    }

    for(i = 0; i < CHANNELS; i++) {
        if(bind(socket_fds[i], (struct sockaddr *) &serv_addr[i],
                sizeof(serv_addr[i])) < 0) {
            printf("error on binding channel %d\n", i);
            return -1;
        }
    }

    for(i = 0; i < CHANNELS; i++) {
        listen(socket_fds[i], 5);
        clilen[i] = sizeof(cli_addr[i]);
    }
    
    // set up the poll array of fds
    bzero(fds, sizeof(fds));
    for(i = 0; i < CHANNELS; i++) {
        fds[i].fd = socket_fds[i];
        fds[i].events = POLLIN;
    }

    for(i = 0; i < CHANNELS; i++) {
        if(connect(client_fds[i], (struct sockaddr *) &serv_addr[i],
                   sizeof(serv_addr[i])) < 0) {
            printf("error connecting %d\n", i);
            return -1;
        }
    }
    
    for(i = 0; i < CHANNELS; i++) {
        fds[i+16].fd = client_fds[i];
        fds[i+16].events = POLLIN;
    }
    
    for(i = 0; i < CHANNELS; i++) {
        new_fds[i] = accept(socket_fds[i], (struct sockaddr *) &cli_addr[i],
                            &clilen[i]);
        if(new_fds[i] < 0) {
            printf("error accepting on %d\n", i);
            return -1;
        }
    }
    
    for(i = 0; i < CHANNELS; i++) {
        fds[i+8].fd = new_fds[i];
        fds[i+8].events = POLLIN;
    }
    
    // initialise locking mechanisms
    for(i = 0; i < CHANNELS; i++) {
        error = pthread_mutex_init(&send_mutexes[i], NULL);
        if(error < 0) {
            printf("error initializing send mutex %d\n", i);
            return -1;
        }
        error = pthread_mutex_init(&recv_mutexes[i], NULL);
        if(error < 0) {
            printf("error initializing recv mutex %d\n", i);
            return -1;
        }
        error = pthread_cond_init(&send_cond[i], NULL);
        if(error < 0) {
            printf("error initializing send cond %d\n", i);
            return -1;
        }
        error = pthread_cond_init(&recv_cond[i], NULL);
        if(error < 0) {
            printf("error initializing recv cond %d\n", i);
            return -1;
        }
    }
    
    // initialise isMessage array to 0
    bzero(isMessage, sizeof(isMessage));
    
    
    return 0;
}

/*
 * Destroy.
 */

int ch_destroy() {
	// clean up before closing program
	// after calling this, not safe to use any channels functions
	// return 0 on success or < 0 on error
	// was ch_setup called earlier?
	// is no one currently listening on any channels?
    int error, i;
    for(i = 0; i < CHANNELS; i++) {
        error = close(new_fds[i]);
        if(error < 0) {
            return -1;
        }
        error = close(socket_fds[i]);
        if(error < 0) {
            return -1;
        }
        error = close(client_fds[i]);
        if(error < 0) {
            return -1;
        }
    }
    return 0;
}

/*
 * Send.
 */

int ch_send(int channel, void* msg) {
	// send a message
	// each channel has capacity = 1 msg
	// if no msg currently pending
		// send function must not wait for msg to be received
	// if msg already in channel that has not been delivered
		// ch_send blocks until first msg delivered
	// channel = channel #
	// msg = msg to send
	// by sending message, caller transfers ownership of msg to channel
	// messages must be on heap
	// returns 0 on success and < 0 on error
	// message CANNOT be NULL
		// is no one currently listening on any channels?
    int error;
    if(msg == NULL) {
        printf("Message was null. Fix me.\n");
        return -1;
    } else if(channel > 7) {
        printf("You've entered a nonexistent channel.\n");
        return -1;
    } else {
        error = pthread_mutex_lock(&send_mutexes[channel]);
        if(error < 0) {
            printf("error locking in send with %d\n", channel);
            return -1;
        }
        while(ch_peek(channel) == 1) {
            error = pthread_cond_wait(&send_cond[channel], &send_mutexes[channel]);
            if(error < 0) {
                printf("error waiting in send with %d\n", channel);
                return -1;
            }
        }
        // critical section
        error = write(fds[channel+8].fd, msg, sizeof(msg));
        if(error < 0) {
            perror("write");
            printf("%s\n", (char *)perror);
            printf("error writing to channel %d\n", channel);
            return -1;
        }
        isMessage[channel] = 1;
        // send signal
        error = pthread_cond_signal(&recv_cond[channel]);
        if(error < 0) {
            printf("error signaling from send on channel %d\n", channel);
            return -1;
        }
        // unlock
        error = pthread_mutex_unlock(&send_mutexes[channel]);
        if(error < 0) {
            printf("error unlocking in channel %d\n", channel);
            return -1;
        }
    }
    return 0;

}

/*
 * Receive.
 */

int ch_recv(int channel, void** dest) {
	// receive msg
	// if no msg available
		// blocks until 1 msg received
	// dest = addr of pointer where msg should be stored
	// transfers ownership of msg to receiver
	// returns 0 on success and < 0 on error
	// in case of error, *dest set to NULL
		// is no one currently listening on any channels?
    *dest = malloc(sizeof(dest));
    if(*dest < 0) {
        printf("error mallocing\n");
        return -1;
    }
    int err;
    if(channel < CHANNELS) {
        // lock
        err = pthread_mutex_lock(&recv_mutexes[channel]);
        if(err < 0) {
            printf("error locking on channel %d in recv\n", channel);
            return -1;
        }
        // wait for message
        while(ch_peek(channel) == 0) {
            err = pthread_cond_wait(&recv_cond[channel], &recv_mutexes[channel]);
            if(err < 0) {
                printf("error waiting on channel %d in recv\n", channel);
                return -1;
            }
        }
        // critical section
        n = read(fds[channel+16].fd, *dest, sizeof(dest));
        if(n < 0) {
            printf("error reading from channel %d\n", channel);
            return -1;
        }
        isMessage[channel] = 0;
        // send signal to producer
        err = pthread_cond_signal(&send_cond[channel]);
        if(err < 0) {
            printf("error signaling from recv on channel %d\n", channel);
            return -1;
        }
        // unlock
        err = pthread_mutex_unlock(&recv_mutexes[channel]);
        if(err < 0) {
            printf("error unlocking in channel %d in recv\n", channel);
            return -1;
        }
    } else {
        printf("you've entered a nonexistent channel\n");
        *dest = NULL;
        return -1;
    }
    return 0;
}

/*
 * Try to receive.
 */

int ch_tryrecv(int channel, void** dest) {
	// similar to receive, except does NOT block
	// if no msg available on this channel
		// immediately returns with value = 0 and *dest set to NULL
	// if msg retrieved
		// return 1 and *dest set to msg
		// is no one currently listening on any channels?
    // if there is no msg
    int err;
    *dest = malloc(sizeof(dest));
    if(*dest < 0) {
        printf("error mallocing\n");
        return -1;
    }
    if(ch_peek(channel) == 0) {
        *dest = NULL;
        return 0;
    // if there is a msg
    } else if(ch_peek(channel) == 1) {
        // lock
        err = pthread_mutex_lock(&recv_mutexes[channel]);
        if(err < 0) {
            printf("error locking on channel %d in try_recv\n", channel);
            return -1;
        }
        // critical section
        n = read(fds[channel+16].fd, *dest, sizeof(dest));
        if(n < 0) {
            printf("error reading from channel %d\n", channel);
            return -1;
        }
        isMessage[channel] = 0;
        // send signal to producer
        err = pthread_cond_signal(&send_cond[channel]);
        if(err < 0) {
            printf("error signaling from recv on channel %d\n", channel);
            return -1;
        }
        // unlock
        err = pthread_mutex_unlock(&recv_mutexes[channel]);
        if(err < 0) {
            printf("error unlocking in channel %d in recv\n", channel);
            return -1;
        }
        return 1;
    } else {
        printf("an error occurred in peek somewhere\n");
        return -1;
    }
	
}

/*
 * Peek.
 */

int ch_peek(int channel) {
	// check if there is currently msg pending in channel
	// channel = channel #
	// return 1 if currently pending msg, 0 if not
	// -1 in case of errors
		// is no one currently listening on any channels?
    
    int avail = 0;
    
    // double check that they entered the correct channel #
    if(channel > 7) {
        printf("You've tried to peek a nonexistent channel.\n");
        return -1;
    }
    
    // double check that there are listening sockets
    for(int i = 0; i < 8; i++) {
        if(fds[i].fd > 0) {
            avail = avail + 1;
        }
    }
    
    // if there are sockets listening
    if(avail > 0) {
        if(isMessage[channel]) {
            return 1;
        } else {
            return 0;
        }
    } else {
        printf("There are no listening sockets available.\n");
        return -1;
    }


}


 
